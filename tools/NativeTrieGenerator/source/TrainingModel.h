#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace LanguageModels::Training {

// IDs are deterministic vocabulary IDs. ID zero is the unknown-word bucket;
// the vocabulary size includes it. Trailing positions in an n-gram are zero.
struct NGram {
    std::array<std::uint32_t, 3> words{};
    std::uint64_t count = 0;
    std::uint8_t order = 0; // 1, 2 or 3
};
struct ScoredNGram : NGram {
    // Observed probability already includes lower-order interpolation. A
    // missing n-gram uses backoff(history) * probability(shorter history).
    double probability = 0;
    double backoff = 1;
    // Positive training log-likelihood gain over the lower-order model. It is
    // a candidate-selection heuristic, not a claim about held-out quality.
    double informationGain = 0;
};
using CountVisitor = std::function<void(const NGram&)>;
using CountScanner = std::function<void(const CountVisitor&)>;
using ScoreVisitor = std::function<void(const ScoredNGram&)>;

struct Options {
    std::size_t memoryBytes = 256u * 1024u * 1024u;
    std::filesystem::path scratchDirectory;
    // The normal merged count stream is unique and sorted by (order, IDs).
    // Set false for an unsorted/duplicate source; external sorting aggregates
    // it without retaining the complete n-gram set in memory.
    bool sortedUniqueInput = true;
    std::function<void()> check; // cancellation; may throw
};
struct Discount {
    double one = .75, two = .75, threePlus = .75;
    double operator()(std::uint64_t count) const noexcept;
};
struct Summary {
    std::array<std::uint64_t, 3> records{};
    std::array<Discount, 3> discounts{};
    std::uint64_t vocabularySize = 0;
    std::uint64_t peakWorkingBytes = 0;
    std::uint64_t temporaryBytesWritten = 0;
    std::uint64_t externalSortRuns = 0, externalMergeGroups = 0;
    double unknownProbability = 0;
};

// Exact modified Kneser-Ney continuation statistics are computed from the
// complete, globally aggregated counts before profile pruning. At maximum
// order three, lower-order bigram counts are distinct left-context counts from
// trigrams; unigram counts are distinct left contexts from bigrams. Sparse
// count-of-counts bins use a documented .75 absolute-discount fallback.
// The caller's existing sentence-reset/tokenization contract is preserved:
// no synthetic BOS/EOS records are invented here. This is a conditional word
// predictor over within-sentence counts, not a KenLM-equivalent sentence model.
// Output is sorted by (order, IDs), and includes ID-zero unigram metadata.
// Uses O(vocabulary) base probabilities plus bounded external-sort buffers.
Summary TrainModifiedKneserNey(const CountScanner& scan, std::uint32_t vocabularySize,
                              const Options& options, const ScoreVisitor& output);

// Normalize a selected profile without throwing away mass for unseen words.
// Retained unigram probabilities remain unchanged; omitted vocabulary mass is
// consolidated into ID zero. Higher-order backoffs are recalculated from the
// retained observed probabilities and the final lower-order distribution.
// Retained n-grams must have vocabulary/history closure. The function sorts
// records and rejects duplicates, missing dependencies, invalid probabilities
// and non-finite values. Returns the final unknown-bucket probability.
double ReestimatePrunedBackoffs(std::vector<ScoredNGram>& retained,
                               std::uint32_t unknownId = 0);

// Reference evaluator for tests/held-out evaluation of a retained profile.
// Unknown target IDs map to the unknown bucket; unknown histories back off.
// It does not own/copy the sorted profile and remains valid until mutation.
class Model {
public:
    explicit Model(const std::vector<ScoredNGram>& records, std::uint32_t unknownId = 0);
    double Probability(std::uint32_t target, std::uint32_t previous = 0,
                       std::uint32_t older = 0, unsigned historyLength = 2) const;
private:
    const std::vector<ScoredNGram>* records_;
    std::uint32_t unknown_;
    const ScoredNGram* Find(unsigned order, std::array<std::uint32_t, 3> words) const;
};
}
