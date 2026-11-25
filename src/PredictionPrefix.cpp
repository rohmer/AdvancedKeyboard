// Ported from ESPOS components/ngram_trie; see PROVENANCE.md.
#include "PredictionPrefix.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <limits>

namespace espos::keyboard::prediction {
namespace {

constexpr std::uint8_t space = ' ';

bool Beats(std::uint64_t mass, std::string_view word, const Suggestion& other) noexcept
{
    return mass > other.mass || (mass == other.mass && word < other.Word());
}

std::uint32_t AfterWord(const Model& model, std::uint32_t node, std::string_view word) noexcept
{
    if (node == Model::kNotFound) return node;
    node = model.FollowFrom(node, word);
    return node == Model::kNotFound ? node : model.FindChild(node, space);
}

std::size_t Collect(const Model& model, PrefixScratch& scratch, std::uint32_t root,
                    std::string_view prefix, Suggestion* output, std::size_t seen,
                    std::size_t limit) noexcept
{
    if (root == Model::kNotFound || seen == limit) return seen;
    Model::Cursor lookup;
    std::uint32_t index = model.FollowFrom(root, prefix);
    Node node;
    if (index == Model::kNotFound || !model.GetNode(index, node, lookup)) return seen;

    if (!prefix.empty()) std::memcpy(scratch.path.data(), prefix.data(), prefix.size());
    std::size_t depth = prefix.size();
    std::size_t count = 0;
    const std::size_t capacity = limit - seen;
    for (;;) {
        PrefixScratch::Frame& frame = scratch.frames[depth];
        frame = {};
        const std::string_view word{scratch.path.data(), depth};
        if (count < capacity || Beats(node.mass, word, scratch.ranked[count - 1])) {
            std::uint64_t mass = node.terminal;
            const auto space_id = model.FindChild(index, space, lookup);
            Node after_word;
            if (space_id != Model::kNotFound && model.GetNode(space_id, after_word, lookup)) mass += after_word.mass;

            const auto* already_emitted = std::find_if(output, output + seen,
                [word](const Suggestion& item) { return word == item.Word(); });
            if (depth != 0 && mass > 0 && already_emitted == output + seen) {
                std::size_t position = 0;
                while (position < count && !Beats(mass, word, scratch.ranked[position])) ++position;
                if (position < capacity) {
                    count = std::min(count + 1, capacity);
                    for (std::size_t i = count - 1; i > position; --i) scratch.ranked[i] = scratch.ranked[i - 1];
                    auto& candidate = scratch.ranked[position];
                    std::memcpy(candidate.text.data(), word.data(), depth);
                    candidate.mass = mass;
                    candidate.length = static_cast<std::uint16_t>(depth);
                }
            }

            if (depth < kSuggestionBytes) frame = {node.first_child, node.child_count};
        }

        for (;;) {
            auto& pending = scratch.frames[depth];
            if (!pending.children_left) {
                if (depth == prefix.size()) {
                    std::copy_n(scratch.ranked.begin(), count, output + seen);
                    return seen + count;
                }
                --depth;
                continue;
            }
            index = pending.next_child; pending.next_child += model.FormatVersion() == 4 ? 256 : 1; --pending.children_left;
            if (!model.GetNode(index, node, lookup) || node.label == space) continue;
            scratch.path[depth++] = static_cast<char>(node.label);
            break;
        }
    }
}

bool HasObserved(const Model& model, uint32_t context, std::string_view word) noexcept
{
    if (context == Model::kNotFound) return false;
    const auto id = model.FollowFrom(context, word); Node node{};
    return id != Model::kNotFound && model.GetNode(id, node, false) && (node.score_flags & 2);
}

bool ScoreBeats(double score, std::string_view word, double other_score, const Suggestion& other) noexcept
{
    return score > other_score + 1e-12 || (std::abs(score - other_score) <= 1e-12 && word < other.Word());
}

// A backoff model partitions its visible words into disjoint layers: observed
// trigrams, remaining observed bigrams, then remaining unigrams. Backoff is a
// constant per layer. Stored probability bounds therefore support exact top-k
// traversal without scanning every spelling or pruning by unrelated raw counts.
void CollectScored(const Model& model, PrefixScratch& scratch, uint32_t root, std::string_view prefix,
                   double factor, uint32_t exclude3, uint32_t exclude2, std::size_t& count,
                   std::size_t limit) noexcept
{
    if (root == Model::kNotFound || !std::isfinite(factor)) return;
    Model::Cursor lookup;
    auto id = model.FollowFrom(root, prefix); Node node{};
    if (id == Model::kNotFound || !model.GetNode(id, node, lookup)) return;
    if (!prefix.empty()) std::memcpy(scratch.path.data(), prefix.data(), prefix.size());
    std::size_t depth = prefix.size();
    for (;;) {
        auto& frame = scratch.frames[depth]; frame = {};
        const auto bound = factor + Model::DecodeLogScore(node.maximum_log_probability);
        // Equal scores can improve lexical tie order, so equality is retained.
        if (std::isfinite(bound) && (count < limit || bound + 1e-12 >= scratch.ranked_scores[count - 1])) {
            const std::string_view word{scratch.path.data(), depth};
            if (depth && (node.score_flags & 2) && !HasObserved(model, exclude3, word) && !HasObserved(model, exclude2, word)) {
                const auto score = factor + Model::DecodeLogScore(node.log_probability);
                if (std::isfinite(score)) {
                    std::size_t position = 0;
                    while (position < count && !ScoreBeats(score, word, scratch.ranked_scores[position], scratch.ranked[position])) ++position;
                    if (position < limit) {
                        count = std::min(count + 1, limit);
                        for (auto i = count - 1; i > position; --i) {
                            scratch.ranked[i] = scratch.ranked[i - 1]; scratch.ranked_scores[i] = scratch.ranked_scores[i - 1];
                        }
                        auto& candidate = scratch.ranked[position];
                        std::memcpy(candidate.text.data(), word.data(), depth);
                        candidate.length = static_cast<uint16_t>(depth);
                        const auto probability = std::exp(score);
                        candidate.mass = probability >= 1 ? UINT32_MAX : static_cast<uint32_t>(std::llround(probability * UINT32_MAX));
                        scratch.ranked_scores[position] = score;
                    }
                }
            }
            if (depth < kSuggestionBytes) frame = {node.first_child, node.child_count};
        }
        for (;;) {
            auto& pending = scratch.frames[depth];
            if (!pending.children_left) {
                if (depth == prefix.size()) return;
                --depth; continue;
            }
            id = pending.next_child; pending.next_child += model.FormatVersion() == 4 ? 256 : 1; --pending.children_left;
            if (!model.GetNode(id, node, lookup) || node.label == space) continue;
            scratch.path[depth++] = static_cast<char>(node.label); break;
        }
    }
}

double HistoryBackoff(const Model& model, uint32_t history) noexcept
{
    Node node{};
    return history != Model::kNotFound && model.GetNode(history, node, false) && (node.score_flags & 4)
        ? Model::DecodeLogScore(node.log_backoff) : 0;
}

std::size_t SuggestScored(const Model& model, PrefixScratch& scratch, std::string_view prefix,
                          std::string_view previous, std::string_view older, Suggestion* output, std::size_t limit) noexcept
{
    uint32_t history2 = Model::kNotFound, history3 = Model::kNotFound;
    if (!previous.empty()) {
        history2 = model.FindPrefix(previous);
        if (!older.empty()) {
            const auto older_space = AfterWord(model, 0, older);
            if (older_space != Model::kNotFound) history3 = model.FollowFrom(older_space, previous);
        }
    }
    const auto context2 = history2 == Model::kNotFound ? Model::kNotFound : model.FindChild(history2, space);
    const auto context3 = history3 == Model::kNotFound ? Model::kNotFound : model.FindChild(history3, space);
    std::size_t count = 0;
    CollectScored(model, scratch, context3, prefix, 0, Model::kNotFound, Model::kNotFound, count, limit);
    const auto factor3 = HistoryBackoff(model, history3);
    CollectScored(model, scratch, context2, prefix, factor3, context3, Model::kNotFound, count, limit);
    CollectScored(model, scratch, 1, prefix, factor3 + HistoryBackoff(model, history2), context3, context2, count, limit);
    std::copy_n(scratch.ranked.begin(), count, output); return count;
}

} // namespace

std::size_t Suggest(const Model& model, PrefixScratch& scratch, std::string_view prefix,
                    std::string_view previous, std::string_view older, Suggestion* output, std::size_t limit) noexcept
{
    if (!model.IsOpen() || !output || limit == 0 || prefix.size() > kSuggestionBytes ||
        prefix.find(' ') != std::string_view::npos) return 0;

    limit = std::min(limit, kSuggestionCount);
    if (model.HasLanguageScores()) return SuggestScored(model, scratch, prefix, previous, older, output, limit);
    std::size_t emitted = 0;
    const auto root = static_cast<std::uint32_t>(Root::Combined);
    if (!previous.empty() && !older.empty()) {
        const auto context = AfterWord(model, AfterWord(model, root, older), previous);
        emitted = Collect(model, scratch, context, prefix, output, emitted, limit);
    }
    if (emitted < limit && !previous.empty()) {
        emitted = Collect(model, scratch, AfterWord(model, root, previous), prefix, output, emitted, limit);
    }
    return Collect(model, scratch, static_cast<std::uint32_t>(Root::Unigrams), prefix, output, emitted, limit);
}

} // namespace espos::keyboard::prediction
