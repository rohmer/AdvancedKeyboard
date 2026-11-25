#include "ngram_trie/autocorrect.h"
#include "ngram_trie/model.h"
#include "ngram_trie/prefix.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

using ngram_trie::AutocorrectResult;
using ngram_trie::AutocorrectScratch;
using ngram_trie::Model;
using ngram_trie::PrefixScratch;

enum Outcome { Good, Bad, Ugly, KeptCorrect, Missed };

struct Score {
    std::int64_t points = 0;
    std::array<std::size_t, 5> counts{};

    void Add(std::string_view expected, std::string_view typed, std::string_view committed)
    {
        constexpr std::array<int, 5> values{10, -25, -100, 1, 0};
        const Outcome outcome = typed == expected ? (committed == typed ? KeptCorrect : Ugly) :
            committed == expected ? Good : committed == typed ? Missed : Bad;
        ++counts[outcome];
        points += values[outcome];
    }
};

struct Regression {
    double threshold;
    Score expected;
    Score actual;
};

struct Word {
    std::size_t pair, position;
    bool starts_sentence;
    std::string expected, typed;
};

struct Candidate {
    std::string word;
    double probability = 0;
};

void Require(bool condition, std::string_view message)
{
    if (!condition) throw std::runtime_error(std::string(message));
}

template <typename Number>
Number NumberFrom(std::string_view field)
{
    Number value{};
    const auto end = field.data() + field.size();
    const auto parsed = std::from_chars(field.data(), end, value);
    Require(parsed.ec == std::errc{} && parsed.ptr == end, "invalid numeric field");
    return value;
}

unsigned Nibble(char c)
{
    if (c >= '0' && c <= '9') return static_cast<unsigned>(c - '0');
    if (c >= 'A' && c <= 'F') return static_cast<unsigned>(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return static_cast<unsigned>(c - 'a' + 10);
    throw std::runtime_error("invalid UTF-8 hex field");
}

std::string DecodeHex(std::string_view hex)
{
    Require(hex.size() % 2 == 0, "incomplete UTF-8 hex byte");
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
        bytes.push_back(static_cast<char>(16 * Nibble(hex[i]) + Nibble(hex[i + 1])));
    return bytes;
}

std::vector<std::string_view> Fields(std::string_view line)
{
    std::vector<std::string_view> fields;
    for (std::size_t end = line.find('\t'); end != std::string_view::npos; end = line.find('\t')) {
        fields.push_back(line.substr(0, end));
        line.remove_prefix(end + 1);
    }
    fields.push_back(line);
    return fields;
}

// The TSV carries folded bytes. Replay the all-caps probe through the firmware
// case folding entry point to exercise the normalization used before prediction.
std::string Fold(std::string_view word)
{
    std::array<char, AutocorrectScratch::kMaxFoldedTokenBytes> buffer;
    std::string_view folded;
    Require(ngram_trie::FoldUtf8(word, buffer.data(), buffer.size(), folded), "invalid UTF-8 or oversize probe");
    return std::string(folded);
}

void CheckCorrection(const Model& model, AutocorrectScratch& scratch, const std::vector<std::string_view>& fields)
{
    Require(fields.size() == 7 && fields[5].empty() == fields[6].empty(), "invalid A row");
    const std::string typed = DecodeHex(fields[2]), previous = DecodeHex(fields[3]), older = DecodeHex(fields[4]);
    AutocorrectResult result;
    const bool found = ngram_trie::BestAutocorrectionUtf8(model, scratch, typed, result, previous, older);
    const std::string expected = DecodeHex(fields[5]);
    Require(found == !expected.empty() && (!found || expected == result.word),
            std::string(fields[1]) + " correction word differs");
    if (found) {
        const double probability = NumberFrom<double>(fields[6]);
        Require(std::fabs(result.probability - probability) <= 5e-12,
                std::string(fields[1]) + " correction probability differs");
    }
}

void CheckPrefix(const Model& model, PrefixScratch& scratch, const std::vector<std::string_view>& fields)
{
    Require(fields.size() >= 5 && fields.size() <= 5 + ngram_trie::kSuggestionCount, "invalid P row");
    const std::string folded_prefix = DecodeHex(fields[2]);
    std::string prefix = folded_prefix;
    if (fields[1] == "all-caps") {
        for (char& c : prefix)
            if (c >= 'a' && c <= 'z') c = static_cast<char>('A' + (c - 'a'));
    }
    prefix = Fold(prefix);
    Require(prefix == folded_prefix, "prefix folding differs");
    const std::string previous = DecodeHex(fields[3]), older = DecodeHex(fields[4]);
    std::array<ngram_trie::Suggestion, ngram_trie::kSuggestionCount> suggestions;
    const auto count = ngram_trie::Suggest(model, scratch, prefix, previous, older, suggestions.data());
    Require(count == fields.size() - 5, std::string(fields[1]) + " prefix result count differs");
    for (std::size_t i = 0; i < count; ++i)
        Require(suggestions[i].Word() == DecodeHex(fields[5 + i]), std::string(fields[1]) + " prefix result order differs");
}

Regression ReadRegression(const std::vector<std::string_view>& fields, std::size_t total_words)
{
    Require(fields.size() == 9 && NumberFrom<std::size_t>(fields[2]) == total_words, "invalid R row");
    Regression value{};
    value.threshold = NumberFrom<double>(fields[1]);
    Require(value.threshold >= 0 && value.threshold <= 1, "threshold outside 0-1");
    value.expected.points = NumberFrom<std::int64_t>(fields[3]);
    for (std::size_t i = 0; i < value.expected.counts.size(); ++i)
        value.expected.counts[i] = NumberFrom<std::size_t>(fields[4 + i]);
    return value;
}

Word ReadWord(const std::vector<std::string_view>& fields, std::size_t pairs, const std::vector<Word>& words)
{
    Require(fields.size() == 6 && (fields[3] == "0" || fields[3] == "1"), "invalid W row");
    Word word{NumberFrom<std::size_t>(fields[1]), NumberFrom<std::size_t>(fields[2]), fields[3] == "1",
              DecodeHex(fields[4]), DecodeHex(fields[5])};
    Require(word.pair > 0 && word.pair <= pairs, "word has invalid pair number");
    if (words.empty() || word.pair > words.back().pair) Require(word.position == 1, "pair must start at word 1");
    else Require(word.pair == words.back().pair && word.position == words.back().position + 1, "words are out of order");
    return word;
}

void RunRegression(const Model& model, AutocorrectScratch& scratch, const std::vector<Word>& words,
                   std::vector<Regression>& runs)
{
    for (auto first = words.begin(); first != words.end();) {
        auto last = first;
        while (last != words.end() && last->pair == first->pair) ++last;

        // Reset context for each threshold. Like GBUTest, cache the search per
        // pair only, keyed by the typed word and both committed context words.
        using Context = std::tuple<std::string_view, std::string_view, std::string_view>;
        std::map<Context, Candidate> candidates;
        for (auto& run : runs) {
            std::string_view previous, older;
            for (auto word = first; word != last; ++word) {
                if (word->starts_sentence) previous = older = {};
                const auto [it, inserted] = candidates.try_emplace({word->typed, previous, older});
                Candidate& candidate = it->second;
                if (inserted) {
                    AutocorrectResult result;
                    if (ngram_trie::BestAutocorrection(model, scratch, word->typed, result, previous, older))
                        candidate = {std::string(result.word), result.probability};
                }
                const std::string_view committed = !candidate.word.empty() && candidate.probability >= run.threshold
                    ? candidate.word : word->typed;
                run.actual.Add(word->expected, word->typed, committed);
                older = previous;
                previous = committed;
            }
        }
        first = last;
    }
}

std::vector<char> ReadBinary(const char* path)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), std::string("cannot open ") + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool Verify(const Model& model, const char* path, std::size_t model_bytes)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.is_open(), "cannot open reference TSV");
    std::string line;
    auto read_line = [&] {
        if (!std::getline(input, line)) return false;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return true;
    };
    Require(read_line() && line == "# keyboard-parity-v1", "unsupported reference TSV format");
    Require(read_line(), "missing metadata");
    const auto metadata = Fields(line);
    Require(metadata.size() == 8 && metadata[0] == "M" && NumberFrom<std::size_t>(metadata[2]) == model_bytes &&
            NumberFrom<unsigned>(metadata[3]) == model.FormatVersion() && NumberFrom<std::uint32_t>(metadata[4]) == model.NodeCount(),
            "metadata does not match the loaded model");
    const auto pairs = NumberFrom<std::size_t>(metadata[6]);
    const auto total_words = NumberFrom<std::size_t>(metadata[7]);

    std::vector<Word> words;
    std::vector<Regression> runs;
    AutocorrectScratch correction_scratch;
    PrefixScratch prefix_scratch;
    std::size_t corrections = 0, prefixes = 0, line_number = 2;
    while (read_line()) {
        ++line_number;
        const auto fields = Fields(line);
        try {
            if (fields[0] == "A") {
                CheckCorrection(model, correction_scratch, fields);
                ++corrections;
            } else if (fields[0] == "P") {
                CheckPrefix(model, prefix_scratch, fields);
                ++prefixes;
            } else if (fields[0] == "R") {
                const auto run = ReadRegression(fields, total_words);
                Require(runs.empty() || runs.back().threshold < run.threshold, "thresholds are out of order");
                runs.push_back(run);
            } else if (fields[0] == "W") {
                words.push_back(ReadWord(fields, pairs, words));
            } else {
                throw std::runtime_error("unknown reference row");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("TSV line " + std::to_string(line_number) + " " + error.what());
        }
    }
    Require(input.eof() && corrections != 0 && prefixes != 0 && !runs.empty() && words.size() == total_words,
            "missing reference data");

    std::cout << corrections << " correction queries and " << prefixes << " prefix queries matched.\n";
    std::cout << "Replaying " << total_words << " words from " << pairs << " pairs at " << runs.size() << " thresholds.\n" << std::flush;
    RunRegression(model, correction_scratch, words, runs);

    bool matched = true;
    std::cout << "threshold\tscore\tgood\tbad\tugly\tkept\tmissed\tparity\n";
    for (const auto& run : runs) {
        const bool same = run.actual.points == run.expected.points && run.actual.counts == run.expected.counts;
        matched &= same;
        std::cout << run.threshold << '\t' << run.actual.points;
        for (auto count : run.actual.counts) std::cout << '\t' << count;
        std::cout << '\t' << (same ? "match" : "DIFFERS") << '\n';
    }
    return matched;
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc != 4) {
        std::cerr << "Usage: host_parity <model.ngtr> <model.ngix> <reference.tsv>\n";
        return 2;
    }
    try {
        const auto trie = ReadBinary(argv[1]), index = ReadBinary(argv[2]);
        Model model;
        Require(model.Open(trie.data(), trie.size(), index.data(), index.size()), "invalid NGTR model or NGIX index");
        return Verify(model, argv[3], trie.size()) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "host_parity: " << error.what() << '\n';
        return 1;
    }
}
