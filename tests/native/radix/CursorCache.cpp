#include "PredictionModel.h"
#include "PredictionPrefix.h"
#include "RadixModelBuilder.h"
#include "AssetFixtures.h"
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace prediction = espos::keyboard::prediction;
namespace {
bool count_allocations;
std::size_t allocations;
}
void* operator new(std::size_t size) {
    if (count_allocations) ++allocations;
    if (auto* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
void Require(bool condition, const char* message) {
    if (!condition) { count_allocations = false; throw std::runtime_error(message); }
}
bool Same(const prediction::Node& a, const prediction::Node& b) {
    return a.mass == b.mass && a.terminal == b.terminal &&
        a.first_child == b.first_child && a.child_count == b.child_count &&
        a.label == b.label && a.child_stride == b.child_stride &&
        a.log_probability == b.log_probability && a.log_backoff == b.log_backoff &&
        a.maximum_log_probability == b.maximum_log_probability && a.score_flags == b.score_flags;
}
prediction::RadixEncodedModel Fixture(bool scored) {
    std::vector<std::string> words{"alpha", "alphabet", "alpine", "beta", "betamax", "gamma", "omega",
        u8"école", u8"éclair", u8"你好", u8"你好世界", u8"𠀀𠀀漢字"};
    // More than two checkpoint blocks, UTF-8 branch labels and a span split
    // at 256 bytes exercise different cache reuse and fallback paths.
    for (char label = 'a'; label <= 'z'; ++label) {
        words.push_back(std::string("branch") + label + "encyclopedia");
        words.push_back(std::string("branch") + label + "encyclopedic");
    }
    words.push_back(std::string("long") + std::string(300, 'q') + u8"école終");
    const auto vocabulary = words.size();
    words.push_back("alpha beta"); words.push_back("alpha gamma"); words.push_back("alpha beta gamma");
    std::vector<prediction::RadixEntry> entries;
    entries.reserve(words.size());
    const double unigram = 1.0 / static_cast<double>(vocabulary + 1);
    for (std::size_t i = 0; i < vocabulary; ++i) {
        const auto frequency = i == 0 ? std::uint64_t(UINT32_MAX) + 111 : (i + 1) * 17;
        entries.push_back({words[i], frequency, scored ? unigram : -1,
            scored && i == 0 ? .2 / (1 - 2 * unigram) : -1});
    }
    entries.push_back({words[vocabulary], 11, scored ? .6 : -1, scored ? .3 / (1 - unigram) : -1});
    entries.push_back({words[vocabulary + 1], 7, scored ? .2 : -1});
    entries.push_back({words[vocabulary + 2], 5, scored ? .7 : -1});
    return prediction::EncodeRadixModel(entries, {scored, scored ? unigram : 0});
}
std::vector<std::uint32_t> Handles(const prediction::Model& model) {
    if (model.FormatVersion() != 4) {
        std::vector<std::uint32_t> result;
        // Legacy access delegates to the unchanged dense reader. Cover every
        // checkpoint's first/last record rather than repeating its full graph.
        for (std::uint32_t id = 0; id < model.NodeCount(); ++id)
            if (model.FormatVersion() == 2 || id < 64 || id + 64 >= model.NodeCount() || id % 32 == 0 || id % 32 == 31)
                result.push_back(id);
        return result;
    }
    std::vector<std::uint32_t> result{0, 1};
    for (std::size_t i = 0; i < result.size(); ++i) {
        prediction::Node node{};
        Require(model.GetNode(result[i], node), "Cannot enumerate valid fixture node");
        for (unsigned child = 0; child < node.child_count; ++child)
            result.push_back(node.first_child + child * node.child_stride);
    }
    Require(result.size() == model.NodeCount(), "Fixture enumeration lost logical span positions");
    return result;
}
void CompareGet(const prediction::Model& model, std::uint32_t handle,
                prediction::Model::Cursor& cache, bool mass) {
    prediction::Node expected{}, actual{};
    const auto reference = model.GetNode(handle, expected, mass);
    const auto cached = model.GetNode(handle, actual, cache, mass);
    Require(reference == cached && (!reference || Same(expected, actual)), "Cached node metadata differs from uncached access");
}
void CacheParity(prediction::Model& model) {
    const auto handles = Handles(model);
    prediction::Model::Cursor cache;
    // One explicit cursor may be reused within a query. Both forward physical
    // traversal and DFS backtracking must retain all terminal/mass metadata.
    for (const auto handle : handles) CompareGet(model, handle, cache, true);
    for (auto i = handles.rbegin(); i != handles.rend(); ++i) CompareGet(model, *i, cache, true);
    for (const auto handle : handles) {
        CompareGet(model, handle, cache, false);
        CompareGet(model, handle, cache, true);
        CompareGet(model, handle, cache, false);
        CompareGet(model, handle, cache, true);
    }
    cache = {};
    std::uint32_t random = 0x5a17c9e3;
    allocations = 0; count_allocations = true;
    for (unsigned query = 0; query < 512; ++query) {
        random = random * 1664525u + 1013904223u;
        const auto handle = handles[random % handles.size()];
        CompareGet(model, handle, cache, (query & 1) != 0);
        prediction::Node parent{};
        Require(model.GetNode(handle, parent, cache), "Cached parent node disappeared");
        for (unsigned child = 0; child < parent.child_count; ++child) {
            const auto childHandle = parent.first_child + child * parent.child_stride;
            prediction::Node expected{};
            Require(model.GetNode(childHandle, expected, false), "Fixture child disappeared");
            Require(model.FindChild(handle, expected.label, cache) == model.FindChild(handle, expected.label),
                    "Cached sibling search changed a branch handle");
        }
        Require(model.FindChild(handle, 0, cache) == prediction::Model::kNotFound,
                "Cached child lookup accepted a zero label");
        Require(model.FindChild(handle, 255, cache) == model.FindChild(handle, 255),
                "Cached missing-child lookup differs");
    }
    count_allocations = false;
    Require(allocations == 0, "Cached lookup allocated during queries");

    prediction::Node sentinel{123, 456, 789, 7, 42};
    auto unchanged = sentinel;
    Require(!model.GetNode(prediction::Model::kNotFound, sentinel, cache) && Same(sentinel, unchanged),
            "Out-of-range cached access changed the destination");
    Require(model.FindChild(prediction::Model::kNotFound, 'a', cache) == prediction::Model::kNotFound,
            "Out-of-range cached parent was accepted");
    CompareGet(model, handles.back(), cache, true);
    if (model.FormatVersion() == 4) {
        const auto terminal = model.FindPrefix(u8"école");
        Require(terminal != prediction::Model::kNotFound, "Fixture accented word disappeared");
        Require(!model.GetNode(terminal + 1, sentinel, cache), "Unused virtual span position was accepted");
        CompareGet(model, terminal, cache, true);
    }
}
void ReopenAndScratchReuse() {
    auto before = Fixture(false), after = Fixture(true);
    prediction::Model model;
    prediction::PrefixScratch scratch{}, independent{};
    std::array<prediction::Suggestion, 3> output{}, expected{};
    Require(model.Open(before.trie.data(), before.trie.size(), before.index.data(), before.index.size()), "Count fixture did not open");
    prediction::Model::Cursor cache;
    CompareGet(model, model.FindPrefix("alpha"), cache, true);
    prediction::Suggest(model, scratch, "a", "beta", "alpha", output.data(), output.size());
    model.Close(); cache = {}; // Open/Close invalidates caller-owned cursors.
    prediction::Node node{};
    Require(!model.GetNode(0, node, cache), "Closed model accepted cached access");
    Require(model.Open(after.trie.data(), after.trie.size(), after.index.data(), after.index.size()), "Scored fixture did not reopen");
    cache = {};
    CompareGet(model, model.FindPrefix("alpha"), cache, true);
    const auto count = prediction::Suggest(model, scratch, "g", "beta", "alpha", output.data(), output.size());
    const auto reference = prediction::Suggest(model, independent, "g", "beta", "alpha", expected.data(), expected.size());
    Require(count == reference, "Reused prefix scratch retained a cursor from a previous model");
    for (std::size_t i = 0; i < count; ++i)
        Require(output[i].Word() == expected[i].Word() && output[i].mass == expected[i].mass,
                "Reopened model used old cached span or mass metadata");
}
#include "LegacyV3.inc"
}
int main() {
    try {
        for (const bool scored : {false, true}) {
            auto packed = Fixture(scored);
            prediction::Model model;
            Require(model.Open(packed.trie.data(), packed.trie.size(), packed.index.data(), packed.index.size()),
                    "Radix cache fixture did not open");
            CacheParity(model);
        }
        auto fixed = RadixFixtures::Fixed();
        prediction::Model legacy;
        Require(legacy.Open(fixed.data(), fixed.size(), nullptr, 0), "V2 fixture did not open");
        CacheParity(legacy);
        Require(legacy.Open(kBuiltinTrie, sizeof kBuiltinTrie, kBuiltinIndex, sizeof kBuiltinIndex) && legacy.FormatVersion() == 3,
                "Frozen V3 fixture did not open as v3");
        CacheParity(legacy);
        ReopenAndScratchReuse();
        std::cout << "Caller-owned cursor cache: metadata parity, span/UTF-8/checkpoint traversal, mass upgrades, bounds, scratch reuse and no query allocations passed\n";
        return 0;
    } catch (const std::exception& error) {
        count_allocations = false; std::cerr << error.what() << '\n'; return 1;
    }
}
