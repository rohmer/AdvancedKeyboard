#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>

namespace espos::keyboard::prediction {

// Offline encoder input. Negative scores mean "not supplied"; zero is an
// explicit zero probability/weight. Keys are normalized UTF-8 words joined by
// ASCII spaces. A unigram is also inserted under the independent spelling root.
struct RadixEntry {
    std::string_view key;
    std::uint64_t count = 0;
    double probability = -1;
    double backoff = -1;
};

struct RadixBuildOptions {
    bool smoothed = false;
    double unknown_probability = 0;
};

struct RadixEncodedModel {
    std::vector<std::uint8_t> trie;
    std::vector<std::uint8_t> index;
    std::uint32_t logical_nodes = 0;
    std::uint32_t physical_nodes = 0;
    std::size_t Bytes() const { return trie.size() + index.size(); }
};

// Counts and aggregate frequencies are exact uint64 values. Keys must be unique.
// Lengths/frequency overflow are checked before publication. Throws std::exception
// for malformed inputs/overflow. The returned lengths are the exact flash cost,
// including NGIX rather than only the NGTR payload.
RadixEncodedModel EncodeRadixModel(const std::vector<RadixEntry>& entries,
                                  const RadixBuildOptions& options = {});

// Re-encodes a validated unsmoothed model without changing vocabulary/counts.
// Requires the matching combined/unigram roots produced by Creator; rejects
// independently authored roots that cannot be represented by this entry API.
// The result can be used to compare lookup parity and structural compression.
RadixEncodedModel ConvertToRadix(const void* trie, std::size_t trie_size,
                                const void* index, std::size_t index_size);

} // namespace espos::keyboard::prediction
