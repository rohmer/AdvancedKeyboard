#include "TrainingModel.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace LanguageModels::Training;
namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void Near(double actual, double expected, double tolerance = 1e-10) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance)
        throw std::runtime_error("Probability differs: actual=" + std::to_string(actual) + ", expected=" + std::to_string(expected));
}
struct Key {
    unsigned order;
    std::array<std::uint32_t, 3> words;
    bool operator<(const Key& other) const { return order != other.order ? order < other.order : words < other.words; }
};
using Counts = std::map<Key, std::uint64_t>;
void Sentence(Counts& counts, const std::vector<std::uint32_t>& sentence, std::uint64_t times = 1) {
    for (std::size_t i = 0; i < sentence.size(); ++i) for (unsigned order = 1; order <= 3 && order <= i + 1; ++order) {
        Key key{order, {}};
        for (unsigned j = 0; j < order; ++j) key.words[j] = sentence[i + 1 - order + j];
        counts[key] += times;
    }
}
std::vector<NGram> Records(const Counts& counts) {
    std::vector<NGram> result;
    for (const auto& entry : counts) {
        NGram row; row.order = static_cast<std::uint8_t>(entry.first.order); row.words = entry.first.words; row.count = entry.second;
        result.push_back(row);
    }
    return result;
}
std::vector<ScoredNGram> Train(const std::vector<NGram>& counts, std::uint32_t vocabulary,
                              Options options, Summary* report = nullptr) {
    std::vector<ScoredNGram> result;
    const auto summary = TrainModifiedKneserNey([&](const CountVisitor& visit) { for (const auto& row : counts) visit(row); },
        vocabulary, options, [&](const ScoredNGram& row) { result.push_back(row); });
    if (report) *report = summary;
    return result;
}
const ScoredNGram& Find(const std::vector<ScoredNGram>& records, unsigned order, std::array<std::uint32_t, 3> words) {
    const auto found = std::find_if(records.begin(), records.end(), [&](const ScoredNGram& row) { return row.order == order && row.words == words; });
    if (found == records.end()) throw std::runtime_error("Expected scored n-gram is absent");
    return *found;
}
void Normalized(const std::vector<ScoredNGram>& records, const std::vector<std::uint32_t>& vocabulary) {
    Model model(records);
    double sum = 0;
    for (auto word : vocabulary) sum += model.Probability(word, 0, 0, 0);
    Near(sum, 1);
    for (auto previous : vocabulary) {
        sum = 0;
        for (auto word : vocabulary) sum += model.Probability(word, previous, 0, 1);
        Near(sum, 1);
        for (auto older : vocabulary) {
            sum = 0;
            for (auto word : vocabulary) sum += model.Probability(word, previous, older, 2);
            Near(sum, 1);
        }
    }
}
void ExpectFailure(const std::function<void()>& work) {
    bool failed = false;
    try { work(); } catch (const std::exception&) { failed = true; }
    Require(failed, "Invalid model unexpectedly succeeded");
}
void Continuations(const Options& options) {
    Counts counts;
    // Hong Kong is frequent in just one context, while glasses has three
    // different left contexts. KN must distinguish diversity from frequency.
    Sentence(counts, {1, 2}, 1000);
    Sentence(counts, {3, 6}, 10); Sentence(counts, {4, 6}, 10); Sentence(counts, {5, 6}, 10);
    auto scores = Train(Records(counts), 7, options);
    Model model(scores);
    Require(model.Probability(6, 0, 0, 0) > model.Probability(2, 0, 0, 0), "Continuation counts were replaced with raw frequency");
    Require(model.Probability(0, 0, 0, 0) > 0, "Unknown bucket has no probability");
    Near(model.Probability(999, 999, 999), model.Probability(0, 0, 0, 0));
    Normalized(scores, {0, 1, 2, 3, 4, 5, 6});
}
void TrigramsAndDiscounts(const Options& options) {
    Counts counts;
    Sentence(counts, {1, 2, 3}, 100);
    Sentence(counts, {4, 2, 3}, 1);
    Sentence(counts, {5, 2, 6}, 1);
    Summary report;
    auto scores = Train(Records(counts), 7, options, &report);
    Model model(scores);
    const auto& discount = report.discounts[1];
    const double lambda = (discount(2) + discount(1)) / 3;
    const double expected = (2 - discount(2)) / 3 + lambda * model.Probability(3, 0, 0, 0);
    Near(model.Probability(3, 2, 0, 1), expected);
    Require(model.Probability(3, 2, 0, 1) < .8, "Lower-order KN used repeated trigram counts instead of distinct left contexts");
    Require(model.Probability(3, 2, 1, 2) > .9, "Observed trigram did not retain its high probability");
    Require(model.Probability(6, 2, 1, 2) > 0, "Unseen trigram lost backoff probability");
    Normalized(scores, {0, 1, 2, 3, 4, 5, 6});

    Counts bins;
    for (unsigned i = 1; i <= 4; ++i) Sentence(bins, {1, 2, i + 2}, i);
    Train(Records(bins), 7, options, &report);
    Near(report.discounts[2].one, 1.0 / 3);
    Near(report.discounts[2].two, 1);
    Near(report.discounts[2].threePlus, 5.0 / 3);
}
ScoredNGram Scored(unsigned order, std::array<std::uint32_t, 3> words, double probability, std::uint64_t count = 1) {
    ScoredNGram row; row.order = static_cast<std::uint8_t>(order); row.words = words; row.probability = probability; row.count = count; return row;
}
void Pruning(const Options& options) {
    std::vector<ScoredNGram> rows{Scored(1, {0, 0, 0}, .2, 0), Scored(1, {1, 0, 0}, .4),
        Scored(1, {2, 0, 0}, .4), Scored(2, {1, 1, 0}, .1), Scored(2, {1, 2, 0}, 0, 0),
        Scored(3, {1, 2, 2}, .9)};
    Near(ReestimatePrunedBackoffs(rows), .2);
    Near(Find(rows, 1, {1, 0, 0}).backoff, 1.5);
    Near(Find(rows, 2, {1, 2, 0}).probability, .6);
    Normalized(rows, {0, 1, 2});
    Require(Find(rows, 1, {1, 0, 0}).backoff > 1, "Pruned backoff weights were incorrectly capped at one");

    Counts counts;
    Sentence(counts, {1, 2, 3}, 10); Sentence(counts, {4, 2, 5}, 5);
    auto original = Train(Records(counts), 6, options);
    const double retainedMass = Find(original, 1, {1, 0, 0}).probability + Find(original, 1, {2, 0, 0}).probability;
    std::vector<ScoredNGram> subset{Find(original, 1, {1, 0, 0}), Find(original, 1, {2, 0, 0})};
    Near(ReestimatePrunedBackoffs(subset), 1 - retainedMass);
    Normalized(subset, {0, 1, 2});
    Model model(subset);
    Near(model.Probability(5), model.Probability(0));
    auto invalid = subset; invalid.push_back(Scored(3, {1, 2, 1}, .4));
    ExpectFailure([&] { ReestimatePrunedBackoffs(invalid); });
    invalid = subset; invalid.push_back(subset.front());
    ExpectFailure([&] { ReestimatePrunedBackoffs(invalid); });
    invalid = subset; invalid.front().probability = (std::numeric_limits<double>::infinity)();
    ExpectFailure([&] { ReestimatePrunedBackoffs(invalid); });
}
void ExternalSortAndValidation(Options options) {
    Counts counts;
    Sentence(counts, {1, 2, 3}, 10); Sentence(counts, {4, 2, 5}, 5);
    const auto sorted = Records(counts);
    const auto expected = Train(sorted, 6, options);
    std::vector<NGram> unsorted;
    for (auto row : sorted) {
        if (row.count > 1) { const auto first = row.count / 2; row.count -= first; unsorted.push_back(row); row.count = first; }
        unsorted.push_back(row);
    }
    std::reverse(unsorted.begin(), unsorted.end());
    ExpectFailure([&] { Train(unsorted, 6, options); });
    options.sortedUniqueInput = false;
    const auto actual = Train(unsorted, 6, options);
    Require(actual.size() == expected.size(), "Duplicate aggregation changed output keys");
    for (std::size_t i = 0; i < actual.size(); ++i) {
        Require(actual[i].words == expected[i].words && actual[i].order == expected[i].order && actual[i].count == expected[i].count,
                "Unsorted input changed aggregated counts");
        Near(actual[i].probability, expected[i].probability); Near(actual[i].backoff, expected[i].backoff);
    }
    options.sortedUniqueInput = true;
    options.memoryBytes = 1024 * 1024;
    std::uint64_t outputRows = 0; double unigramMass = 0;
    const auto report = TrainModifiedKneserNey([&](const CountVisitor& visit) {
        NGram row;
        for (std::uint32_t a = 1; a < 41; ++a) { row = {}; row.order = 1; row.words[0] = a; row.count = 100; visit(row); }
        for (std::uint32_t a = 1; a < 41; ++a) for (std::uint32_t b = 1; b < 41; ++b) {
            row = {}; row.order = 2; row.words = {a, b, 0}; row.count = 100; visit(row);
        }
        for (std::uint32_t a = 1; a < 41; ++a) for (std::uint32_t b = 1; b < 41; ++b) for (std::uint32_t c = 1; c < 41; ++c) {
            row = {}; row.order = 3; row.words = {a, b, c}; row.count = 1; visit(row);
        }
    }, 41, options, [&](const ScoredNGram& row) {
        ++outputRows;
        Require(std::isfinite(row.probability) && row.probability > 0 && row.probability <= 1, "Invalid spilled model probability");
        if (row.order == 1) unigramMass += row.probability;
    });
    Require(outputRows == 41 + 1600 + 64000, "Spilled training lost scored records");
    Require(report.externalMergeGroups > 0, "Small RAM budget did not exercise external merge sorting");
    Require(report.peakWorkingBytes <= options.memoryBytes, "Training buffers exceeded their RAM budget");
    Near(unigramMass, 1);
    ExpectFailure([&] { Train(sorted, 1000000, options); });
    auto invalid = sorted; invalid.front().count = 0;
    ExpectFailure([&] { Train(invalid, 6, options); });
    invalid = sorted; invalid.front().words[0] = 999;
    ExpectFailure([&] { Train(invalid, 6, options); });
    unsigned checks = 0;
    options.check = [&] { if (++checks == 3) throw std::runtime_error("Cancelled"); };
    ExpectFailure([&] { Train(sorted, 6, options); });
}
}
int main(int argc, char** argv) {
    try {
        Options options; options.memoryBytes = 4 * 1024 * 1024;
        options.scratchDirectory = argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() / "uicreator-mkn-tests";
        Continuations(options); TrigramsAndDiscounts(options); Pruning(options); ExternalSortAndValidation(options);
        std::cout << "MKN training passed: continuation counts, derived discounts, normalized unseen contexts, pruning, unknown bucket, bounded spill/merge, invalid input and cancellation\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "MKN training test failed: " << error.what() << '\n'; return 1; }
}
