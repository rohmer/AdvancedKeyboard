// Native port of ESPOS tools/NgramTrie; see PROVENANCE.md.
#include "Generator.h"
#include "TrainingModel.h"
#include "RadixModelBuilder.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <set>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace LanguageModels {
namespace {
namespace fs = std::filesystem;
using Count = std::uint64_t;
constexpr Count maximumCount = (std::numeric_limits<Count>::max)();
constexpr std::size_t maximumWordBytes = 16u * 1024u * 1024u;
struct Range { std::uint32_t first, last; };
struct Pair { std::uint32_t scalar, value; };
struct Decomposition { std::uint32_t scalar, first, second; };
struct Composition { std::uint64_t key; std::uint32_t value; };
#include "UnicodeTables.inc"

template<class T, std::size_t Size> const T* Scalar(const T (&data)[Size], std::uint32_t value)
{
    const auto* found = std::lower_bound(std::begin(data), std::end(data), value,
        [](const T& item, std::uint32_t key) { return item.scalar < key; });
    return found != std::end(data) && found->scalar == value ? found : nullptr;
}
template<std::size_t Size> bool Inside(const Range (&data)[Size], std::uint32_t value)
{
    const auto* found = std::upper_bound(std::begin(data), std::end(data), value,
        [](std::uint32_t key, const Range& item) { return key < item.first; });
    return found != std::begin(data) && value <= (found - 1)->last;
}
bool Word(std::uint32_t value) {
    if (value < 0x80) return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9');
    return Inside(kWordRanges, value);
}
bool Mark(std::uint32_t value) { return value >= 0x80 && Inside(kMarkRanges, value); }
bool ScriptCorpus(const std::string& language) {
    return language == "ar" || language == "hi" || language == "zh" || language == "bn" ||
        language == "id" || language == "ur" || language == "ru";
}
bool Han(std::uint32_t value) {
    return (value >= 0x3400 && value <= 0x4dbf) || (value >= 0x4e00 && value <= 0x9fff) ||
        (value >= 0xf900 && value <= 0xfaff) || (value >= 0x20000 && value <= 0x2ee5f) ||
        (value >= 0x2f800 && value <= 0x2fa1f) || (value >= 0x30000 && value <= 0x323af);
}
bool Joiner(std::uint32_t value, const std::string& language) {
    return (language == "ar" || language == "hi" || language == "bn" || language == "ur") && (value == 0x200c || value == 0x200d);
}
bool SentenceReset(std::uint32_t value, const std::string& language) {
    if (value == '.' || value == '\n' || value == '\r') return true;
    return ScriptCorpus(language) && (value == 0x61f || value == 0x6d4 || value == 0x964 || value == 0x965 ||
        value == 0x3002 || value == 0xff01 || value == 0xff1f);
}
bool Space(std::uint32_t value) { return value < 0x80 ? value == ' ' || (value >= '\t' && value <= '\r') : Inside(kSpaceRanges, value); }
unsigned Combining(std::uint32_t value) { if (value < 0x80) return 0; const auto* row = Scalar(kCombining, value); return row ? row->value : 0; }
void Append(std::string& text, std::uint32_t scalar)
{
    if (scalar < 0x80) text.push_back(static_cast<char>(scalar));
    else if (scalar < 0x800) {
        text.push_back(static_cast<char>(0xc0 | (scalar >> 6))); text.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else if (scalar < 0x10000) {
        text.push_back(static_cast<char>(0xe0 | (scalar >> 12))); text.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63))); text.push_back(static_cast<char>(0x80 | (scalar & 63)));
    } else {
        text.push_back(static_cast<char>(0xf0 | (scalar >> 18))); text.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 63)));
        text.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63))); text.push_back(static_cast<char>(0x80 | (scalar & 63)));
    }
}
std::string Encode(const std::vector<std::uint32_t>& text)
{
    std::string output; output.reserve(text.size()); for (auto scalar : text) Append(output, scalar); return output;
}
std::vector<std::uint32_t> Fold(const std::vector<std::uint32_t>& text)
{
    std::vector<std::uint32_t> result; result.reserve(text.size());
    for (auto scalar : text) {
        if (scalar >= 'A' && scalar <= 'Z') result.push_back(scalar + 32);
        else if (scalar < 0x80) result.push_back(scalar);
        else if (const auto* row = Scalar(kFold, scalar))
            result.insert(result.end(), kFoldScalars + row->first, kFoldScalars + row->first + row->second);
        else result.push_back(scalar);
    }
    return result;
}
void Decompose(std::uint32_t scalar, std::vector<std::uint32_t>& output)
{
    if (scalar < 0x80) { output.push_back(scalar); return; }
    constexpr std::uint32_t SBase = 0xac00, LBase = 0x1100, VBase = 0x1161, TBase = 0x11a7;
    if (scalar >= SBase && scalar < SBase + 11172) {
        const auto offset = scalar - SBase;
        output.push_back(LBase + offset / 588); output.push_back(VBase + offset % 588 / 28);
        if (offset % 28) output.push_back(TBase + offset % 28);
    } else if (const auto* row = Scalar(kDecompositions, scalar)) {
        Decompose(row->first, output); if (row->second) Decompose(row->second, output);
    } else output.push_back(scalar);
}
std::uint32_t Compose(std::uint32_t first, std::uint32_t second)
{
    if (first >= 0x1100 && first < 0x1113 && second >= 0x1161 && second < 0x1176)
        return 0xac00 + ((first - 0x1100) * 21 + second - 0x1161) * 28;
    if (first >= 0xac00 && first < 0xd7a4 && (first - 0xac00) % 28 == 0 && second > 0x11a7 && second < 0x11c3)
        return first + second - 0x11a7;
    const auto key = (std::uint64_t(first) << 21) | second;
    const auto* row = std::lower_bound(std::begin(kCompositions), std::end(kCompositions), key,
        [](const Composition& item, std::uint64_t value) { return item.key < value; });
    return row != std::end(kCompositions) && row->key == key ? row->value : 0;
}
std::vector<std::uint32_t> Nfc(const std::vector<std::uint32_t>& input)
{
    if (std::all_of(input.begin(), input.end(), [](std::uint32_t scalar) { return scalar < 0x80; })) return input;
    std::vector<std::uint32_t> decomposed; decomposed.reserve(input.size());
    for (auto scalar : input) Decompose(scalar, decomposed);
    // Stable canonical ordering; only the combining sequence is reordered.
    for (std::size_t i = 1; i < decomposed.size(); ++i) {
        const auto current = decomposed[i]; const auto rank = Combining(current);
        if (!rank) continue;
        auto cursor = i;
        while (cursor && Combining(decomposed[cursor - 1]) > rank) { decomposed[cursor] = decomposed[cursor - 1]; --cursor; }
        decomposed[cursor] = current;
    }
    if (decomposed.empty()) return {};
    std::vector<std::uint32_t> result; result.reserve(decomposed.size()); result.push_back(decomposed[0]);
    std::size_t starter = 0; unsigned previous = Combining(decomposed[0]);
    for (std::size_t index = 1; index < decomposed.size(); ++index) {
        const auto scalar = decomposed[index]; const auto rank = Combining(scalar);
        const auto composite = (previous == 0 || previous < rank) ? Compose(result[starter], scalar) : 0;
        if (composite) result[starter] = composite;
        else { if (!rank) starter = result.size(); result.push_back(scalar); previous = rank; }
    }
    return result;
}
std::vector<std::uint32_t> Prepare(const std::vector<std::uint32_t>& text, const std::string& language)
{
    if (ScriptCorpus(language)) return Nfc(Fold(text));
    if (language == "fr" || language == "de" || language == "el") return Nfc(text);
    auto folded = Fold(text); return language == "es" ? Nfc(folded) : folded;
}
std::string NormalizeListed(std::vector<std::uint32_t> text, const std::string& language)
{
    const bool finalSigma = language == "el" && text.size() > 1 && text.back() == 0x3a3;
    for (auto& scalar : text) {
        if (scalar < 0x80) { if (scalar >= 'A' && scalar <= 'Z') scalar += 32; }
        else if (const auto* row = Scalar(kLower, scalar)) scalar = row->value;
        if (language == "fr" && scalar == 0x2019) scalar = '\'';
    }
    if (finalSigma) text.back() = 0x3c2;
    return Encode(language == "el" ? Nfc(text) : text);
}

struct Hash {
    std::uint64_t first = 14695981039346656037ull, second = 1099511628211ull;
    void Add(const void* source, std::size_t size) {
        const auto* bytes = static_cast<const unsigned char*>(source);
        for (std::size_t i = 0; i < size; ++i) { first = (first ^ bytes[i]) * 1099511628211ull; second = (second ^ (bytes[i] + 1u)) * 14029467366897019727ull; }
    }
    void Add(const std::string& value) { Add(value.data(), value.size()); }
    std::string Text() const { std::ostringstream text; text.imbue(std::locale::classic()); text << std::hex << std::setfill('0') << std::setw(16) << first << std::setw(16) << second; return text.str(); }
    bool operator==(const Hash& other) const { return first == other.first && second == other.second; }
};
class Control {
    ProgressCallback callback; const std::atomic_bool* cancellation; std::mutex gate;
    std::string previous; unsigned percent = 0;
    std::mutex streamGate; std::condition_variable streamsAvailable;
    std::size_t streams = 0, merges = 0;
public:
    std::atomic<std::uint64_t> bytes{0}, files{0}; std::atomic_bool interrupted{false}; std::uint64_t totalBytes = 0, totalFiles = 0;
    Control(ProgressCallback progress, const std::atomic_bool* cancelled) : callback(std::move(progress)), cancellation(cancelled) {}
    void Check() const { if (interrupted.load(std::memory_order_relaxed) || (cancellation && cancellation->load(std::memory_order_relaxed))) throw Cancelled(); }
    void Report(const std::string& stage, unsigned value, bool cancellable = true) {
        if (cancellable) Check(); std::lock_guard<std::mutex> lock(gate); value = (std::max)(value, percent);
        if (callback && (stage != previous || value != percent)) callback({stage, value, files.load(), totalFiles, bytes.load()});
        percent = value; previous = stage;
    }
    void CountProgress(std::uint64_t amount) {
        const auto completed = bytes.fetch_add(amount) + amount;
        Report("Counting n-grams", totalBytes ? static_cast<unsigned>((std::min)(70.0, 70.0 * completed / totalBytes)) : 70);
    }
    void AcquireStreams(std::size_t amount, bool merging) {
        // MSVC defaults to 512 FILE slots. Reserve only a bounded fraction for
        // worker I/O, leaving room for the editor and other library consumers.
        std::unique_lock<std::mutex> lock(streamGate);
        while (streams + amount > 192 || (merging && merges >= 4)) {
            Check(); streamsAvailable.wait_for(lock, std::chrono::milliseconds(25));
        }
        Check(); streams += amount; if (merging) ++merges;
    }
    void ReleaseStreams(std::size_t amount, bool merging) {
        { std::lock_guard<std::mutex> lock(streamGate); streams -= amount; if (merging) --merges; }
        streamsAvailable.notify_all();
    }
};
class StreamPermit {
    Control& control; const std::size_t amount; const bool merging;
public:
    StreamPermit(Control& owner, std::size_t count, bool merge = false) : control(owner), amount(count), merging(merge) { control.AcquireStreams(amount, merging); }
    ~StreamPermit() { control.ReleaseStreams(amount, merging); }
    StreamPermit(const StreamPermit&) = delete;
    StreamPermit& operator=(const StreamPermit&) = delete;
};
struct Signature { std::uint64_t size = 0; fs::file_time_type modified{}; };
Signature SignatureFor(const fs::path& path) { return {fs::file_size(path), fs::last_write_time(path)}; }
bool SameSignature(const Signature& first, const Signature& second) { return first.size == second.size && first.modified == second.modified; }
Hash FileHash(const fs::path& path, Control& control)
{
    std::ifstream file(path, std::ios::binary); if (!file) throw std::runtime_error("Cannot open " + path.u8string());
    Hash result; char buffer[65536];
    while (file) { control.Check(); file.read(buffer, sizeof buffer); result.Add(buffer, static_cast<std::size_t>(file.gcount())); }
    if (!file.eof()) throw std::runtime_error("Cannot read " + path.u8string());
    return result;
}
struct TextFormat { unsigned encoding = 8; bool big = false; std::uint64_t skip = 0; };
TextFormat FormatFor(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("Cannot open " + path.u8string());
    unsigned char prefix[4]{}; input.read(reinterpret_cast<char*>(prefix), sizeof prefix); const auto size = input.gcount();
    if (input.bad() || (!input && !input.eof())) throw std::runtime_error("Cannot read " + path.u8string());
    if (size >= 4 && prefix[0] == 0xff && prefix[1] == 0xfe && !prefix[2] && !prefix[3]) return {32, false, 4};
    if (size >= 4 && !prefix[0] && !prefix[1] && prefix[2] == 0xfe && prefix[3] == 0xff) return {32, true, 4};
    if (size >= 2 && prefix[0] == 0xff && prefix[1] == 0xfe) return {16, false, 2};
    if (size >= 2 && prefix[0] == 0xfe && prefix[1] == 0xff) return {16, true, 2};
    return {8, false, size >= 3 && prefix[0] == 0xef && prefix[1] == 0xbb && prefix[2] == 0xbf ? 3u : 0u};
}
struct TextRange { std::uint64_t begin = 0, end = 0; TextFormat format; };
std::vector<TextRange> Partition(const fs::path& path, std::uint64_t size, std::uint64_t target, Control& control)
{
    const auto format = FormatFor(path);
    if (format.encoding != 8 || size <= target) return {{0, size, format}};
    std::ifstream input(path, std::ios::binary); if (!input) throw std::runtime_error("Cannot open " + path.u8string());
    std::vector<TextRange> ranges; std::uint64_t begin = 0;
    constexpr std::uint64_t scanLimit = 1024u * 1024u;
    std::array<char, 65536> bytes{};
    while (size - begin > target) {
        control.Check(); const auto probe = begin + target;
        input.clear(); input.seekg(static_cast<std::streamoff>(probe));
        if (!input) throw std::runtime_error("Cannot seek corpus: " + path.u8string());
        auto offset = probe; const auto limit = probe + (std::min)(scanLimit, size - probe);
        std::uint64_t boundary = 0;
        while (offset < limit && !boundary) {
            control.Check(); const auto amount = static_cast<std::streamsize>((std::min)(std::uint64_t(bytes.size()), limit - offset));
            input.read(bytes.data(), amount);
            if (input.gcount() != amount) throw std::runtime_error("Cannot read corpus boundary: " + path.u8string());
            for (std::streamsize i = 0; i < amount; ++i) if (bytes[static_cast<std::size_t>(i)] == '.' || bytes[static_cast<std::size_t>(i)] == '\r' || bytes[static_cast<std::size_t>(i)] == '\n') { boundary = offset + std::uint64_t(i) + 1; break; }
            offset += static_cast<std::uint64_t>(amount);
        }
        // A long sentence is left intact: spaces alone do not reset n-grams.
        if (!boundary) break;
        ranges.push_back({begin, boundary, format}); begin = boundary;
    }
    ranges.push_back({begin, size, format}); return ranges;
}
// The decoder is strict, BOM-aware and bounded even for a single very long line.
class TextReader {
    std::ifstream file; fs::path path; unsigned encoding = 8; bool big = false;
    std::array<unsigned char, 65536> buffer{}; std::size_t used = 0, available = 0; Control* control;
    std::uint64_t consumed = 0, reported = 0, end = (std::numeric_limits<std::uint64_t>::max)(); bool countProgress = false;
    int Byte() {
        if (used == available) {
            control->Check(); if (consumed == end) return -1;
            const auto amount = static_cast<std::streamsize>((std::min)(std::uint64_t(buffer.size()), end - consumed));
            file.read(reinterpret_cast<char*>(buffer.data()), amount); available = static_cast<std::size_t>(file.gcount()); used = 0;
            if (!available) { if (!file.eof()) throw std::runtime_error("Cannot read " + path.u8string()); return -1; }
        }
        ++consumed; if (countProgress && consumed - reported >= 65536) { control->CountProgress(consumed - reported); reported = consumed; }
        return buffer[used++];
    }
    std::uint32_t Unit(unsigned bytes) {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < bytes; ++i) {
            const auto value = Byte(); if (value < 0) Invalid();
            result |= static_cast<std::uint32_t>(value) << (8 * (big ? bytes - i - 1 : i));
        }
        return result;
    }
    [[noreturn]] void Invalid() const { throw std::runtime_error("Cannot decode " + path.u8string() + ": invalid Unicode text."); }
public:
    TextReader(const fs::path& source, Control& owner, bool report = false, const TextRange* range = nullptr) : file(source, std::ios::binary), path(source), control(&owner), countProgress(report) {
        if (!file) throw std::runtime_error("Cannot open " + source.u8string());
        const auto format = range ? range->format : FormatFor(source); encoding = format.encoding; big = format.big;
        if (range) { consumed = range->begin; end = range->end; reported = consumed; }
        // Only the first range owns the initial BOM. A nonzero UTF-8 start must
        // never reinterpret an embedded BOM or payload bytes as a new encoding.
        if (!consumed) consumed = format.skip;
        if (consumed > end) throw std::runtime_error("Invalid corpus range: " + source.u8string());
        file.seekg(static_cast<std::streamoff>(consumed)); if (!file) throw std::runtime_error("Cannot seek " + source.u8string());
    }
    bool Next(std::uint32_t& scalar) {
        const auto first = Byte();
        if (first < 0) { if (countProgress && consumed != reported) { control->CountProgress(consumed - reported); reported = consumed; } return false; }
        if (encoding == 8) {
            if (first < 0x80) { scalar = first; return true; }
            if (first < 0xc2 || first > 0xf4) Invalid();
            const unsigned width = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4; scalar = first & (0x7f >> width);
            for (unsigned i = 1; i < width; ++i) { const auto part = Byte(); if (part < 0 || (part & 0xc0) != 0x80) Invalid(); scalar = (scalar << 6) | (part & 63); }
            if (scalar < (width == 2 ? 0x80u : width == 3 ? 0x800u : 0x10000u)) Invalid();
        } else {
            const auto second = Byte(); if (second < 0) Invalid();
            scalar = big ? (static_cast<unsigned>(first) << 8) | second : first | (static_cast<unsigned>(second) << 8);
            if (encoding == 32) {
                const auto tail = Unit(2); scalar = big ? (scalar << 16) | tail : scalar | (tail << 16);
            } else if (scalar >= 0xd800 && scalar <= 0xdbff) {
                const auto low = Unit(2); if (low < 0xdc00 || low > 0xdfff) Invalid();
                scalar = 0x10000 + ((scalar - 0xd800) << 10) + low - 0xdc00;
            }
        }
        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) Invalid();
        return true;
    }
};

struct WordList {
    std::unordered_set<std::string> words; std::string language; Hash hash;
    bool listed = false, enabled = false;
    explicit WordList(const Options& options, Control& control) {
        language = options.wordListPath.empty() || ScriptCorpus(options.language) ? options.language : "en";
        listed = language == "fr" || language == "de" || language == "el" || ScriptCorpus(language);
        if (!options.useWordList || (ScriptCorpus(language) && options.wordListPath.empty())) return;
        enabled = true;
        auto path = options.wordListPath;
        if (path.empty()) {
            const auto filename = language == "en" ? "words_alpha.txt" : "words_" + language + "_top150000.txt";
            path = options.wordListDirectory / filename;
        }
        hash = FileHash(path, control); hash.Add(language);
        TextReader reader(path, control); std::vector<std::uint32_t> line; std::uint32_t scalar;
        auto accept = [&] {
            auto start = line.begin(), finish = line.end();
            while (start != finish && Space(*start)) ++start; while (finish != start && Space(*(finish - 1))) --finish;
            if (start != finish && *start != '#') {
                if (std::any_of(start, finish, Space)) throw std::runtime_error("Use one word per line in " + path.u8string());
                std::vector<std::uint32_t> word(start, finish);
                words.insert(ScriptCorpus(language) ? NormalizeListed(Prepare(word, language), language) : Encode(listed ? word : Fold(word)));
            }
            line.clear();
        };
        while (reader.Next(scalar)) { if (scalar == '\n' || scalar == '\r') accept(); else { if (line.size() > maximumWordBytes) throw std::runtime_error("Word list entry is too long."); line.push_back(scalar); } }
        accept(); if (words.empty()) throw std::runtime_error("No words found in " + path.u8string());
    }
    std::uint64_t EstimatedBytes() const {
        std::uint64_t result = words.bucket_count() * sizeof(void*);
        for (const auto& word : words) result += 128 + word.capacity();
        return result;
    }
};
using Counts = std::unordered_map<std::string, Count>;
struct Entry { std::string key; Count count = 0; };
bool Frequency(const Entry& left, const Entry& right) { return left.count != right.count ? left.count > right.count : left.key < right.key; }
std::atomic<std::uint64_t> temporarySequence{0};
std::string Unique() { return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(temporarySequence.fetch_add(1)); }
class Scratch {
    std::atomic<std::uint64_t> next{0};
public:
    fs::path directory;
    explicit Scratch(const fs::path& base) {
        const auto parent = fs::absolute(base).lexically_normal();
        fs::create_directories(parent);
        directory = parent / (".ngram-work-" + Unique());
        if (directory.parent_path() != parent || !fs::create_directory(directory)) throw std::runtime_error("Cannot create language model workspace.");
    }
    ~Scratch() { std::error_code error; fs::remove_all(directory, error); }
    fs::path File() { return directory / (std::to_string(next.fetch_add(1)) + ".counts"); }
};
void U32(std::ostream& output, std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) output.put(static_cast<char>(value >> (8 * i))); }
void U64(std::ostream& output, std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) output.put(static_cast<char>(value >> (8 * i))); }
std::uint32_t Read32(std::istream& input) { std::uint32_t value = 0; for (unsigned i = 0; i < 4; ++i) { const auto next = input.get(); if (next == EOF) throw std::runtime_error("Truncated n-gram count file."); value |= std::uint32_t(next) << (8 * i); } return value; }
std::uint64_t Read64(std::istream& input) { std::uint64_t value = 0; for (unsigned i = 0; i < 8; ++i) { const auto next = input.get(); if (next == EOF) throw std::runtime_error("Truncated n-gram count file."); value |= std::uint64_t(next) << (8 * i); } return value; }
void WriteEntry(std::ostream& output, const Entry& entry) {
    if (entry.key.size() > (std::numeric_limits<std::uint32_t>::max)()) throw std::runtime_error("N-gram is too long.");
    unsigned char header[12]; const auto size = static_cast<std::uint32_t>(entry.key.size());
    for (unsigned i = 0; i < 4; ++i) header[i] = static_cast<unsigned char>(size >> (8 * i));
    for (unsigned i = 0; i < 8; ++i) header[i + 4] = static_cast<unsigned char>(entry.count >> (8 * i));
    output.write(reinterpret_cast<const char*>(header), sizeof header);
    output.write(entry.key.data(), static_cast<std::streamsize>(entry.key.size()));
}
bool ReadEntry(std::istream& input, Entry& entry) {
    unsigned char header[12]; input.read(reinterpret_cast<char*>(header), sizeof header);
    if (!input.gcount() && input.eof() && !input.bad()) return false;
    if (input.gcount() != sizeof header || !input) throw std::runtime_error("Truncated n-gram count file.");
    std::uint32_t size = 0; entry.count = 0;
    for (unsigned i = 0; i < 4; ++i) size |= std::uint32_t(header[i]) << (8 * i);
    for (unsigned i = 0; i < 8; ++i) entry.count |= std::uint64_t(header[i + 4]) << (8 * i);
    if (!size || size > maximumWordBytes * 3 + 3 || !entry.count) throw std::runtime_error("Invalid n-gram count record.");
    entry.key.resize(size); input.read(&entry.key[0], size); if (!input || static_cast<unsigned char>(entry.key[0]) > 2) throw std::runtime_error("Invalid n-gram count data."); return true;
}
void Finish(std::ofstream& output, const fs::path& path) { output.flush(); if (!output) throw std::runtime_error("Cannot write " + path.u8string()); output.close(); if (!output) throw std::runtime_error("Cannot close " + path.u8string()); }
void Replace(const fs::path& from, const fs::path& to) {
#ifdef _WIN32
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace " + to.u8string() + ": Windows error " + std::to_string(GetLastError()));
#else
    fs::rename(from, to);
#endif
}
template<class Write> void AtomicWrite(const fs::path& path, Control& control, Write write) {
    const auto temporary = path.parent_path() / (".ngram-output-" + Unique() + ".tmp");
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc); if (!output) throw std::runtime_error("Cannot create output in " + path.parent_path().u8string());
        write(output); Finish(output, temporary); control.Check(); Replace(temporary, path);
    } catch (...) { std::error_code error; fs::remove(temporary, error); throw; }
}

// Stage both files and check cancellation before touching an existing pair.
// The two renames are not a filesystem transaction, but a failed second rename
// restores the original sidecar. Never observe cancellation between them.
void WriteModelPair(const fs::path& triePath, const fs::path& indexPath,
                    const espos::keyboard::prediction::RadixEncodedModel& model,
                    Control& control, unsigned percent) {
    const auto tag = Unique();
    const auto trieTemporary = triePath.parent_path() / (".ngram-output-" + tag + "-trie.tmp");
    const auto indexTemporary = indexPath.parent_path() / (".ngram-output-" + tag + "-index.tmp");
    const auto backup = indexPath.parent_path() / (".ngram-backup-" + tag + ".tmp");
    bool hasBackup = false, indexChanged = false;
    const auto cleanup = [&](bool removeBackup) {
        std::error_code ignored;
        fs::remove(trieTemporary, ignored); fs::remove(indexTemporary, ignored);
        if (removeBackup) fs::remove(backup, ignored);
    };
    try {
        const auto stage = [&](const fs::path& path, const std::vector<std::uint8_t>& bytes) {
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            if (!stream) throw std::runtime_error("Cannot create model output in " + path.parent_path().u8string());
            stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            Finish(stream, path); control.Check();
        };
        stage(trieTemporary, model.trie); stage(indexTemporary, model.index);
        if (fs::exists(indexPath)) {
            std::error_code error; fs::create_hard_link(indexPath, backup, error);
            if (error) fs::copy_file(indexPath, backup);
            hasBackup = true;
        }
        control.Report("Publishing model and index", percent); control.Check();
        Replace(indexTemporary, indexPath); indexChanged = true;
        Replace(trieTemporary, triePath);
        cleanup(true);
    } catch (...) {
        const auto failure = std::current_exception();
        if (indexChanged) {
            try {
                if (hasBackup) Replace(backup, indexPath);
                else fs::remove(indexPath);
            } catch (...) {
                cleanup(false);
                throw std::runtime_error("Could not restore the previous model index; recovery copy: " + backup.u8string());
            }
        }
        cleanup(true); std::rethrow_exception(failure);
    }
}

fs::path WriteCounts(Counts& counts, Scratch& scratch, Control& control) {
    control.Check(); const auto path = scratch.File(); std::vector<Entry> entries; entries.reserve(counts.size());
    // unordered_map keys are const; extracting nodes makes these actual moves.
    while (!counts.empty()) { auto node = counts.extract(counts.begin()); entries.push_back({std::move(node.key()), node.mapped()}); }
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.key < right.key; });
    std::ofstream output(path, std::ios::binary); if (!output) throw std::runtime_error("Cannot create count workspace.");
    std::size_t written = 0; for (const auto& entry : entries) { if (!(written++ & 1023)) control.Check(); WriteEntry(output, entry); }
    Finish(output, path); return path;
}
// Hot-path keys contain word IDs instead of separately allocated concatenated
// strings. IDs are local to a bounded chunk; materialization happens once per
// distinct spilled n-gram, retaining the existing portable count-cache codec.
struct WordGram {
    std::array<std::uint32_t, 3> words{};
    std::uint8_t order = 0;
    bool operator==(const WordGram& other) const noexcept { return order == other.order && words == other.words; }
};
struct WordGramHash {
    std::size_t operator()(const WordGram& gram) const noexcept {
        std::uint64_t value = gram.order + 0x9e3779b97f4a7c15ull;
        for (const auto word : gram.words) value ^= word + 0x9e3779b97f4a7c15ull + (value << 6) + (value >> 2);
        return static_cast<std::size_t>(value ^ (value >> 32));
    }
};
class WordCounts {
    static constexpr auto missing = (std::numeric_limits<std::uint32_t>::max)();
    std::unordered_map<std::string, std::uint32_t> vocabulary;
    std::vector<const std::string*> words;
    std::unordered_map<WordGram, Count, WordGramHash> counts;
    std::uint32_t previous = missing, older = missing;
    std::uint64_t wordBytes = 0, materializedBytes = 0;
    const std::uint64_t budget;
    const std::size_t entryLimit;
    Scratch& scratch;
    Control& control;
    std::vector<fs::path>& chunks;
    std::uint32_t Intern(std::string text) {
        const auto found = vocabulary.find(text);
        if (found != vocabulary.end()) return found->second;
        if (words.size() >= missing) throw std::overflow_error("Too many words in one count chunk.");
        const auto id = static_cast<std::uint32_t>(words.size());
        const auto inserted = vocabulary.emplace(std::move(text), id).first;
        wordBytes += 128 + inserted->first.capacity(); words.push_back(&inserted->first);
        return id;
    }
    void Add(const WordGram& key) {
        const auto inserted = counts.emplace(key, 0);
        if (inserted.second) {
            materializedBytes += key.order;
            for (unsigned i = 0; i < key.order; ++i) materializedBytes += words[key.words[i]]->size();
        }
        if (inserted.first->second == maximumCount) throw std::overflow_error("N-gram count overflow.");
        ++inserted.first->second;
    }
    std::uint64_t EstimatedBytes() const {
        return wordBytes + materializedBytes + counts.size() * 96ull + words.capacity() * sizeof(void*) +
            (vocabulary.bucket_count() + counts.bucket_count()) * sizeof(void*);
    }
public:
    WordCounts(std::uint64_t bytes, std::size_t limit, Scratch& work, Control& owner, std::vector<fs::path>& parts)
        : budget(bytes), entryLimit(limit), scratch(work), control(owner), chunks(parts) {}
    void Reset() noexcept { previous = older = missing; }
    void Token(std::string text) {
        if (text.size() > budget / 32) throw std::runtime_error("A corpus token exceeds the per-worker memory budget.");
        const auto current = Intern(std::move(text));
        Add({{current, 0, 0}, 1});
        if (previous != missing) Add({{previous, current, 0}, 2});
        if (older != missing) Add({{older, previous, current}, 3});
        older = previous; previous = current;
        if (counts.size() >= entryLimit || EstimatedBytes() >= budget / 2) Flush();
    }
    void Flush() {
        if (counts.empty()) return;
        control.Check(); const auto path = scratch.File(); std::vector<Entry> entries; entries.reserve(counts.size());
        while (!counts.empty()) {
            const auto node = counts.extract(counts.begin()); const auto& key = node.key();
            Entry entry{std::string(1, static_cast<char>(key.order - 1)), node.mapped()};
            for (unsigned i = 0; i < key.order; ++i) { if (i) entry.key += ' '; entry.key += *words[key.words[i]]; }
            entries.push_back(std::move(entry));
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.key < right.key; });
        std::ofstream output(path, std::ios::binary); if (!output) throw std::runtime_error("Cannot create count workspace.");
        std::size_t written = 0; for (const auto& entry : entries) { if (!(written++ & 1023)) control.Check(); WriteEntry(output, entry); }
        Finish(output, path); chunks.push_back(path);
        const std::string before = older == missing ? std::string{} : *words[older];
        const std::string last = previous == missing ? std::string{} : *words[previous];
        vocabulary.clear(); words.clear(); wordBytes = materializedBytes = 0;
        older = before.empty() ? missing : Intern(before); previous = last.empty() ? missing : Intern(last);
    }
};
fs::path MergeGroup(const std::vector<fs::path>& inputs, bool ranked, Scratch& scratch, Control& control) {
    StreamPermit permit(control, inputs.size() + 1, true);
    struct Reader { std::ifstream stream; Entry current; explicit Reader(const fs::path& path) : stream(path, std::ios::binary) { if (!stream) { const auto code = errno; throw std::runtime_error("Cannot open count workspace '" + path.u8string() + "': " + std::error_code(code, std::generic_category()).message() + " (errno " + std::to_string(code) + ")."); } } };
    std::vector<std::unique_ptr<Reader>> readers; readers.reserve(inputs.size());
    for (const auto& path : inputs) readers.push_back(std::make_unique<Reader>(path));
    auto later = [&](std::size_t left, std::size_t right) { return ranked ? Frequency(readers[right]->current, readers[left]->current) : readers[left]->current.key > readers[right]->current.key; };
    std::priority_queue<std::size_t, std::vector<std::size_t>, decltype(later)> queue(later);
    for (std::size_t i = 0; i < readers.size(); ++i) if (ReadEntry(readers[i]->stream, readers[i]->current)) queue.push(i);
    const auto result = scratch.File(); std::ofstream output(result, std::ios::binary); if (!output) throw std::runtime_error("Cannot create merged counts.");
    Entry pending; bool hasPending = false; std::uint64_t records = 0;
    while (!queue.empty()) {
        if (!(records++ & 1023)) control.Check();
        const auto index = queue.top(); queue.pop(); const auto& current = readers[index]->current;
        if (!ranked && hasPending && pending.key == current.key) { if (current.count > maximumCount - pending.count) throw std::overflow_error("N-gram count overflow."); pending.count += current.count; }
        else { if (hasPending) WriteEntry(output, pending); pending = current; hasPending = true; }
        if (ReadEntry(readers[index]->stream, readers[index]->current)) queue.push(index);
    }
    if (hasPending) WriteEntry(output, pending); Finish(output, result); return result;
}
fs::path Merge(std::vector<fs::path> paths, bool ranked, Scratch& scratch, Control& control) {
    if (paths.empty()) { StreamPermit permit(control, 1); Counts empty; return WriteCounts(empty, scratch, control); }
    while (paths.size() > 1) {
        std::vector<fs::path> next;
        for (std::size_t i = 0; i < paths.size(); i += 32) {
            const auto end = (std::min)(paths.size(), i + 32);
            next.push_back(MergeGroup({paths.begin() + i, paths.begin() + end}, ranked, scratch, control));
            // MergeGroup has closed all readers before these owned runs retire.
            for (auto input = i; input < end; ++input) { control.Check(); fs::remove(paths[input]); }
        }
        paths = std::move(next);
    }
    return paths[0];
}

template<class Work> void RunWorkers(std::size_t count, unsigned requested, Control& control, Work work)
{
    if (!count) return;
    const auto workers = (std::min)(std::size_t(requested), count);
    if (workers == 1) { for (std::size_t i = 0; i < count; ++i) { control.Check(); work(i); } return; }
    std::atomic<std::size_t> next{0}; std::exception_ptr failure; std::mutex failureGate;
    auto worker = [&] {
        try {
            for (;;) { control.Check(); const auto index = next.fetch_add(1, std::memory_order_relaxed); if (index >= count) break; work(index); }
        } catch (...) {
            std::lock_guard<std::mutex> lock(failureGate); if (!failure) failure = std::current_exception();
            control.interrupted.store(true, std::memory_order_relaxed);
        }
    };
    std::vector<std::thread> threads; threads.reserve(workers);
    try { for (std::size_t i = 0; i < workers; ++i) threads.emplace_back(worker); }
    catch (...) { control.interrupted.store(true, std::memory_order_relaxed); for (auto& thread : threads) thread.join(); throw; }
    for (auto& thread : threads) thread.join(); if (failure) std::rethrow_exception(failure); control.Check();
}

void CountFile(const fs::path& path, const WordList& list, WordCounts& counts, Control& control, const TextRange* range = nullptr) {
    TextReader input(path, control, true, range); std::vector<std::uint32_t> candidate; bool asciiCandidate = true;
    auto tokenText = [&](std::string text) {
        if (list.enabled && list.words.find(text) == list.words.end()) { counts.Reset(); return; }
        counts.Token(std::move(text));
    };
    auto process = [&] {
        if (candidate.empty()) return;
        if (asciiCandidate) {
            // ASCII normalization is lowercase-only in every supported language.
            // Scan the same apostrophe grammar without allocating scalar slices.
            for (std::size_t index = 0; index < candidate.size();) {
                if (!Word(candidate[index])) { ++index; continue; }
                const auto start = index++;
                while (index < candidate.size()) {
                    if (Word(candidate[index])) { ++index; continue; }
                    if (candidate[index] == '\'' && index + 1 < candidate.size() && Word(candidate[index + 1])) { index += 2; continue; }
                    break;
                }
                std::string text; text.reserve(index - start);
                for (auto i = start; i < index; ++i) { auto scalar = candidate[i]; if (scalar >= 'A' && scalar <= 'Z') scalar += 32; text.push_back(static_cast<char>(scalar)); }
                tokenText(std::move(text));
            }
        } else {
            const auto text = list.enabled || ScriptCorpus(list.language) ? Prepare(candidate, list.language) : Fold(candidate);
            for (std::size_t index = 0; index < text.size();) {
                if (!Word(text[index])) { ++index; continue; }
                const auto start = index++;
                while (index < text.size()) {
                    if (Word(text[index]) || (list.listed && Mark(text[index])) || Joiner(text[index], list.language)) { ++index; continue; }
                    if ((text[index] == '\'' || text[index] == 0x2019) && index + 1 < text.size() && Word(text[index + 1])) { index += 2; continue; }
                    break;
                }
                const std::vector<std::uint32_t> word{text.begin() + start, text.begin() + index};
                tokenText(list.listed ? NormalizeListed(word, list.language) : Encode(word));
            }
        }
        candidate.clear(); asciiCandidate = true;
    };
    std::uint32_t scalar;
    while (input.Next(scalar)) {
        if (list.language == "zh" && Han(scalar)) {
            process();
            const auto normalized = Prepare({scalar}, list.language);
            tokenText(Encode(normalized));
        } else if (Word(scalar) || Mark(scalar) || scalar == '\'' || scalar == 0x2019 || (Joiner(scalar, list.language) && !candidate.empty())) {
            if (candidate.size() >= maximumWordBytes / 4) throw std::runtime_error("Corpus word exceeds the 16 MiB token safety limit.");
            candidate.push_back(scalar); asciiCandidate = asciiCandidate && scalar < 0x80;
        } else {
            process(); if (SentenceReset(scalar, list.language)) counts.Reset();
        }
    }
    process();
}

constexpr char cacheMagic[] = "LMCACHE4";
fs::path ReadCache(const fs::path& path, const Hash& identity, Scratch& scratch, Control& control) {
    try {
        std::ifstream input(path, std::ios::binary); if (!input) return {};
        char magic[8]; input.read(magic, 8); if (!input || std::memcmp(magic, cacheMagic, 8)) return {};
        Hash signature; signature.first = Read64(input); signature.second = Read64(input); if (!(signature == identity)) return {};
        const auto size = Read64(input); const auto fileSize = fs::file_size(path); if (fileSize < 48 || size != fileSize - 48) return {};
        const auto data = scratch.File(); std::ofstream output(data, std::ios::binary); if (!output) throw std::runtime_error("Cannot extract cached counts.");
        Hash checksum; std::uint64_t left = size; char buffer[65536];
        while (left) { control.Check(); const auto amount = static_cast<std::streamsize>((std::min)(left, std::uint64_t(sizeof buffer))); input.read(buffer, amount); if (!input) return {}; output.write(buffer, amount); checksum.Add(buffer, static_cast<std::size_t>(amount)); left -= amount; }
        Hash expected; expected.first = Read64(input); expected.second = Read64(input); if (!(checksum == expected) || input.peek() != EOF) return {};
        Finish(output, data);
        // Validate the internal records before trusting or merging cached bytes.
        std::ifstream records(data, std::ios::binary); Entry entry; std::string previous; bool first = true;
        while (ReadEntry(records, entry)) { control.Check(); if (!first && entry.key <= previous) return {}; first = false; previous = entry.key; }
        return data;
    } catch (const Cancelled&) { throw; } catch (const std::exception&) { return {}; }
}
void SaveCache(const fs::path& destination, const fs::path& data, const Hash& identity, Control& control) {
    AtomicWrite(destination, control, [&](std::ostream& output) {
        output.write(cacheMagic, 8); U64(output, identity.first); U64(output, identity.second); U64(output, fs::file_size(data));
        std::ifstream input(data, std::ios::binary); if (!input) throw std::runtime_error("Cannot read generated count cache.");
        Hash checksum; char buffer[65536];
        while (input) { control.Check(); input.read(buffer, sizeof buffer); const auto amount = static_cast<std::size_t>(input.gcount()); output.write(buffer, static_cast<std::streamsize>(amount)); checksum.Add(buffer, amount); }
        if (!input.eof()) throw std::runtime_error("Cannot read generated count cache."); U64(output, checksum.first); U64(output, checksum.second);
    });
}

struct TrainingArtifact {
    fs::path combined;
    std::array<fs::path, 3> ranked;
    std::array<std::uint64_t, 3> sizes{};
};
// Ranked artifacts are immutable. A small atomic manifest publishes all four
// files together; readers validate their lengths/content and borrow them
// directly instead of extracting, merging and sorting the counts again.
TrainingArtifact LoadTraining(const fs::path& directory, const Hash& identity, Control& control) {
    try {
        std::ifstream input(directory / (identity.Text() + ".training"), std::ios::binary);
        char magic[8]; input.read(magic, 8); if (!input || std::memcmp(magic, "LMTRAIN1", 8)) return {};
        Hash actual; actual.first = Read64(input); actual.second = Read64(input); if (!(actual == identity)) return {};
        const auto length = Read32(input); if (!length || length > 128) return {};
        std::string tag(length, '\0'); input.read(tag.data(), length);
        if (!input || tag.find_first_not_of("0123456789-") != std::string::npos) return {};
        TrainingArtifact result;
        std::array<fs::path*, 4> paths{&result.combined, &result.ranked[0], &result.ranked[1], &result.ranked[2]};
        for (unsigned i = 0; i < paths.size(); ++i) {
            const auto size = Read64(input); Hash expected; expected.first = Read64(input); expected.second = Read64(input);
            *paths[i] = directory / (identity.Text() + "-" + tag + "-" + std::to_string(i) + ".ranked");
            if (!fs::is_regular_file(*paths[i]) || fs::file_size(*paths[i]) != size || !(FileHash(*paths[i], control) == expected)) return {};
        }
        for (auto& size : result.sizes) size = Read64(input);
        if (input.peek() != EOF) return {};
        return result;
    } catch (const Cancelled&) { throw; } catch (const std::exception&) { return {}; }
}
void SaveTraining(const fs::path& directory, const Hash& identity, const TrainingArtifact& artifact, Control& control) {
    const auto tag = Unique();
    const std::array<fs::path, 4> sources{artifact.combined, artifact.ranked[0], artifact.ranked[1], artifact.ranked[2]};
    std::array<std::uint64_t, 4> sizes{}; std::array<Hash, 4> hashes;
    for (unsigned i = 0; i < sources.size(); ++i) {
        control.Check(); const auto destination = directory / (identity.Text() + "-" + tag + "-" + std::to_string(i) + ".ranked");
        std::error_code error; fs::create_hard_link(sources[i], destination, error);
        if (error) AtomicWrite(destination, control, [&](std::ostream& output) {
            std::ifstream input(sources[i], std::ios::binary); if (!input) throw std::runtime_error("Cannot read training artifact.");
            char buffer[65536]; while (input) { control.Check(); input.read(buffer, sizeof buffer); output.write(buffer, input.gcount()); }
            if (!input.eof()) throw std::runtime_error("Cannot copy training artifact.");
        });
        sizes[i] = fs::file_size(destination); hashes[i] = FileHash(destination, control);
    }
    AtomicWrite(directory / (identity.Text() + ".training"), control, [&](std::ostream& output) {
        output.write("LMTRAIN1", 8); U64(output, identity.first); U64(output, identity.second);
        U32(output, static_cast<std::uint32_t>(tag.size())); output.write(tag.data(), tag.size());
        for (unsigned i = 0; i < sources.size(); ++i) { U64(output, sizes[i]); U64(output, hashes[i].first); U64(output, hashes[i].second); }
        for (const auto size : artifact.sizes) U64(output, size);
    });
}

void WriteScore(std::ostream& output, const Training::ScoredNGram& row) {
    unsigned char bytes[45]{}; bytes[0] = row.order;
    for (unsigned i = 0; i < 3; ++i) for (unsigned b = 0; b < 4; ++b) bytes[1 + i * 4 + b] = static_cast<unsigned char>(row.words[i] >> (b * 8));
    for (unsigned b = 0; b < 8; ++b) bytes[13 + b] = static_cast<unsigned char>(row.count >> (b * 8));
    const std::array<double, 3> values{row.probability, row.backoff, row.informationGain};
    for (unsigned i = 0; i < values.size(); ++i) { std::uint64_t bits; static_assert(sizeof bits == sizeof values[i]); std::memcpy(&bits, &values[i], sizeof bits); for (unsigned b = 0; b < 8; ++b) bytes[21 + i * 8 + b] = static_cast<unsigned char>(bits >> (b * 8)); }
    output.write(reinterpret_cast<const char*>(bytes), sizeof bytes);
}
bool ReadScore(std::istream& input, Training::ScoredNGram& row) {
    unsigned char bytes[45]; input.read(reinterpret_cast<char*>(bytes), sizeof bytes);
    if (!input.gcount() && input.eof() && !input.bad()) return false;
    if (!input || input.gcount() != sizeof bytes || bytes[0] < 1 || bytes[0] > 3) throw std::runtime_error("Invalid scored training record.");
    row = {}; row.order = bytes[0];
    for (unsigned i = 0; i < 3; ++i) for (unsigned b = 0; b < 4; ++b) row.words[i] |= std::uint32_t(bytes[1 + i * 4 + b]) << (b * 8);
    for (unsigned b = 0; b < 8; ++b) row.count |= std::uint64_t(bytes[13 + b]) << (b * 8);
    std::array<double*, 3> values{&row.probability, &row.backoff, &row.informationGain};
    for (unsigned i = 0; i < values.size(); ++i) { std::uint64_t bits = 0; for (unsigned b = 0; b < 8; ++b) bits |= std::uint64_t(bytes[21 + i * 8 + b]) << (b * 8); std::memcpy(values[i], &bits, sizeof bits); if (!std::isfinite(*values[i])) throw std::runtime_error("Invalid scored training number."); }
    return true;
}
struct Vocabulary {
    std::vector<std::string> words{std::string{}};
    std::unordered_map<std::string_view, std::uint32_t> ids;
    Vocabulary(const fs::path& counts, std::uint64_t memory, Control& control) {
        std::ifstream input(counts, std::ios::binary); Entry row; std::uint64_t bytes = 0;
        while (ReadEntry(input, row) && row.key[0] == 0) {
            control.Check(); bytes += 128 + row.key.size();
            if (bytes > memory / 3 || words.size() >= (std::numeric_limits<std::uint32_t>::max)()) throw std::runtime_error("The vocabulary exceeds the training RAM budget; use a vocabulary filter or increase memory.");
            words.push_back(row.key.substr(1));
        }
        ids.reserve(words.size()); for (std::uint32_t i = 1; i < words.size(); ++i) ids.emplace(words[i], i);
    }
    Training::NGram Gram(const Entry& row) const {
        Training::NGram result; result.order = static_cast<std::uint8_t>(row.key[0] + 1); result.count = row.count;
        std::size_t start = 1;
        for (unsigned i = 0; i < result.order; ++i) {
            const auto end = row.key.find(' ', start); const auto length = end == std::string::npos ? row.key.size() - start : end - start;
            const auto found = ids.find(std::string_view(row.key.data() + start, length));
            if (found == ids.end()) throw std::runtime_error("Training n-gram references a missing word.");
            result.words[i] = found->second; start = end == std::string::npos ? row.key.size() : end + 1;
        }
        return result;
    }
    std::string Key(const Training::NGram& row) const {
        std::string text;
        for (unsigned i = 0; i < row.order; ++i) { if (i) text += ' '; if (!row.words[i] || row.words[i] >= words.size()) throw std::runtime_error("Invalid vocabulary ID in profile."); text += words[row.words[i]]; }
        return text;
    }
};
fs::path ScoreTraining(const fs::path& combined, const Vocabulary& vocabulary, const Options& options,
                       const Hash& identity, const fs::path& cache, Scratch& scratch, Control& control) {
    Hash scoredIdentity = identity; scoredIdentity.Add("Boundaryless-MKN-v1");
    const auto manifest = cache / (scoredIdentity.Text() + ".scores-manifest");
    if (options.useCache && options.useTrainingCache) try {
        std::ifstream metadata(manifest, std::ios::binary); char magic[8]; metadata.read(magic, 8);
        if (metadata && !std::memcmp(magic, "LMSCORE1", 8)) {
            const auto length = Read32(metadata); if (!length || length > 128) throw std::runtime_error("Invalid score cache.");
            std::string tag(length, '\0'); metadata.read(tag.data(), length);
            if (!metadata || tag.find_first_not_of("0123456789-") != std::string::npos) throw std::runtime_error("Invalid score cache.");
            const auto size = Read64(metadata); Hash expected; expected.first = Read64(metadata); expected.second = Read64(metadata);
            const auto file = cache / (scoredIdentity.Text() + "-" + tag + ".scores");
            if (metadata.peek() == EOF && fs::is_regular_file(file) && fs::file_size(file) == size && size % 45 == 0 && FileHash(file, control) == expected) { control.Report("Reusing smoothed training scores", 80); return file; }
        }
    } catch (const Cancelled&) { throw; } catch (const std::exception&) { /* Rebuild only the corrupt scored layer. */ }
    control.Report("Training modified Kneser-Ney probabilities", 79);
    const auto tuples = scratch.File();
    {
        std::ifstream input(combined, std::ios::binary); std::ofstream output(tuples, std::ios::binary); Entry row;
        if (!input || !output) throw std::runtime_error("Cannot create word-ID training stream.");
        while (ReadEntry(input, row)) { control.Check(); const auto gram = vocabulary.Gram(row); unsigned char bytes[21]{}; bytes[0] = gram.order;
            for (unsigned i = 0; i < 3; ++i) for (unsigned b = 0; b < 4; ++b) bytes[1 + i * 4 + b] = static_cast<unsigned char>(gram.words[i] >> (b * 8));
            for (unsigned b = 0; b < 8; ++b) bytes[13 + b] = static_cast<unsigned char>(gram.count >> (b * 8)); output.write(reinterpret_cast<const char*>(bytes), sizeof bytes);
        }
        Finish(output, tuples);
    }
    const Training::CountScanner scan = [&](const Training::CountVisitor& visit) {
        std::ifstream input(tuples, std::ios::binary); unsigned char bytes[21];
        for (;;) { input.read(reinterpret_cast<char*>(bytes), sizeof bytes); if (!input.gcount() && input.eof() && !input.bad()) break;
            if (!input || input.gcount() != sizeof bytes) throw std::runtime_error("Truncated word-ID training stream.");
            control.Check(); Training::NGram gram; gram.order = bytes[0];
            for (unsigned i = 0; i < 3; ++i) for (unsigned b = 0; b < 4; ++b) gram.words[i] |= std::uint32_t(bytes[1 + i * 4 + b]) << (b * 8);
            for (unsigned b = 0; b < 8; ++b) gram.count |= std::uint64_t(bytes[13 + b]) << (b * 8); visit(gram);
        }
    };
    const auto scored = scratch.File(); std::ofstream output(scored, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot create scored training stream.");
    Training::Options training; training.memoryBytes = static_cast<std::size_t>(options.memoryBytes / 2); training.scratchDirectory = scratch.directory; training.check = [&] { control.Check(); };
    Training::TrainModifiedKneserNey(scan, static_cast<std::uint32_t>(vocabulary.words.size()), training, [&](const Training::ScoredNGram& row) { WriteScore(output, row); });
    Finish(output, scored);
    if (options.useCache && options.useTrainingCache) {
        const auto tag = Unique(); const auto destination = cache / (scoredIdentity.Text() + "-" + tag + ".scores");
        std::error_code error; fs::create_hard_link(scored, destination, error);
        if (error) fs::copy_file(scored, destination);
        const auto checksum = FileHash(destination, control);
        AtomicWrite(manifest, control, [&](std::ostream& stream) { stream.write("LMSCORE1", 8); U32(stream, static_cast<std::uint32_t>(tag.size())); stream.write(tag.data(), tag.size()); U64(stream, fs::file_size(destination)); U64(stream, checksum.first); U64(stream, checksum.second); });
    }
    return scored;
}

struct ProfileCandidate { Training::ScoredNGram row; double utility = 0; };
bool BetterCandidate(const ProfileCandidate& left, const ProfileCandidate& right) {
    if (left.utility != right.utility) return left.utility > right.utility;
    if (left.row.count != right.row.count) return left.row.count > right.row.count;
    if (left.row.order != right.row.order) return left.row.order < right.row.order;
    return left.row.words < right.row.words;
}
struct GramLess {
    bool operator()(const WordGram& left, const WordGram& right) const { return left.order != right.order ? left.order < right.order : left.words < right.words; }
};
void WriteJson(const std::array<std::vector<Entry>, 3>& selected, std::ostream& output, Control& control);
void JsonString(std::ostream& output, const std::string& text);
Result GenerateRadix(const Options& options, const fs::path& combined, const std::array<fs::path, 3>& ranked,
                     const Hash& identity, const fs::path& cache, Scratch& scratch, Control& control,
                     const fs::path& triePath, const fs::path& jsonPath, Result result) {
    if (triePath.empty()) throw std::invalid_argument("A compressed radix export needs a trie output file.");
    if (options.smoothing != Smoothing::Counts && options.limits[0] && !*options.limits[0])
        throw std::invalid_argument("Smoothed models need a unigram vocabulary.");
    if (options.smoothing != Smoothing::Counts && options.limits[1] && !*options.limits[1] && (!options.limits[2] || *options.limits[2]))
        throw std::invalid_argument("Smoothed trigrams require their bigram history records; enable bigrams or disable trigrams.");
    Vocabulary vocabulary(combined, options.memoryBytes, control);
    const bool smoothed = options.smoothing == Smoothing::ModifiedKneserNey;
    std::uint64_t maximumProfile = options.maximumBytes.value_or(options.memoryBytes / 16);
    for (const auto size : options.profileSizes) maximumProfile = (std::max)(maximumProfile, size);
    const auto capacity = static_cast<std::size_t>((std::max)(std::uint64_t(64), (std::min)(maximumProfile / 2 + 1, options.memoryBytes / 1024)));
    using CandidateQueue = std::priority_queue<ProfileCandidate, std::vector<ProfileCandidate>, decltype(&BetterCandidate)>;
    CandidateQueue pool(&BetterCandidate);
    // Explicit order limits can describe a bounded request even when no byte
    // size was selected. Retain the best rows of each requested order so a
    // frequent order cannot displace all candidates of another limited order.
    const bool finiteLimits = std::any_of(options.limits.begin(), options.limits.end(),
        [](const auto& limit) { return limit.has_value(); });
    std::array<std::unique_ptr<CandidateQueue>, 3> limitedPools;
    if (finiteLimits) for (auto& queue : limitedPools) queue = std::make_unique<CandidateQueue>(&BetterCandidate);
    std::vector<Training::ScoredNGram> unigrams(vocabulary.words.size());
    std::uint64_t eligible = 0;
    std::array<std::uint64_t, 3> eligibleByOrder{};
    auto candidate = [&](const Training::ScoredNGram& row) {
        if (!row.words[0]) return;
        if (row.order == 1) unigrams.at(row.words[0]) = row;
        if (options.limits[row.order - 1] && !*options.limits[row.order - 1]) return;
        ++eligible;
        ++eligibleByOrder[row.order - 1];
        double cost = row.order == 1 ? 16 : 10;
        for (unsigned i = 0; i < row.order; ++i) cost += vocabulary.words.at(row.words[i]).size() * (row.order == 1 ? 2 : 1);
        const double gain = row.order == 1 || !smoothed ? static_cast<double>(row.count) : row.informationGain;
        ProfileCandidate item{row, gain / cost};
        auto& queue = finiteLimits ? *limitedPools[row.order - 1] : pool;
        const auto limit = finiteLimits ? static_cast<std::size_t>((std::min)(std::uint64_t(capacity), options.limits[row.order - 1].value_or(capacity))) : capacity;
        if (queue.size() < limit) queue.push(item);
        else if (limit && BetterCandidate(item, queue.top())) { queue.pop(); queue.push(item); }
    };
    fs::path scored;
    if (smoothed) {
        scored = ScoreTraining(combined, vocabulary, options, identity, cache, scratch, control);
        std::ifstream input(scored, std::ios::binary); Training::ScoredNGram row;
        while (ReadScore(input, row)) { control.Check(); candidate(row); }
    } else {
        for (unsigned order = 0; order < 3; ++order) {
            std::ifstream input(ranked[order], std::ios::binary); Entry row;
            while (ReadEntry(input, row)) { control.Check(); Training::ScoredNGram next; static_cast<Training::NGram&>(next) = vocabulary.Gram(row); candidate(next); }
        }
    }
    if (finiteLimits) {
        eligible = 0;
        for (unsigned order = 0; order < 3; ++order) {
            const auto amount = (std::min)(eligibleByOrder[order], options.limits[order].value_or(eligibleByOrder[order]));
            eligible = amount > UINT64_MAX - eligible ? UINT64_MAX : eligible + amount;
            auto& queue = limitedPools[order];
            while (!queue->empty()) {
                const auto item = queue->top(); queue->pop();
                if (pool.size() < capacity) pool.push(item);
                else if (BetterCandidate(item, pool.top())) { pool.pop(); pool.push(item); }
            }
            queue.reset();
        }
    }
    if (!options.maximumBytes && eligible > capacity)
        throw std::runtime_error("An unlimited radix model exceeds this memory budget; choose a model size or increase training memory.");
    std::vector<ProfileCandidate> candidates; candidates.reserve(pool.size());
    while (!pool.empty()) { candidates.push_back(pool.top()); pool.pop(); }
    std::sort(candidates.begin(), candidates.end(), BetterCandidate);
    std::unordered_map<WordGram, Training::ScoredNGram, WordGramHash> histories;
    if (smoothed) {
        for (const auto& item : candidates) if (item.row.order == 3) histories.emplace(WordGram{{item.row.words[0], item.row.words[1], 0}, 2}, Training::ScoredNGram{});
        std::ifstream input(scored, std::ios::binary); Training::ScoredNGram row;
        while (ReadScore(input, row)) {
            control.Check(); if (row.order > 2) break;
            if (row.order == 2) { const auto found = histories.find({row.words, row.order}); if (found != histories.end()) found->second = row; }
        }
    }
    struct Profile {
        espos::keyboard::prediction::RadixEncodedModel encoded;
        std::vector<Training::ScoredNGram> records;
        double unknown = 0;
    };
    // Keep the encoder's expanded byte graph inside the aggregate workspace
    // budget. A small output budget alone does not bound an oversized binary
    // search probe, especially when corpus words have long shared prefixes.
    auto addBytes = [](std::uint64_t left, std::uint64_t right) {
        return right > UINT64_MAX - left ? UINT64_MAX : left + right;
    };
    auto multiplyBytes = [](std::uint64_t amount, std::uint64_t bytes) {
        return bytes && amount > UINT64_MAX / bytes ? UINT64_MAX : amount * bytes;
    };
    std::uint64_t persistentBytes = 65536;
    persistentBytes = addBytes(persistentBytes, multiplyBytes(vocabulary.words.capacity(), sizeof(std::string)));
    persistentBytes = addBytes(persistentBytes, multiplyBytes(vocabulary.ids.bucket_count(), sizeof(void*)));
    for (const auto& word : vocabulary.words) persistentBytes = addBytes(persistentBytes, word.capacity() + 1);
    persistentBytes = addBytes(persistentBytes, multiplyBytes(vocabulary.ids.size(), sizeof(std::pair<const std::string_view, std::uint32_t>) + 8 * sizeof(void*)));
    persistentBytes = addBytes(persistentBytes, multiplyBytes(unigrams.capacity(), sizeof(Training::ScoredNGram)));
    // priority_queue retains its backing allocation after draining. Reserve
    // twice its maximum size to cover vector growth on supported toolchains.
    persistentBytes = addBytes(persistentBytes, multiplyBytes(capacity, 2 * sizeof(ProfileCandidate)));
    persistentBytes = addBytes(persistentBytes, multiplyBytes(candidates.capacity(), sizeof(ProfileCandidate)));
    persistentBytes = addBytes(persistentBytes, multiplyBytes(histories.bucket_count(), sizeof(void*)));
    persistentBytes = addBytes(persistentBytes, multiplyBytes(histories.size(), sizeof(std::pair<const WordGram, Training::ScoredNGram>) + 8 * sizeof(void*)));
    const auto retainedBytes = [&](const Profile& profile) {
        return addBytes(multiplyBytes(profile.records.capacity(), sizeof(Training::ScoredNGram)),
            addBytes(profile.encoded.trie.capacity(), profile.encoded.index.capacity()));
    };
    auto build = [&](std::size_t count, std::uint64_t liveProfileBytes = 0) -> std::optional<Profile> {
        control.Check(); Profile profile; std::map<WordGram, Training::ScoredNGram, GramLess> selected;
        std::array<std::uint64_t, 3> amounts{};
        std::uint64_t keyBytes = 0;
        // Includes the selected map, record-vector growth when inserting UNK,
        // string objects, borrowed encoder entries and sorted key pointers.
        constexpr auto rowWorkspace = sizeof(std::pair<const WordGram, Training::ScoredNGram>) + 8 * sizeof(void*) +
            2 * sizeof(Training::ScoredNGram) + sizeof(std::string) + sizeof(espos::keyboard::prediction::RadixEntry) + sizeof(void*);
        auto workspace = [&](std::uint64_t records, std::uint64_t text, std::uint64_t nodes) {
            auto bytes = addBytes(persistentBytes, liveProfileBytes);
            bytes = addBytes(bytes, multiplyBytes(records + 2, rowWorkspace));
            // Appending a multiword key may briefly retain its old string
            // buffer while allocating the next geometrically grown buffer.
            bytes = addBytes(bytes, multiplyBytes(text, 3));
            // Per logical byte: BuilderNode growth (64), compressed Edge
            // growth (32), labels/bounds (3), and grown NGTR/NGIX buffers.
            // 192 also covers worst-case 64-bit terminal/mass varints.
            bytes = addBytes(bytes, multiplyBytes(nodes, 192));
            return addBytes(bytes, 4096);
        };
        for (std::size_t i = 0; i < count; ++i) {
            const auto& row = candidates[i].row;
            std::vector<Training::ScoredNGram> dependencies;
            if (smoothed && row.order > 1) {
                for (unsigned word = 0; word < row.order; ++word) dependencies.push_back(unigrams.at(row.words[word]));
                if (row.order == 3) {
                    const auto found = histories.find({{row.words[0], row.words[1], 0}, 2});
                    if (found == histories.end() || found->second.order != 2) throw std::runtime_error("A trigram is missing its trained history.");
                    dependencies.push_back(found->second);
                }
            }
            dependencies.push_back(row);
            auto prospective = amounts; std::set<WordGram, GramLess> additions; bool allowed = true;
            for (const auto& next : dependencies) {
                const WordGram key{next.words, next.order};
                if (!next.order || !next.words[0]) throw std::runtime_error("Incomplete language-model dependency.");
                if (!selected.count(key) && additions.insert(key).second) {
                    ++prospective[next.order - 1];
                    if (options.limits[next.order - 1] && prospective[next.order - 1] > *options.limits[next.order - 1]) allowed = false;
                }
            }
            if (!allowed) continue;
            for (const auto& next : dependencies) {
                const WordGram key{next.words, next.order};
                if (!selected.count(key)) {
                    std::uint64_t length = next.order - 1;
                    for (unsigned word = 0; word < next.order; ++word) length = addBytes(length, vocabulary.words.at(next.words[word]).size());
                    const auto nextBytes = addBytes(keyBytes, length);
                    if (workspace(selected.size() + 1, nextBytes, 0) > options.memoryBytes) return std::nullopt;
                    selected.emplace(key, next); keyBytes = nextBytes;
                }
            }
            amounts = prospective;
        }
        profile.records.reserve(selected.size()); for (const auto& item : selected) profile.records.push_back(item.second);
        double unknown = 0;
        if (smoothed) unknown = Training::ReestimatePrunedBackoffs(profile.records);
        profile.unknown = unknown;
        std::vector<std::string> keys; keys.reserve(profile.records.size());
        for (const auto& row : profile.records) if (row.words[0]) keys.push_back(vocabulary.Key(row));
        std::vector<espos::keyboard::prediction::RadixEntry> entries; entries.reserve(keys.size());
        std::size_t key = 0;
        for (const auto& row : profile.records) if (row.words[0]) {
            double backoff = smoothed && row.order < 3 ? row.backoff : -1;
            // An omitted backoff means one. Avoid spending two bytes for the
            // same quantized value on every context-free spelling entry.
            if (backoff > 0 && std::abs(std::log2(backoff) * 256) < .5) backoff = -1;
            entries.push_back({keys[key++], row.count, smoothed ? row.probability : -1, backoff});
        }
        {
            std::vector<const espos::keyboard::prediction::RadixEntry*> ordered;
            ordered.reserve(entries.size()); for (const auto& entry : entries) ordered.push_back(&entry);
            std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) { return left->key < right->key; });
            std::uint64_t nodes = 2; std::string_view previous, previousUnigram;
            auto growth = [](std::string_view word, std::string_view before) {
                std::size_t common = 0; while (common < word.size() && common < before.size() && word[common] == before[common]) ++common;
                return word.size() - common;
            };
            for (const auto* entry : ordered) {
                control.Check(); nodes = addBytes(nodes, growth(entry->key, previous)); previous = entry->key;
                if (entry->count && entry->key.find(' ') == std::string_view::npos) {
                    nodes = addBytes(nodes, growth(entry->key, previousUnigram)); previousUnigram = entry->key;
                }
            }
            if (workspace(selected.size(), keyBytes, nodes) > options.memoryBytes) return std::nullopt;
        }
        profile.encoded = espos::keyboard::prediction::EncodeRadixModel(entries, {smoothed, unknown});
        return profile;
    };
    std::vector<std::optional<std::uint64_t>> budgets{options.maximumBytes};
    for (const auto size : options.profileSizes) if (std::find(budgets.begin(), budgets.end(), std::optional<std::uint64_t>(size)) == budgets.end()) budgets.push_back(size);
    for (std::size_t number = 0; number < budgets.size(); ++number) {
        const auto budget = budgets[number];
        if (budget && *budget < 64) throw std::invalid_argument("A radix model plus index needs at least 64 bytes.");
        Profile profile;
        if (!budget) {
            auto complete = build(candidates.size());
            if (!complete) throw std::runtime_error("The requested radix profile exceeds the encoder RAM budget; choose a model size, reduce n-gram limits, or increase training memory.");
            profile = std::move(*complete);
        }
        else {
            // Utility/cost ranks are estimates; enforce the actual encoded
            // NGTR+NGIX budget and vocabulary/history closure at publication.
            std::size_t low = 0, high = candidates.size(); auto empty = build(0);
            if (!empty) throw std::runtime_error("Training RAM budget is too small for radix profile encoding.");
            profile = std::move(*empty);
            if (profile.encoded.Bytes() > *budget) throw std::invalid_argument("The selected budget cannot hold an empty model and index.");
            while (low < high) {
                const auto middle = low + (high - low + 1) / 2; auto trial = build(middle, retainedBytes(profile));
                if (trial && trial->encoded.Bytes() <= *budget) { low = middle; profile = std::move(*trial); }
                else high = middle - 1;
            }
        }
        const auto path = number == 0 ? triePath : triePath.parent_path() /
            (triePath.stem().u8string() + "-" + std::to_string(*budget) + triePath.extension().u8string());
        auto indexPath = path; indexPath.replace_extension(".ngix");
        WriteModelPair(path, indexPath, profile.encoded, control,
            80 + static_cast<unsigned>(17 * (number + 1) / budgets.size()));
        result.profileOutputs.push_back(path);
        if (!number) {
            result.trieBytes = profile.encoded.trie.size(); result.indexBytes = profile.encoded.index.size();
            result.logicalNodes = profile.encoded.logical_nodes; result.physicalNodes = profile.encoded.physical_nodes;
            for (const auto& row : profile.records) if (row.words[0] && row.count) ++result.counts[row.order - 1];
            if (!jsonPath.empty()) {
                if (smoothed) AtomicWrite(jsonPath, control, [&](std::ostream& stream) {
                    stream.imbue(std::locale::classic()); stream << std::setprecision(17);
                    stream << "{\n  \"formatVersion\": 4,\n  \"smoothing\": \"modified-kn\",\n  \"unknownProbability\": " << profile.unknown;
                    const char* names[]{"unigrams", "bigrams", "trigrams"};
                    for (unsigned order = 1; order <= 3; ++order) {
                        stream << ",\n  \"" << names[order - 1] << "\": ["; bool first = true;
                        for (const auto& row : profile.records) if (row.order == order && row.words[0]) {
                            control.Check(); stream << (first ? "\n" : ",\n") << "    {\"tokens\": ["; first = false;
                            for (unsigned i = 0; i < order; ++i) { if (i) stream << ", "; JsonString(stream, vocabulary.words.at(row.words[i])); }
                            stream << "], \"count\": " << row.count << ", \"weight\": " << row.probability
                                   << ", \"probability\": " << row.probability << ", \"backoff\": " << row.backoff << '}';
                        }
                        stream << (first ? "" : "\n  ") << ']';
                    }
                    stream << "\n}\n";
                });
                else {
                std::array<std::vector<Entry>, 3> selected;
                for (const auto& row : profile.records) if (row.words[0] && row.count) selected[row.order - 1].push_back({std::string(1, static_cast<char>(row.order - 1)) + vocabulary.Key(row), row.count});
                for (auto& rows : selected) std::sort(rows.begin(), rows.end(), Frequency);
                AtomicWrite(jsonPath, control, [&](std::ostream& stream) { WriteJson(selected, stream, control); });
                }
            }
        }
        control.Report("Encoding compressed model profiles", 80 + static_cast<unsigned>(17 * (number + 1) / budgets.size()));
    }
    control.Report("Complete", 100, false); return result;
}

struct Node { unsigned char label = 0; std::uint32_t mass = 0, child = 0, sibling = 0; };
class Trie {
    bool compact; std::vector<Node> nodes{Node{}, Node{}}; std::uint64_t compactSize = 20;
    std::uint32_t Find(std::uint32_t parent, unsigned char label, std::uint32_t& before) const {
        before = 0; auto child = nodes[parent].child;
        while (child && nodes[child].label < label) { before = child; child = nodes[child].sibling; } return child;
    }
    static unsigned VarSize(std::uint32_t value) { unsigned size = 1; while ((value >>= 7)) ++size; return size; }
    std::uint64_t Growth(std::uint32_t parent, const std::string& path, std::uint32_t count) const {
        for (std::size_t index = 0; index < path.size(); ++index) {
            std::uint32_t before; const auto child = Find(parent, static_cast<unsigned char>(path[index]), before);
            if (!child || nodes[child].label != static_cast<unsigned char>(path[index])) {
                const auto missing = path.size() - index; if (!compact) return missing * 12;
                unsigned degree = 0; for (auto current = nodes[parent].child; current && degree <= 63; current = nodes[current].sibling) ++degree;
                return missing * 2 + VarSize(count) + (degree == 63 ? 1 : 0);
            }
            parent = child;
        }
        if (!compact) return 0;
        auto terminal = nodes[parent].mass; for (auto child = nodes[parent].child; child; child = nodes[child].sibling) terminal -= nodes[child].mass;
        return VarSize(terminal + count) - (terminal ? VarSize(terminal) : 0);
    }
    void Insert(std::uint32_t parent, const std::string& path, std::uint32_t count) {
        nodes[parent].mass += count;
        for (unsigned char label : path) {
            std::uint32_t before; auto child = Find(parent, label, before);
            if (!child || nodes[child].label != label) {
                if (nodes.size() >= (std::numeric_limits<std::uint32_t>::max)()) throw std::overflow_error("Trie node limit exceeded.");
                const auto created = static_cast<std::uint32_t>(nodes.size()); nodes.push_back({label, 0, 0, child});
                if (!before) nodes[parent].child = created; else nodes[before].sibling = created; child = created;
            }
            parent = child; nodes[parent].mass += count;
        }
    }
    static void Varint(std::ostream& output, std::uint32_t value) { while (value >= 128) { output.put(static_cast<char>((value & 127) | 128)); value >>= 7; } output.put(static_cast<char>(value)); }
public:
    explicit Trie(bool dense) : compact(dense) {}
    std::uint64_t Size() const { return compact ? compactSize : 16 + nodes.size() * 12ull; }
    bool Add(const Entry& entry, bool unigram, const std::optional<std::uint64_t>& budget) {
        if (entry.count > std::uint64_t((std::numeric_limits<std::uint32_t>::max)()) - nodes[0].mass) {
            if (budget) return false; throw std::overflow_error("Trie exceeds the format's 32-bit count limit.");
        }
        const auto path = entry.key.substr(1); const auto count = static_cast<std::uint32_t>(entry.count);
        const auto growth = (compact || budget) ? Growth(0, path, count) + (unigram ? Growth(1, path, count) : 0) : 0;
        if (budget && growth > *budget - Size()) return false;
        Insert(0, path, count); if (unigram) Insert(1, path, count); if (compact) compactSize += growth; return true;
    }
    void Write(std::ostream& output, Control& control) const {
        output.write("NGTR", 4); U32(output, compact ? 3 : 2); U32(output, static_cast<std::uint32_t>(nodes.size())); U32(output, compact ? 0 : 12);
        std::vector<std::uint32_t> ordered{0, 1}; ordered.reserve(nodes.size());
        for (std::size_t index = 0; index < ordered.size(); ++index) {
            if (!(index & 4095)) { control.Check(); control.Report("Writing trie", 97 + static_cast<unsigned>(2.0 * index / nodes.size())); }
            const auto& node = nodes[ordered[index]]; const auto first = static_cast<std::uint32_t>(ordered.size()); auto terminal = node.mass;
            for (auto child = node.child; child; child = nodes[child].sibling) { ordered.push_back(child); if (compact) terminal -= nodes[child].mass; }
            const auto children = static_cast<std::uint32_t>(ordered.size()) - first;
            if (compact) { output.put(static_cast<char>(node.label)); Varint(output, (children << 1) | (terminal ? 1 : 0)); if (terminal) Varint(output, terminal); }
            else { U32(output, children ? first : 0); U32(output, node.mass); output.put(static_cast<char>(children)); output.put(static_cast<char>(children >> 8)); output.put(static_cast<char>(node.label)); output.put(0); }
        }
    }
};

std::string PathIdentity(const fs::path& path) {
    auto text = fs::weakly_canonical(fs::absolute(path)).u8string();
#ifdef _WIN32
    // Windows paths are case insensitive; corpus/word-list text remains Unicode.
    for (auto& byte : text) if (byte >= 'A' && byte <= 'Z') byte += 'a' - 'A';
#endif
    return text;
}
std::vector<fs::path> FindFiles(const Options& options, Control& control) {
    std::vector<fs::path> result; std::unordered_set<std::string> seen;
    auto add = [&](const fs::path& file) { if (seen.insert(PathIdentity(file)).second) result.push_back(fs::absolute(file).lexically_normal()); };
    for (const auto& source : options.inputs) {
        control.Check(); if (fs::is_regular_file(source)) { add(source); continue; }
        if (!fs::is_directory(source)) throw std::runtime_error("Corpus file or folder not found: " + source.u8string());
        std::vector<fs::path> files;
        auto inspect = [&](const fs::directory_entry& entry) {
            control.Check(); if (entry.is_symlink() || !entry.is_regular_file()) return;
            auto extension = entry.path().extension().u8string(); for (auto& byte : extension) if (byte >= 'A' && byte <= 'Z') byte += 32;
            if (extension == ".txt") files.push_back(entry.path());
        };
        if (options.recursive) for (const auto& entry : fs::recursive_directory_iterator(source)) inspect(entry);
        else for (const auto& entry : fs::directory_iterator(source)) inspect(entry);
        if (files.empty()) throw std::runtime_error("No .txt files found in " + source.u8string());
        std::sort(files.begin(), files.end()); for (const auto& file : files) add(file);
    }
    return result;
}
void JsonString(std::ostream& output, const std::string& text) {
    output.put('"'); constexpr char hex[] = "0123456789abcdef";
    for (unsigned char byte : text) {
        if (byte == '"' || byte == '\\') { output.put('\\'); output.put(static_cast<char>(byte)); }
        else if (byte < 32) { output << "\\u00"; output.put(hex[byte >> 4]); output.put(hex[byte & 15]); }
        else output.put(static_cast<char>(byte));
    }
    output.put('"');
}
void WriteJson(const std::array<std::vector<Entry>, 3>& selected, std::ostream& output, Control& control) {
    output.imbue(std::locale::classic());
    constexpr const char* names[] = {"unigrams", "bigrams", "trigrams"}; output << "{\n"; output << std::setprecision(17);
    for (unsigned order = 0; order < 3; ++order) {
        std::unordered_map<std::string, Count> prefixes;
        for (const auto& entry : selected[order]) {
            const auto gram = entry.key.substr(1); const auto prefix = order ? gram.substr(0, gram.rfind(' ')) : std::string{};
            auto& count = prefixes[prefix]; if (entry.count > maximumCount - count) throw std::overflow_error("Prefix count overflow."); count += entry.count;
        }
        output << "  \"" << names[order] << "\": [";
        for (std::size_t index = 0; index < selected[order].size(); ++index) {
            if (!(index & 511)) control.Check(); const auto& entry = selected[order][index]; const auto gram = entry.key.substr(1);
            output << (index ? ",\n" : "\n") << "    {\"tokens\": [";
            std::size_t start = 0;
            for (unsigned token = 0; token <= order; ++token) { if (token) output << ", "; const auto end = gram.find(' ', start); JsonString(output, gram.substr(start, end == std::string::npos ? end : end - start)); start = end + 1; }
            const auto prefix = order ? gram.substr(0, gram.rfind(' ')) : std::string{};
            output << "], \"count\": " << entry.count << ", \"weight\": " << double(entry.count) / prefixes[prefix] << '}';
        }
        output << (selected[order].empty() ? "" : "\n  ") << ']' << (order < 2 ? ",\n" : "\n");
    }
    output << "}\n";
}
}

Result Generate(const Options& options, ProgressCallback progress, const std::atomic_bool* cancelled)
{
    Control control(std::move(progress), cancelled); control.Report("Finding corpus files", 0);
    if (options.inputs.empty()) throw std::invalid_argument("Add at least one corpus file or folder.");
    if (options.trieOutput.empty() && options.jsonOutput.empty()) throw std::invalid_argument("Choose a trie output file.");
    if (options.workers < 1 || options.workers > 1024) throw std::invalid_argument("Worker count must be between 1 and 1024.");
    if (!options.chunkEntries) throw std::invalid_argument("Count chunk size must be positive.");
    if (options.memoryBytes < 8ull * 1024 * 1024 || options.memoryBytes > SIZE_MAX)
        throw std::invalid_argument("Training memory must be at least 8 MiB and fit this process's address space.");
    if (options.smoothing != Smoothing::Counts && options.format != ModelFormat::Radix)
        throw std::invalid_argument("Smoothed language models require the compressed radix format.");
    if (!options.profileSizes.empty() && options.format != ModelFormat::Radix)
        throw std::invalid_argument("Batch profiles require the compressed radix format.");
    if (options.format == ModelFormat::Radix) {
        if (options.maximumBytes && *options.maximumBytes < 64) throw std::invalid_argument("A radix model plus index needs at least 64 bytes.");
        for (const auto size : options.profileSizes) if (size < 64) throw std::invalid_argument("Every radix profile needs at least 64 bytes.");
    }
    if (options.language != "en" && options.language != "es" && options.language != "fr" && options.language != "de" && options.language != "el" && !ScriptCorpus(options.language)) throw std::invalid_argument("Choose a supported corpus language: English, Spanish, French, German, Greek, Arabic, Hindi, Mandarin Chinese, Bengali, Indonesian, Urdu or Russian.");
    if (options.language != "en" && !ScriptCorpus(options.language) && (!options.useWordList || !options.wordListPath.empty())) throw std::invalid_argument("This corpus language requires its bundled word list.");
    if (!options.useWordList && !options.wordListPath.empty()) throw std::invalid_argument("Choose a custom word list or disable filtering, not both.");
    if (options.maximumBytes && *options.maximumBytes < (options.compact ? 20u : 40u)) throw std::invalid_argument(options.compact ? "An NGTR v3 trie needs at least 20 bytes." : "An NGTR v2 trie needs at least 40 bytes.");
    const auto files = FindFiles(options, control); if (files.empty()) throw std::invalid_argument("No corpus files found.");
    const auto triePath = options.trieOutput.empty() ? fs::path{} : fs::absolute(options.trieOutput).lexically_normal();
    const auto jsonPath = options.jsonOutput.empty() ? fs::path{} : fs::absolute(options.jsonOutput).lexically_normal();
    if (!triePath.empty() && !jsonPath.empty() && PathIdentity(triePath) == PathIdentity(jsonPath)) throw std::invalid_argument("JSON and trie outputs must use different files.");
    for (const auto& output : {triePath, jsonPath}) if (!output.empty()) {
        if (fs::is_directory(output)) throw std::invalid_argument("Choose an output file, not a folder.");
        for (const auto& input : files) if (PathIdentity(output) == PathIdentity(input)) throw std::invalid_argument("Output files must be different from corpus files.");
        if (!options.wordListPath.empty() && PathIdentity(output) == PathIdentity(options.wordListPath)) throw std::invalid_argument("Output files must be different from the word list.");
        if (!fs::is_directory(output.parent_path())) throw std::invalid_argument("Output folder does not exist: " + output.parent_path().u8string());
    }
    if (options.format == ModelFormat::Radix) {
        auto primaryIndex = triePath; primaryIndex.replace_extension(".ngix");
        if (triePath.empty() || PathIdentity(triePath) == PathIdentity(primaryIndex)) throw std::invalid_argument("Choose a trie output file other than the .ngix lookup index.");
        std::vector<fs::path> outputs{triePath};
        for (const auto size : options.profileSizes) if (!options.maximumBytes || size != *options.maximumBytes) {
            const auto path = triePath.parent_path() / (triePath.stem().u8string() + "-" + std::to_string(size) + triePath.extension().u8string());
            if (std::find(outputs.begin(), outputs.end(), path) == outputs.end()) outputs.push_back(path);
        }
        const auto modelCount = outputs.size();
        for (std::size_t i = 0; i < modelCount; ++i) { auto path = outputs[i]; path.replace_extension(".ngix"); outputs.push_back(path); }
        std::set<std::string> outputIdentities;
        for (const auto& output : outputs) {
            if (!outputIdentities.insert(PathIdentity(output)).second) throw std::invalid_argument("Model and index outputs must identify different files.");
            if (!jsonPath.empty() && PathIdentity(output) == PathIdentity(jsonPath)) throw std::invalid_argument("A model/index output would overwrite the JSON output.");
            for (const auto& input : files) if (PathIdentity(output) == PathIdentity(input)) throw std::invalid_argument("Model and index outputs must not overwrite corpus files.");
            if (!options.wordListPath.empty() && PathIdentity(output) == PathIdentity(options.wordListPath)) throw std::invalid_argument("Model and index outputs must not overwrite the vocabulary.");
            if (fs::is_directory(output)) throw std::invalid_argument("A model output names an existing directory.");
        }
    }
    std::vector<fs::path> corpus;
    for (const auto& file : files) if (options.wordListPath.empty() || PathIdentity(file) != PathIdentity(options.wordListPath)) corpus.push_back(file);
    if (corpus.empty()) throw std::invalid_argument("Provide corpus text in addition to the word list.");
    control.totalFiles = corpus.size(); for (const auto& file : corpus) control.totalBytes += fs::file_size(file);
    control.Report("Loading word list", 0); const WordList words(options, control);
    const auto reservedBytes = words.EstimatedBytes() + 4ull * 1024 * 1024;
    if (options.memoryBytes <= reservedBytes + 2ull * 1024 * 1024)
        throw std::invalid_argument("Training memory is too small for the selected vocabulary; increase the memory budget.");
    const auto workingBytes = options.memoryBytes - reservedBytes;
    const unsigned workers = static_cast<unsigned>((std::min<std::uint64_t>)(options.workers, workingBytes / (2ull * 1024 * 1024)));
    const auto workerBytes = workingBytes / workers;
    auto cache = options.cacheDirectory;
    if (options.useCache && cache.empty()) cache = fs::temp_directory_path() / "LVGL_UI_Creator" / "LanguageModelCounts";
    if (options.useCache) fs::create_directories(cache);
    const auto scratchDirectory = options.scratchDirectory.empty() ? fs::temp_directory_path() / "LVGL_UI_Creator" / "LanguageModelWork" : options.scratchDirectory;
    Scratch scratch(scratchDirectory);
    struct FilePlan { Signature signature; Hash identity; fs::path destination, merged; std::vector<fs::path> parts; };
    std::vector<FilePlan> plans(corpus.size()); std::atomic<std::uint64_t> hits{0};
    control.Report("Counting n-grams", 0);
    RunWorkers(corpus.size(), workers, control, [&](std::size_t index) {
        StreamPermit permit(control, 2);
        const auto& path = corpus[index]; auto& plan = plans[index]; plan.signature = SignatureFor(path);
        plan.identity.Add("Native-Counts-v4"); plan.identity.Add(PathIdentity(path));
        if (ScriptCorpus(words.language)) { plan.identity.Add("Script-Corpus-v1"); plan.identity.Add(words.language); }
        if (options.useCache) { const auto content = FileHash(path, control); plan.identity.Add(&content.first, sizeof content.first); plan.identity.Add(&content.second, sizeof content.second); }
        if (words.enabled) { plan.identity.Add(words.language); plan.identity.Add(&words.hash.first, sizeof words.hash.first); plan.identity.Add(&words.hash.second, sizeof words.hash.second); }
        plan.destination = options.useCache ? cache / (plan.identity.Text() + ".counts") : fs::path{};
        if (!SameSignature(plan.signature, SignatureFor(path))) throw std::runtime_error("Corpus file changed while processing: " + path.u8string());
    });
    Hash trainingIdentity; trainingIdentity.Add("Merged-Ranked-Counts-v1");
    for (const auto& plan : plans) { trainingIdentity.Add(&plan.identity.first, sizeof plan.identity.first); trainingIdentity.Add(&plan.identity.second, sizeof plan.identity.second); }
    TrainingArtifact artifact;
    if (options.useCache && options.useTrainingCache) artifact = LoadTraining(cache, trainingIdentity, control);
    const bool trainingCacheHit = !artifact.combined.empty();
    fs::path combined; std::array<fs::path, 3> ranked; std::array<std::uint64_t, 3> sizes{};
    if (trainingCacheHit) {
        combined = artifact.combined; ranked = artifact.ranked; sizes = artifact.sizes;
        hits = corpus.size(); control.files = corpus.size(); control.CountProgress(control.totalBytes);
        control.Report("Reusing ranked training counts", 79);
    } else {
    RunWorkers(corpus.size(), workers, control, [&](std::size_t index) {
        auto& plan = plans[index];
        if (options.useCache) { StreamPermit permit(control, 2); plan.merged = ReadCache(plan.destination, plan.identity, scratch, control); }
        if (!plan.merged.empty()) { ++hits; control.CountProgress(plan.signature.size); ++control.files; }
    });
    std::uint64_t uncachedBytes = 0;
    for (const auto& plan : plans) if (plan.merged.empty()) uncachedBytes += plan.signature.size;
    const auto desiredJobs = std::uint64_t(workers) * 4;
    const auto chunkBytes = (std::max)(std::uint64_t(8u * 1024u * 1024u), uncachedBytes / desiredJobs + (uncachedBytes % desiredJobs != 0));
    struct CountJob { std::size_t file, part; TextRange range; };
    std::vector<CountJob> jobs;
    for (std::size_t index = 0; index < plans.size(); ++index) if (plans[index].merged.empty()) {
        const auto ranges = Partition(corpus[index], plans[index].signature.size, chunkBytes, control);
        plans[index].parts.resize(ranges.size());
        for (std::size_t part = 0; part < ranges.size(); ++part) jobs.push_back({index, part, ranges[part]});
    }
    // The pool consumes ranges from every uncached file. A dominant UTF-8 file
    // can therefore use all requested workers without crossing sentence resets.
    RunWorkers(jobs.size(), workers, control, [&](std::size_t index) {
        const auto& job = jobs[index]; std::vector<fs::path> chunks;
        WordCounts counts(workerBytes, options.lowMemory ? options.chunkEntries : (std::numeric_limits<std::size_t>::max)(), scratch, control, chunks);
        {
            // Reserve the reader and spill writer together to avoid workers
            // deadlocking while each holds a reader and waits for a writer.
            StreamPermit permit(control, 2);
            CountFile(corpus[job.file], words, counts, control, &job.range);
            counts.Flush();
        }
        plans[job.file].parts[job.part] = Merge(std::move(chunks), false, scratch, control);
    });
    RunWorkers(corpus.size(), workers, control, [&](std::size_t index) {
        const auto& path = corpus[index]; auto& plan = plans[index];
        if (!SameSignature(plan.signature, SignatureFor(path))) throw std::runtime_error("Corpus file changed while processing: " + path.u8string());
        if (plan.merged.empty()) {
            plan.merged = Merge(std::move(plan.parts), false, scratch, control);
            if (options.useCache) { StreamPermit permit(control, 2); SaveCache(plan.destination, plan.merged, plan.identity, control); }
            ++control.files;
        }
        if (!SameSignature(plan.signature, SignatureFor(path))) throw std::runtime_error("Corpus file changed while processing: " + path.u8string());
    });
    std::vector<fs::path> counted; counted.reserve(plans.size()); for (auto& plan : plans) counted.push_back(std::move(plan.merged));
    combined = Merge(std::move(counted), false, scratch, control);
    control.Report(options.lowMemory ? "Ranking n-grams on disk" : "Ranking n-grams", 70);
    std::array<std::vector<fs::path>, 3> rankingChunks;
    std::array<std::vector<Entry>, 3> ranking;
    std::array<std::uint64_t, 3> rankingBytes{};
    auto flushRank = [&](unsigned order) {
        auto& entries = ranking[order]; if (entries.empty()) return;
        std::sort(entries.begin(), entries.end(), Frequency); const auto path = scratch.File(); std::ofstream output(path, std::ios::binary);
        if (!output) throw std::runtime_error("Cannot create ranking workspace.");
        for (const auto& entry : entries) WriteEntry(output, entry); Finish(output, path); rankingChunks[order].push_back(path); entries.clear(); rankingBytes[order] = 0;
    };
    std::ifstream counts(combined, std::ios::binary); if (!counts) throw std::runtime_error("Cannot open merged count workspace.");
    Entry entry;
    while (ReadEntry(counts, entry)) {
        control.Check(); const auto order = static_cast<unsigned char>(entry.key[0]);
        ++sizes[order]; rankingBytes[order] += 96 + entry.key.capacity(); ranking[order].push_back(std::move(entry));
        if ((options.lowMemory && ranking[order].size() >= options.chunkEntries) || rankingBytes[order] >= workingBytes / 4) flushRank(order);
    }
    for (unsigned order = 0; order < 3; ++order) { flushRank(order); ranked[order] = Merge(std::move(rankingChunks[order]), true, scratch, control); }
    if (options.useCache && options.useTrainingCache) SaveTraining(cache, trainingIdentity, {combined, ranked, sizes}, control);
    }
    for (unsigned order = 0; order < 3; ++order) if (options.limits[order] && !*options.limits[order]) sizes[order] = 0;
    if (!options.maximumBytes) for (unsigned order = 0; order < 3; ++order) if (options.limits[order]) sizes[order] = (std::min)(sizes[order], *options.limits[order]);
    control.Report("Building trie", 80);
    auto trie = (options.maximumBytes || !triePath.empty()) ? std::make_unique<Trie>(options.compact) : nullptr;
    Result result; result.files = corpus.size(); result.cacheHits = hits.load(); result.output = triePath; result.effectiveWorkers = workers; result.trainingCacheHit = trainingCacheHit;
    if (options.format == ModelFormat::Radix) {
        auto profileOptions = options; profileOptions.memoryBytes = workingBytes;
        return GenerateRadix(profileOptions, combined, ranked, trainingIdentity, cache, scratch, control, triePath, jsonPath, std::move(result));
    }
    std::array<std::ifstream, 3> streams; for (unsigned order = 0; order < 3; ++order) { streams[order].open(ranked[order], std::ios::binary); if (!streams[order]) throw std::runtime_error("Cannot read ranked n-grams."); }
    std::array<std::vector<Entry>, 3> selected; std::array<std::uint64_t, 3> positions{};
    const auto total = sizes[0] + sizes[1] + sizes[2]; std::uint64_t considered = 0;
    auto consider = [&](unsigned order) {
        control.Check(); Entry candidate; if (!ReadEntry(streams[order], candidate)) throw std::runtime_error("Ranked n-gram stream ended early.");
        ++positions[order]; ++considered;
        if (!trie || trie->Add(candidate, order == 0, options.maximumBytes)) { ++result.counts[order]; if (!jsonPath.empty()) selected[order].push_back(std::move(candidate)); }
        if (!(considered & 511)) control.Report("Building trie", 80 + static_cast<unsigned>(total ? 17.0 * considered / total : 17));
    };
    if (options.maximumBytes) {
        auto until = [&](const std::array<std::uint64_t, 3>& ends) {
            // Unique compact-format grams always need at least one terminal
            // varint byte. Unlike v2, a completely full v3 cannot accept a later
            // zero-growth terminal, so there is no reason to scan the tail.
            while ((!options.compact || trie->Size() < *options.maximumBytes) &&
                   (positions[0] < ends[0] || positions[1] < ends[1] || positions[2] < ends[2]))
                for (unsigned order = 0; order < 3; ++order) for (unsigned attempt = 0; attempt < (order ? 1u : 4u) && positions[order] < ends[order]; ++attempt) consider(order);
        };
        std::array<std::uint64_t, 3> targets{};
        for (unsigned order = 0; order < 3; ++order) targets[order] = (std::min)(sizes[order], options.limits[order].value_or(0));
        until(targets); until(sizes);
    } else for (unsigned order = 0; order < 3; ++order) while (positions[order] < sizes[order]) consider(order);
    if (!triePath.empty()) { AtomicWrite(triePath, control, [&](std::ostream& output) { trie->Write(output, control); }); result.trieBytes = trie->Size(); }
    if (!jsonPath.empty()) { control.Report("Writing JSON", 97); AtomicWrite(jsonPath, control, [&](std::ostream& output) { WriteJson(selected, output, control); }); }
    // Publication is the commit boundary. A late Stop must not turn a
    // successfully published model into a reported cancellation.
    control.Report("Complete", 100, false); return result;
}
}
