// Ported from ESPOS components/ngram_trie; see PROVENANCE.md.
#include "PredictionModel.h"

#include <cstring>
#include <cmath>
#include <limits>

namespace espos::keyboard::prediction {
namespace {

// NGIX v1 uses a little-endian header: "NGIX", version, NGTR byte length,
// and node count. Each 32-node block has one checkpoint holding the NGTR
// record offset, next BFS child index, and range-relative mass offset at
// the start of the block. Its mass offset points into the stream after all
// checkpoints, where only nonleaf masses appear, in BFS order as ULEB128.
constexpr uint32_t kTrieHeaderSize = 16;
constexpr uint32_t kIndexHeaderSize = 16;
constexpr uint32_t kCheckpointSize = 12;
constexpr uint32_t kNodesPerCheckpoint = 32;
constexpr uint32_t kRadixStride = 256;
constexpr uint8_t kProbability = 2, kBackoff = 4;

uint32_t RadixHandle(uint32_t edge, uint16_t position = 0)
{
    return edge < 2 ? edge : 2 + (edge - 2) * kRadixStride + position;
}
uint32_t RadixEdge(uint32_t handle) { return handle < 2 ? handle : 2 + (handle - 2) / kRadixStride; }
uint16_t RadixPosition(uint32_t handle) { return handle < 2 ? 0 : static_cast<uint16_t>((handle - 2) % kRadixStride); }

Node RadixLogicalNode(Node node, uint32_t edge, const uint8_t* span, uint16_t length, uint16_t position)
{
    if (edge >= 2) {
        node.label = span[position];
        if (position + 1 < length) {
            node.terminal = 0; node.score_flags = 0;
            node.first_child = RadixHandle(edge, static_cast<uint16_t>(position + 1));
            node.child_count = 1; node.child_stride = 1;
            return node;
        }
    } else node.label = 0;
    node.first_child = node.child_count ? RadixHandle(node.first_child) : 0;
    node.child_stride = kRadixStride;
    return node;
}
double LogScore(int16_t value)
{
    return value == INT16_MIN ? -std::numeric_limits<double>::infinity() : value * (0.69314718055994530942 / 256.0);
}

uint32_t ReadLe32(const uint8_t* data)
{
    return uint32_t{data[0]} | (uint32_t{data[1]} << 8) |
           (uint32_t{data[2]} << 16) | (uint32_t{data[3]} << 24);
}

bool ReadVarUInt32(const uint8_t* bytes, uint32_t size, uint32_t& offset, uint32_t& value)
{
    value = 0;
    for (unsigned shift = 0; shift <= 28 && offset < size; shift += 7) {
        const uint8_t next = bytes[offset++];
        if (shift == 28 && next > 0x0f) {
            return false;
        }
        value |= (uint32_t{next} & 0x7f) << shift;
        if ((next & 0x80) == 0) {
            return shift == 0 || next != 0;
        }
    }
    return false;
}

bool ReadVarUInt64(const uint8_t* bytes, uint32_t size, uint32_t& offset, uint64_t& value)
{
    value = 0;
    for (unsigned shift = 0; shift <= 63 && offset < size; shift += 7) {
        const auto next = bytes[offset++];
        if (shift == 63 && next > 1) return false;
        value |= uint64_t(next & 127) << shift;
        if (!(next & 128)) return shift == 0 || next != 0;
    }
    return false;
}

int16_t ReadLe16(const uint8_t* bytes)
{
    const auto value = uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8);
    return value < 0x8000 ? static_cast<int16_t>(value) : static_cast<int16_t>(static_cast<int32_t>(value) - 0x10000);
}

}  // namespace

bool Model::Open(const void* model, size_t model_size, const void* index, size_t index_size)
{
    Close();
    if (model == nullptr || model_size < kTrieHeaderSize || model_size > UINT32_MAX || index_size > UINT32_MAX) {
        return false;
    }

    const auto* trie = static_cast<const uint8_t*>(model);
    if (std::memcmp(trie, "NGTR", 4) != 0) return false;
    const auto version = ReadLe32(trie + 4);
    const auto count = ReadLe32(trie + 8);
    if (version == 2) {
        if (count < 2 || ReadLe32(trie + 12) != 12 || model_size != 16 + uint64_t{count} * 12) return false;
        records_ = trie; records_size_ = static_cast<uint32_t>(model_size);
        node_count_ = logical_node_count_ = count; version_ = 2;
        if (ValidateLegacy2()) return true;
        Close(); return false;
    }
    if ((version != 3 && version != 4) || index == nullptr || index_size < kIndexHeaderSize) return false;
    const auto* sidecar = static_cast<const uint8_t*>(index);
    if ((version == 3 && ReadLe32(trie + 12) != 0) || std::memcmp(sidecar, "NGIX", 4) != 0 ||
        ReadLe32(sidecar + 4) != (version == 4 ? 2u : 1u) || ReadLe32(sidecar + 8) != model_size) {
        return false;
    }

    const size_t payload_size = model_size - kTrieHeaderSize;
    if (count < 2 || count > payload_size / 2 || ReadLe32(sidecar + 12) != count ||
        (version == 3 && payload_size > uint64_t{count} * 8) ||
        (version == 4 && (count > 0x00ffffffu || ReadLe32(trie + 12) < count ||
            ReadLe32(trie + 12) > 2 + uint64_t{count - 2} * 256))) {
        return false;
    }

    const size_t checkpoint_count = (count + kNodesPerCheckpoint - 1) / kNodesPerCheckpoint;
    const size_t mass_start = kIndexHeaderSize + checkpoint_count * kCheckpointSize;
    if (index_size < mass_start) {
        return false;
    }

    records_ = trie;
    checkpoints_ = sidecar + kIndexHeaderSize;
    masses_ = sidecar + mass_start;
    records_size_ = static_cast<uint32_t>(model_size);
    masses_size_ = static_cast<uint32_t>(index_size - mass_start);
    node_count_ = count;
    logical_node_count_ = version == 4 ? ReadLe32(trie + 12) : count;
    version_ = version;
    if (version == 4) {
        Cursor first; first.model_ = this; first.record_offset_ = 16; first.next_child_ = 2; first.include_mass_ = false;
        Node root; const uint8_t* span; uint16_t length;
        if (!DecodeRadixRecord(first, root, span, length)) { Close(); return false; }
        language_scores_ = (root.score_flags & kProbability) != 0;
    }
    if (version == 4 ? ValidateRadix() : Validate()) {
        if (version == 4) { Node root{}; GetNode(0, root, false); language_scores_ = (root.score_flags & kProbability) != 0; }
        return true;
    }
    Close();
    return false;
}

void Model::Close()
{
    *this = Model{};
}

double Model::DecodeLogScore(int16_t value) noexcept { return LogScore(value); }

bool Model::Cursor::Next(Node& out)
{
    if (model_ == nullptr) return false;
    if (model_->version_ == 2) {
        if (!model_->ReadLegacy2(node_index_, out, include_mass_)) return false;
        ++node_index_; return true;
    }
    if (model_->version_ != 4) return model_->Decode(*this, out);
    if (!radix_ready_) {
        radix_id_ = node_index_;
        if (!model_->DecodeRadixRecord(*this, radix_node_, radix_span_, radix_length_)) return false;
        radix_position_ = 0; radix_ready_ = true;
    }
    out = RadixLogicalNode(radix_node_, radix_id_, radix_span_, radix_length_, radix_position_);
    if (radix_id_ >= 2) {
        ++radix_position_;
        if (radix_position_ == radix_length_) radix_ready_ = false;
    } else radix_ready_ = false;
    return true;
}

Model::Cursor Model::AtCheckpoint(uint32_t node_index, bool include_mass) const
{
    const uint32_t block = node_index / kNodesPerCheckpoint;
    const uint8_t* checkpoint = checkpoints_ + block * kCheckpointSize;
    Cursor cursor;
    cursor.model_ = this;
    cursor.node_index_ = block * kNodesPerCheckpoint;
    cursor.record_offset_ = ReadLe32(checkpoint);
    cursor.next_child_ = ReadLe32(checkpoint + 4);
    cursor.mass_offset_ = ReadLe32(checkpoint + 8);
    cursor.include_mass_ = include_mass;
    return cursor;
}

Model::Cursor Model::ReadFrom(uint32_t node_index, bool include_mass) const
{
    const auto physical = version_ == 4 ? RadixEdge(node_index) : node_index;
    if (!IsOpen() || physical >= node_count_) {
        return {};
    }
    if (version_ == 2) { Cursor cursor; cursor.model_ = this; cursor.node_index_ = node_index; cursor.include_mass_ = include_mass; return cursor; }
    Cursor cursor = AtCheckpoint(physical, include_mass);
    Node skipped;
    while (cursor.node_index_ < physical) {
        const uint8_t* span; uint16_t length;
        if (!(version_ == 4 ? DecodeRadixRecord(cursor, skipped, span, length) : Decode(cursor, skipped))) {
            return {};
        }
    }
    if (version_ == 4) {
        cursor.radix_id_ = physical;
        if (!DecodeRadixRecord(cursor, cursor.radix_node_, cursor.radix_span_, cursor.radix_length_)) return {};
        const auto position = RadixPosition(node_index);
        if (physical >= 2 && position >= cursor.radix_length_) return {};
        cursor.radix_position_ = position; cursor.radix_ready_ = true;
    }
    return cursor;
}

bool Model::GetNode(uint32_t node_index, Node& out, bool include_mass) const
{
    return ReadFrom(node_index, include_mass).Next(out);
}

bool Model::GetNode(uint32_t node_index, Node& out, Cursor& lookup, bool include_mass) const
{
    if (version_ != 4) return GetNode(node_index, out, include_mass);
    const auto physical = RadixEdge(node_index);
    if (!IsOpen() || physical >= node_count_) { lookup = {}; return false; }
    const bool reusable = lookup.model_ == this && lookup.radix_span_ && (lookup.include_mass_ || !include_mass);
    const bool load_mass = include_mass || (reusable && lookup.include_mass_);
    if (!reusable || physical != lookup.radix_id_) {
        // Saved offsets point immediately after the cached physical record.
        // Use them only when they are closer than this target's checkpoint.
        if (!reusable || physical < lookup.node_index_ ||
            physical - lookup.node_index_ > physical % kNodesPerCheckpoint) {
            lookup = ReadFrom(node_index, load_mass);
            if (!lookup.radix_ready_) return false;
        } else {
            while (lookup.node_index_ <= physical) {
                lookup.radix_id_ = lookup.node_index_;
                if (!DecodeRadixRecord(lookup, lookup.radix_node_, lookup.radix_span_, lookup.radix_length_)) {
                    lookup = {}; return false;
                }
            }
        }
    }
    const auto position = RadixPosition(node_index);
    if (physical >= 2 && position >= lookup.radix_length_) return false;
    lookup.radix_position_ = position; lookup.radix_ready_ = true;
    if (!lookup.Next(out)) return false;
    if (!include_mass) { out.mass = 0; out.maximum_log_probability = INT16_MIN; }
    return true;
}

uint32_t Model::FindChild(uint32_t node_index, uint8_t label) const
{
    Node parent;
    return GetNode(node_index, parent, false) ? FindChildAndRead(parent, label) : kNotFound;
}

uint32_t Model::FindChild(uint32_t node_index, uint8_t label, Cursor& lookup) const
{
    Node parent;
    return GetNode(node_index, parent, lookup, false) ? FindChildAndRead(parent, label, lookup) : kNotFound;
}

uint32_t Model::FindChildAndRead(Node& current, uint8_t label, Cursor& lookup) const
{
    if (!label) return kNotFound;
    for (uint32_t ordinal = 0; ordinal < current.child_count; ++ordinal) {
        const auto index = current.first_child + ordinal * current.child_stride;
        Node child;
        if (!GetNode(index, child, lookup, false) || child.label > label) break;
        if (child.label == label) { current = child; return index; }
    }
    return kNotFound;
}

uint32_t Model::FindChildAndRead(Node& current, uint8_t label) const
{
    if (label == 0 || current.child_count == 0) {
        return kNotFound;
    }

    Cursor cursor = ReadFrom(current.first_child, false);
    if (version_ == 4 && current.child_stride == kRadixStride) {
        // Read physical siblings sequentially, rather than scanning the same
        // checkpoint again for every possible outgoing label.
        for (uint32_t offset = 0; offset < current.child_count; ++offset) {
            Node child;
            const uint8_t* span = cursor.radix_span_; uint16_t length = cursor.radix_length_;
            if (offset == 0) child = cursor.radix_node_;
            else {
                if (!DecodeRadixRecord(cursor, child, span, length)) break;
            }
            if (child.label > label) break;
            if (child.label == label) {
                const auto index = current.first_child + offset * kRadixStride;
                current = RadixLogicalNode(child, RadixEdge(index), span, length, 0);
                return index;
            }
        }
        return kNotFound;
    }
    for (uint32_t offset = 0; offset < current.child_count; ++offset) {
        Node child;
        const auto index = current.first_child + offset * current.child_stride;
        if (!cursor.Next(child) || child.label > label) {
            break;
        }
        if (child.label == label) {
            current = child;
            return index;
        }
    }
    return kNotFound;
}

uint32_t Model::FollowFrom(uint32_t node_index, std::string_view suffix) const
{
    if (suffix.empty()) {
        Node ignored;
        return GetNode(node_index, ignored, false) ? node_index : kNotFound;
    }

    if (version_ == 4) {
        Cursor lookup;
        Node current;
        if (!GetNode(node_index, current, lookup, false)) return kNotFound;
        while (!suffix.empty()) {
            const auto physical = lookup.radix_id_;
            const auto position = RadixPosition(node_index);
            if (physical >= 2 && position + 1 < lookup.radix_length_) {
                const auto remaining = static_cast<size_t>(lookup.radix_length_ - position - 1);
                const auto advance = suffix.size() < remaining ? suffix.size() : remaining;
                if (std::memcmp(lookup.radix_span_ + position + 1, suffix.data(), advance)) return kNotFound;
                node_index += static_cast<uint32_t>(advance); suffix.remove_prefix(advance);
                if (suffix.empty()) return node_index;
                if (!GetNode(node_index, current, lookup, false)) return kNotFound;
            }
            node_index = FindChildAndRead(current, static_cast<uint8_t>(suffix.front()), lookup);
            if (node_index == kNotFound) return kNotFound;
            suffix.remove_prefix(1);
        }
        return node_index;
    }

    Node child;
    if (!GetNode(node_index, child, false)) {
        return kNotFound;
    }

    for (unsigned char label : suffix) {
        node_index = FindChildAndRead(child, label);
        if (node_index == kNotFound) {
            break;
        }
    }
    return node_index;
}

uint32_t Model::FindPrefix(std::string_view prefix, Root root) const
{
    if (root != Root::Combined && root != Root::Unigrams) {
        return kNotFound;
    }
    return FollowFrom(static_cast<uint32_t>(root), prefix);
}

bool Model::Decode(Cursor& cursor, Node& out) const
{
    if (cursor.node_index_ >= node_count_ || cursor.node_index_ >= cursor.next_child_ ||
        cursor.next_child_ > node_count_ || cursor.record_offset_ >= records_size_) {
        return false;
    }

    uint32_t flags;
    const uint8_t label = records_[cursor.record_offset_++];
    if (!ReadVarUInt32(records_, records_size_, cursor.record_offset_, flags) ||
        (cursor.node_index_ < 2 ? label != 0 : label == 0)) {
        return false;
    }

    const uint32_t count = flags >> 1;
    if (count > UINT8_MAX || count > node_count_ - cursor.next_child_) {
        return false;
    }

    uint32_t terminal = 0;
    if ((flags & 1) != 0 &&
        (cursor.node_index_ < 2 ||
         !ReadVarUInt32(records_, records_size_, cursor.record_offset_, terminal) || terminal == 0)) {
        return false;
    }
    if (cursor.node_index_ >= 2 && count == 0 && terminal == 0) {
        return false;
    }

    uint32_t mass = 0;
    if (cursor.include_mass_) {
        mass = terminal;
        if (count != 0 && (!ReadVarUInt32(masses_, masses_size_, cursor.mass_offset_, mass) || mass == 0)) {
            return false;
        }
    }

    out = {mass, terminal, count == 0 ? 0 : cursor.next_child_, static_cast<uint16_t>(count), label};
    ++cursor.node_index_;
    cursor.next_child_ += count;
    return true;
}

bool Model::DecodeRadixRecord(Cursor& cursor, Node& out, const uint8_t*& span, uint16_t& length) const
{
    if (cursor.node_index_ >= node_count_ || cursor.node_index_ >= cursor.next_child_ ||
        cursor.next_child_ > node_count_ || cursor.record_offset_ >= records_size_) return false;
    uint32_t size = 0, flags = 0;
    if (!ReadVarUInt32(records_, records_size_, cursor.record_offset_, size) || size > 256 ||
        (size == 0) != (cursor.node_index_ < 2) || size > records_size_ - cursor.record_offset_) return false;
    span = records_ + cursor.record_offset_; length = static_cast<uint16_t>(size);
    for (uint32_t i = 0; i < size; ++i) if (!span[i]) return false;
    cursor.record_offset_ += size;
    if (!ReadVarUInt32(records_, records_size_, cursor.record_offset_, flags)) return false;
    const auto children = flags >> 3;
    if (children > 255 || children > node_count_ - cursor.next_child_) return false;
    uint64_t terminal = 0, mass = 0;
    if ((flags & 1) && (cursor.node_index_ < 2 ||
        !ReadVarUInt64(records_, records_size_, cursor.record_offset_, terminal) || !terminal)) return false;
    Node decoded{0, terminal, children ? cursor.next_child_ : 0, static_cast<uint16_t>(children), size ? span[0] : uint8_t(0)};
    decoded.score_flags = static_cast<uint8_t>(flags & 6);
    if (flags & kProbability) {
        if (records_size_ - cursor.record_offset_ < 2) return false;
        decoded.log_probability = ReadLe16(records_ + cursor.record_offset_); cursor.record_offset_ += 2;
        if (decoded.log_probability > 0) return false;
    }
    if (flags & kBackoff) {
        if (records_size_ - cursor.record_offset_ < 2) return false;
        decoded.log_backoff = ReadLe16(records_ + cursor.record_offset_); cursor.record_offset_ += 2;
    }
    if (cursor.node_index_ >= 2 && !children && !terminal && !decoded.score_flags) return false;
    if (cursor.include_mass_) {
        mass = terminal;
        if (children && !ReadVarUInt64(masses_, masses_size_, cursor.mass_offset_, mass)) return false;
        if (language_scores_) {
            if (children) {
                if (masses_size_ - cursor.mass_offset_ < 2) return false;
                decoded.maximum_log_probability = ReadLe16(masses_ + cursor.mass_offset_); cursor.mass_offset_ += 2;
                if (decoded.maximum_log_probability > 0) return false;
            } else if (cursor.node_index_ >= 2 && (flags & kProbability)) decoded.maximum_log_probability = decoded.log_probability;
        }
    }
    decoded.mass = mass;
    ++cursor.node_index_; cursor.next_child_ += children;
    out = decoded; return true;
}

bool Model::ReadLegacy2(uint32_t node_index, Node& out, bool include_mass) const
{
    if (node_index >= node_count_) return false;
    const auto* bytes = records_ + 16 + uint64_t(node_index) * 12;
    const auto first = ReadLe32(bytes), mass = ReadLe32(bytes + 4);
    const auto children = static_cast<uint16_t>(uint16_t(bytes[8]) | (uint16_t(bytes[9]) << 8));
    if (children > 255 || first > node_count_ || children > node_count_ - first || bytes[11] ||
        (bytes[10] == 0) != (node_index < 2) || (children ? first < 2 : first != 0)) return false;
    uint64_t remaining = mass;
    for (uint32_t i = 0; i < children; ++i) {
        const auto amount = ReadLe32(records_ + 16 + uint64_t(first + i) * 12 + 4);
        if (amount > remaining) return false;
        remaining -= amount;
    }
    if (node_index < 2 && remaining) return false;
    if (node_index >= 2 && !children && !remaining) return false;
    out = {include_mass ? mass : 0u, remaining, first, children, bytes[10]}; return true;
}

bool Model::ValidateLegacy2() const
{
    uint32_t next = 2;
    for (uint32_t i = 0; i < node_count_; ++i) {
        Node node{};
        if (i >= next || !ReadLegacy2(i, node, true) || (node.child_count && node.first_child != next)) return false;
        uint8_t previous = 0;
        for (uint32_t child = 0; child < node.child_count; ++child) {
            const auto label = records_[16 + uint64_t(next + child) * 12 + 10];
            if (label <= previous) return false;
            previous = label;
        }
        next += node.child_count;
    }
    return next == node_count_;
}

bool Model::ValidateRadix() const
{
    Cursor parents; parents.model_ = this; parents.record_offset_ = 16; parents.next_child_ = 2;
    Cursor children = parents;
    Node ignored{}; const uint8_t* span; uint16_t length;
    if (!DecodeRadixRecord(children, ignored, span, length) || !DecodeRadixRecord(children, ignored, span, length)) return false;
    uint64_t logical = 0; bool tagged = false;
    for (uint32_t i = 0; i < node_count_; ++i) {
        if (i % 32 == 0) {
            const auto checkpoint = AtCheckpoint(i, true);
            if (checkpoint.record_offset_ != parents.record_offset_ || checkpoint.next_child_ != parents.next_child_ ||
                checkpoint.mass_offset_ != parents.mass_offset_) return false;
        }
        Node parent{};
        if (!DecodeRadixRecord(parents, parent, span, length) || parent.terminal > parent.mass) return false;
        logical += i < 2 ? 1 : length;
        if (i == 0) {
            tagged = (parent.score_flags & kProbability) != 0;
            if ((parent.score_flags & kBackoff) || (tagged && parent.log_probability != 0)) return false;
        } else if (i == 1) {
            if ((parent.score_flags & kBackoff) || ((parent.score_flags & kProbability) != 0) != tagged) return false;
        } else if ((parent.score_flags && !tagged) || (tagged && parent.terminal && !(parent.score_flags & kProbability))) return false;
        uint64_t remaining = parent.mass - parent.terminal;
        int16_t maximum = i >= 2 && (parent.score_flags & kProbability) ? parent.log_probability : INT16_MIN;
        uint8_t previous = 0;
        for (uint32_t child = 0; child < parent.child_count; ++child) {
            Node item{};
            if (!DecodeRadixRecord(children, item, span, length) || item.label <= previous || item.mass > remaining) return false;
            previous = item.label; remaining -= item.mass;
            if (item.maximum_log_probability > maximum) maximum = item.maximum_log_probability;
        }
        if (remaining || (tagged && maximum != parent.maximum_log_probability)) return false;
    }
    return logical == logical_node_count_ && parents.next_child_ == node_count_ && children.node_index_ == node_count_ &&
        parents.record_offset_ == records_size_ && parents.mass_offset_ == masses_size_;
}

double Model::ConditionalLogProbability(std::string_view word, std::string_view previous, std::string_view older) const noexcept
{
    const auto missing = -std::numeric_limits<double>::infinity();
    if (!IsOpen() || !language_scores_) return missing;
    Node unigram{};
    auto id = FindPrefix(word, Root::Unigrams);
    double result;
    if (id != kNotFound && GetNode(id, unigram, false) && (unigram.score_flags & kProbability)) result = LogScore(unigram.log_probability);
    else {
        if (!GetNode(1, unigram, false) || !(unigram.score_flags & kProbability)) return missing;
        result = LogScore(unigram.log_probability);
    }
    const auto apply = [&](uint32_t history, double lower) {
        Node context{};
        if (history == kNotFound || !GetNode(history, context, false)) return lower;
        const auto space = FindChild(history, ' ');
        const auto target = space == kNotFound ? kNotFound : FollowFrom(space, word);
        Node observed{};
        if (target != kNotFound && GetNode(target, observed, false) && (observed.score_flags & kProbability))
            return LogScore(observed.log_probability);
        return (context.score_flags & kBackoff) ? LogScore(context.log_backoff) + lower : lower;
    };
    if (!previous.empty()) {
        result = apply(FindPrefix(previous), result);
        if (!older.empty()) {
            const auto history = FindPrefix(older);
            const auto space = history == kNotFound ? kNotFound : FindChild(history, ' ');
            result = apply(space == kNotFound ? kNotFound : FollowFrom(space, previous), result);
        }
    }
    return result;
}

bool Model::Validate() const
{
    Cursor parents;
    parents.model_ = this;
    parents.record_offset_ = kTrieHeaderSize;
    parents.next_child_ = 2;
    Cursor children = parents;
    Node ignored;
    if (!children.Next(ignored) || !children.Next(ignored)) {
        return false;
    }

    for (uint32_t i = 0; i < node_count_; ++i) {
        if (i % kNodesPerCheckpoint == 0) {
            const Cursor checkpoint = AtCheckpoint(i, true);
            if (checkpoint.record_offset_ != parents.record_offset_ ||
                checkpoint.next_child_ != parents.next_child_ || checkpoint.mass_offset_ != parents.mass_offset_) {
                return false;
            }
        }
        Node parent;
        if (!parents.Next(parent) || parent.terminal > parent.mass) {
            return false;
        }

        // Child runs are consecutive in BFS order, so both cursors advance only forwards.
        uint64_t remaining_mass = parent.mass - parent.terminal;
        uint8_t previous_label = 0;
        for (uint32_t child_index = 0; child_index < parent.child_count; ++child_index) {
            Node child;
            if (!children.Next(child) || child.label <= previous_label || child.mass > remaining_mass) {
                return false;
            }
            remaining_mass -= child.mass;
            previous_label = child.label;
        }
        if (remaining_mass != 0) {
            return false;
        }
    }

    return parents.next_child_ == node_count_ && children.node_index_ == node_count_ &&
           parents.record_offset_ == records_size_ && parents.mass_offset_ == masses_size_;
}

}  // namespace espos::keyboard::prediction
