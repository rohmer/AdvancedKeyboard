#pragma once

#include "PredictionModel.h"
#include "espos_keyboard/Prediction.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace espos::keyboard::prediction {

struct PrefixScratch {
    // Count-based frames retain the original 8-byte footprint. Radix branch
    // siblings use a format-wide stride; an interior span byte has one child,
    // so the unused post-visit increment never needs a per-frame stride.
    struct Frame { std::uint32_t next_child, children_left; };
    std::array<Frame, kSuggestionBytes + 1> frames;
    std::array<char, kSuggestionBytes> path;
    std::array<Suggestion, kSuggestionCount> ranked;
    std::array<double, kSuggestionCount> ranked_scores;
};

// All inputs and outputs use the model's folded UTF-8 vocabulary. The caller
// supplies the latest two words in the current sentence and an output array.
std::size_t Suggest(const Model& model, PrefixScratch& scratch, std::string_view folded_prefix,
                    std::string_view folded_previous, std::string_view folded_older,
                    Suggestion* output, std::size_t limit = kSuggestionCount) noexcept;

} // namespace espos::keyboard::prediction
