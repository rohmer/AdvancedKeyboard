// Ported from ESPOS components/ngram_trie; see PROVENANCE.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace espos::keyboard::prediction {

enum class Root : uint8_t { Combined = 0, Unigrams = 1 };

struct Node {
    uint64_t mass;
    uint64_t terminal;
    uint32_t first_child;
    uint16_t child_count;
    uint8_t label;
    // Radix branches address physical edge slots; interior bytes use stride 1.
    uint16_t child_stride = 1;
    int16_t log_probability = 0;
    int16_t log_backoff = 0;
    int16_t maximum_log_probability = INT16_MIN;
    uint8_t score_flags = 0;
};

// Borrows immutable NGTR v2, NGTR v3/NGIX v1, or radix NGTR v4/NGIX v2.
// Ranges remain readable until Close. No expanded graph or heap is owned.
// Radix node IDs are opaque virtual byte handles, not dense ordinals. Children
// are first_child + ordinal*child_stride. NodeCount is the expanded logical
// count; PhysicalNodeCount is the number of serialized compressed edges.
class Model {
public:
    static constexpr uint32_t kNotFound = UINT32_MAX;

    class Cursor {
    public:
        // Legacy nodes are read in byte-BFS order. Radix cursors expand each
        // span in physical-edge BFS order; handles are never dense ordinals.
        // A failed read leaves out unchanged.
        bool Next(Node& out);

    private:
        friend class Model;
        const Model* model_ = nullptr;
        uint32_t node_index_ = 0;
        uint32_t record_offset_ = 0;
        uint32_t next_child_ = 0;
        uint32_t mass_offset_ = 0;
        bool include_mass_ = true;
        Node radix_node_{};
        const uint8_t* radix_span_ = nullptr;
        uint32_t radix_id_ = 0;
        uint16_t radix_length_ = 0, radix_position_ = 0;
        bool radix_ready_ = false;
    };

    // Validates both ranges in place. On failure the model is closed.
    bool Open(const void* model, size_t model_size, const void* index, size_t index_size);
    void Close();
    bool IsOpen() const { return records_ != nullptr; }
    uint32_t NodeCount() const { return logical_node_count_; }
    uint32_t PhysicalNodeCount() const { return node_count_; }
    uint32_t FormatVersion() const { return version_; }
    bool HasLanguageScores() const { return language_scores_; }
    static double DecodeLogScore(int16_t value) noexcept;
    // Natural log of calibrated P(word|previous,older), including unknown-word
    // and missing-history backoff. Returns -infinity on an unscored model.
    double ConditionalLogProbability(std::string_view word, std::string_view previous = {},
                                     std::string_view older = {}) const noexcept;

    // Set include_mass to false for traversal using only exact terminal counts.
    // The returned mass is then zero and probability bounds are not loaded.
    // Close or Open invalidates existing cursors.
    Cursor ReadFrom(uint32_t node_index, bool include_mass = true) const;
    bool GetNode(uint32_t node_index, Node& out, bool include_mass = true) const;
    // Caller-owned lookup cursor reuses decoded radix spans and nearby physical
    // records. Initialize it once per query; Open/Close invalidates it like any
    // other cursor. A mass-bearing cursor also serves no-mass lookups.
    bool GetNode(uint32_t node_index, Node& out, Cursor& lookup, bool include_mass = true) const;
    uint32_t FindChild(uint32_t node_index, uint8_t label) const;
    uint32_t FindChild(uint32_t node_index, uint8_t label, Cursor& lookup) const;
    uint32_t FollowFrom(uint32_t node_index, std::string_view suffix) const;
    uint32_t FindPrefix(std::string_view prefix, Root root = Root::Combined) const;

private:
    uint32_t FindChildAndRead(Node& current, uint8_t label) const;
    uint32_t FindChildAndRead(Node& current, uint8_t label, Cursor& lookup) const;
    Cursor AtCheckpoint(uint32_t node_index, bool include_mass) const;
    bool Decode(Cursor& cursor, Node& out) const;
    bool DecodeRadixRecord(Cursor& cursor, Node& out, const uint8_t*& span, uint16_t& length) const;
    bool ReadLegacy2(uint32_t node_index, Node& out, bool include_mass) const;
    bool ValidateRadix() const;
    bool ValidateLegacy2() const;
    bool Validate() const;

    const uint8_t* records_ = nullptr;
    const uint8_t* checkpoints_ = nullptr;
    const uint8_t* masses_ = nullptr;
    uint32_t records_size_ = 0;
    uint32_t masses_size_ = 0;
    uint32_t node_count_ = 0;
    uint32_t logical_node_count_ = 0;
    uint32_t version_ = 0;
    bool language_scores_ = false;
};

}  // namespace espos::keyboard::prediction
