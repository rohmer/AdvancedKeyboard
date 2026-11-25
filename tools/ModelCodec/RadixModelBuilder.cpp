#include "RadixModelBuilder.h"
#include "PredictionModel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

namespace espos::keyboard::prediction {
namespace {
constexpr std::uint32_t none = UINT32_MAX;
constexpr std::uint8_t has_count = 1, has_probability = 2, has_backoff = 4;
constexpr std::size_t header_size = 16, checkpoint_size = 12, block_size = 32;
struct BuilderNode {
    std::uint32_t child = none, sibling = none;
    std::uint64_t count = 0, mass = 0;
    std::int16_t probability = 0, backoff = 0;
    std::uint8_t label = 0, scores = 0;
};
struct Edge { std::uint32_t end, label, first = 0; std::uint16_t length = 0, children = 0; };
void put32(std::vector<std::uint8_t>& output, std::uint32_t value) {
    for(unsigned shift = 0; shift != 32; shift += 8) output.push_back(static_cast<std::uint8_t>(value >> shift));
}
void set32(std::vector<std::uint8_t>& output, std::size_t offset, std::uint32_t value) {
    for(unsigned shift = 0; shift != 32; shift += 8) output[offset++] = static_cast<std::uint8_t>(value >> shift);
}
void put16(std::vector<std::uint8_t>& output, std::int16_t value) {
    const auto raw = static_cast<std::uint16_t>(value);
    output.push_back(static_cast<std::uint8_t>(raw)); output.push_back(static_cast<std::uint8_t>(raw >> 8));
}
void var(std::vector<std::uint8_t>& output, std::uint64_t value) {
    while(value >= 128) { output.push_back(static_cast<std::uint8_t>((value & 127) | 128)); value >>= 7; }
    output.push_back(static_cast<std::uint8_t>(value));
}
std::int16_t score(double value, bool probability) {
    if(!std::isfinite(value) || value < 0 || (probability && value > 1)) throw std::invalid_argument("Invalid radix language-model score");
    if(value == 0) return INT16_MIN;
    const auto quantized = std::round(std::log2(value) * 256.0);
    if(quantized < -32767 || quantized > 32767) throw std::overflow_error("Radix language-model score exceeds signed Q8 log range");
    return static_cast<std::int16_t>(quantized);
}
std::uint32_t child(std::vector<BuilderNode>& nodes, std::uint32_t parent, std::uint8_t label) {
    auto current = nodes[parent].child, previous = none;
    while(current != none && nodes[current].label < label) { previous = current; current = nodes[current].sibling; }
    if(current != none && nodes[current].label == label) return current;
    if(nodes.size() >= UINT32_MAX) throw std::overflow_error("Radix logical node limit exceeded");
    const auto index = static_cast<std::uint32_t>(nodes.size());
    nodes.emplace_back(); nodes.back().label = label; nodes.back().sibling = current;
    if(previous == none) nodes[parent].child = index; else nodes[previous].sibling = index;
    return index;
}
void insert(std::vector<BuilderNode>& nodes, std::uint32_t root, const RadixEntry& entry, bool unigram) {
    auto parent = root;
    for(unsigned char label : entry.key) parent = child(nodes, parent, label);
    auto& node = nodes[parent];
    if(node.count || node.scores) throw std::invalid_argument("Radix entries must have unique keys");
    node.count = entry.count;
    if(entry.probability >= 0) { node.probability = score(entry.probability, true); node.scores |= has_probability; }
    if(!unigram && entry.backoff >= 0) { node.backoff = score(entry.backoff, false); node.scores |= has_backoff; }
}
}

RadixEncodedModel EncodeRadixModel(const std::vector<RadixEntry>& entries, const RadixBuildOptions& options) {
    std::vector<BuilderNode> nodes(2);
    if(options.smoothed) {
        nodes[0].scores = has_probability; // Explicit tag: this is a conditional-probability model.
        nodes[1].scores = has_probability;
        nodes[1].probability = score(options.unknown_probability, true);
    }
    for(const auto& entry : entries) {
        if(entry.key.empty() || entry.key.find('\0') != std::string_view::npos || entry.key.front() == ' ' || entry.key.back() == ' ' || entry.key.find("  ") != std::string_view::npos)
            throw std::invalid_argument("Radix keys must contain nonempty normalized words");
        if(!entry.count && entry.probability < 0 && entry.backoff < 0) throw std::invalid_argument("Radix entry has no count or score");
        if(!options.smoothed && (entry.probability >= 0 || entry.backoff >= 0)) throw std::invalid_argument("Scores require a smoothed radix model");
        insert(nodes, 0, entry, false);
        if(entry.key.find(' ') == std::string_view::npos && entry.count) insert(nodes, 1, entry, true);
    }
    // Parents are created before children, so reverse allocation order is a
    // topological order even though insertion itself is not breadth first.
    for(std::size_t index = nodes.size(); index-- > 0;) {
        std::uint64_t mass = nodes[index].count;
        for(auto id = nodes[index].child; id != none; id = nodes[id].sibling) {
            if(nodes[id].mass > UINT64_MAX - mass) throw std::overflow_error("Radix aggregate frequency exceeds 64 bits");
            mass += nodes[id].mass;
        }
        nodes[index].mass = mass;
    }
    std::vector<Edge> edges{{0, 0}, {1, 0}};
    std::vector<std::uint8_t> labels;
    labels.reserve(nodes.size() - 2);
    for(std::size_t index = 0; index != edges.size(); ++index) {
        const auto first = static_cast<std::uint32_t>(edges.size());
        std::uint16_t degree = 0;
        for(auto id = nodes[edges[index].end].child; id != none; id = nodes[id].sibling) {
            Edge edge{id, static_cast<std::uint32_t>(labels.size())};
            for(;;) {
                labels.push_back(nodes[edge.end].label); ++edge.length;
                const auto next = nodes[edge.end].child;
                if(edge.length == 256 || nodes[edge.end].count || nodes[edge.end].scores || next == none || nodes[next].sibling != none) break;
                edge.end = next;
            }
            edges.push_back(edge); ++degree;
        }
        edges[index].first = degree ? first : 0; edges[index].children = degree;
        if(edges.size() > 0x00ffffffu) throw std::overflow_error("Radix physical handle limit exceeded");
    }
    RadixEncodedModel result;
    result.logical_nodes = static_cast<std::uint32_t>(nodes.size());
    result.physical_nodes = static_cast<std::uint32_t>(edges.size());
    std::vector<std::int16_t> maximum_probability(edges.size(), INT16_MIN);
    if(options.smoothed) for(std::size_t index = edges.size(); index-- > 0;) {
        const auto& edge = edges[index]; const auto& node = nodes[edge.end];
        std::int16_t bound = index >= 2 && (node.scores & has_probability) ? node.probability : INT16_MIN;
        for(std::uint32_t child = 0; child < edge.children; ++child) bound = (std::max)(bound, maximum_probability[edge.first + child]);
        maximum_probability[index] = bound;
    }
    result.trie = {'N','G','T','R'};
    put32(result.trie, 4); put32(result.trie, result.physical_nodes); put32(result.trie, result.logical_nodes);
    const auto blocks = (edges.size() + block_size - 1) / block_size;
    const auto mass_start = header_size + blocks * checkpoint_size;
    result.index.resize(mass_start);
    result.index[0] = 'N'; result.index[1] = 'G'; result.index[2] = 'I'; result.index[3] = 'X';
    set32(result.index, 4, 2); set32(result.index, 12, result.physical_nodes);
    std::uint32_t next_child = 2;
    for(std::size_t index = 0; index != edges.size(); ++index) {
        if(index % block_size == 0) {
            const auto checkpoint = header_size + index / block_size * checkpoint_size;
            set32(result.index, checkpoint, static_cast<std::uint32_t>(result.trie.size()));
            set32(result.index, checkpoint + 4, next_child);
            set32(result.index, checkpoint + 8, static_cast<std::uint32_t>(result.index.size() - mass_start));
        }
        const auto& edge = edges[index]; const auto& node = nodes[edge.end];
        var(result.trie, edge.length);
        result.trie.insert(result.trie.end(), labels.begin() + edge.label, labels.begin() + edge.label + edge.length);
        var(result.trie, (std::uint32_t(edge.children) << 3) | node.scores | (node.count ? has_count : 0));
        if(node.count) var(result.trie, node.count);
        if(node.scores & has_probability) put16(result.trie, node.probability);
        if(node.scores & has_backoff) put16(result.trie, node.backoff);
        if(edge.children) {
            var(result.index, node.mass);
            if(options.smoothed) put16(result.index, maximum_probability[index]);
        }
        next_child += edge.children;
        if(result.trie.size() > UINT32_MAX || result.index.size() > UINT32_MAX) throw std::overflow_error("Radix byte length exceeds 32 bits");
    }
    set32(result.index, 8, static_cast<std::uint32_t>(result.trie.size()));
    Model validation;
    if(!validation.Open(result.trie.data(), result.trie.size(), result.index.data(), result.index.size())) throw std::logic_error("Encoded radix model did not validate");
    return result;
}

RadixEncodedModel ConvertToRadix(const void* trie, std::size_t trie_size, const void* index, std::size_t index_size) {
    Model model;
    if(!model.Open(trie, trie_size, index, index_size) || model.HasLanguageScores()) throw std::invalid_argument("Expected a validated unsmoothed model");
    std::vector<std::string> words;
    std::vector<std::uint64_t> counts;
    struct Frame { std::uint32_t node; std::string word; };
    std::vector<Frame> stack{{0, {}}};
    while(!stack.empty()) {
        auto frame = std::move(stack.back()); stack.pop_back();
        Node node{};
        if(!model.GetNode(frame.node, node, false)) throw std::logic_error("Validated model traversal failed");
        if(node.terminal) {
            if(frame.word.find(' ') == std::string::npos) {
                Node independent{};
                if(!model.GetNode(model.FindPrefix(frame.word, Root::Unigrams), independent, false) || independent.terminal != node.terminal)
                    throw std::invalid_argument("Legacy combined/unigram roots disagree");
            }
            words.push_back(frame.word); counts.push_back(node.terminal);
        }
        for(std::uint32_t i = node.child_count; i-- > 0;) {
            const auto id = node.first_child + i * node.child_stride; Node next{};
            if(!model.GetNode(id, next, false)) throw std::logic_error("Validated model child failed");
            auto word = frame.word; word.push_back(static_cast<char>(next.label));
            stack.push_back({id, std::move(word)});
        }
    }
    stack.push_back({1, {}});
    while(!stack.empty()) {
        auto frame = std::move(stack.back()); stack.pop_back(); Node node{};
        if(!model.GetNode(frame.node, node, false)) throw std::logic_error("Validated unigram traversal failed");
        if(node.terminal) {
            Node combined{};
            if(!model.GetNode(model.FindPrefix(frame.word), combined, false) || combined.terminal != node.terminal)
                throw std::invalid_argument("Legacy combined/unigram roots disagree");
        }
        for(std::uint32_t i = node.child_count; i-- > 0;) {
            const auto id = node.first_child + i * node.child_stride; Node next{};
            if(!model.GetNode(id, next, false)) throw std::logic_error("Validated unigram child failed");
            auto word = frame.word; word.push_back(static_cast<char>(next.label)); stack.push_back({id, std::move(word)});
        }
    }
    std::vector<RadixEntry> entries; entries.reserve(words.size());
    for(std::size_t i = 0; i != words.size(); ++i) entries.push_back({words[i], counts[i]});
    return EncodeRadixModel(entries);
}
} // namespace espos::keyboard::prediction
