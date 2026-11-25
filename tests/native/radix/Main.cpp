#include "RadixModelBuilder.h"
#include "PredictionModel.h"
#include "PredictionPrefix.h"
#include "PredictionAutocorrect.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace prediction = espos::keyboard::prediction;
static bool count_allocations;
static std::size_t allocations;
void* operator new(std::size_t size) { if(count_allocations) ++allocations; if(auto* value = std::malloc(size ? size : 1)) return value; throw std::bad_alloc(); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
namespace {
void Require(bool condition, const char* message) { if(!condition) throw std::runtime_error(message); }
void Put32(std::vector<uint8_t>& data, uint32_t value) { for(int shift = 0; shift != 32; shift += 8) data.push_back(static_cast<uint8_t>(value >> shift)); }
void PutRecord(std::vector<uint8_t>& data, uint32_t child, uint32_t mass, uint8_t label) {
    Put32(data, child); Put32(data, mass); data.push_back(child ? 1 : 0); data.push_back(0); data.push_back(label); data.push_back(0);
}
#include "LegacyV3.inc"
void LegacyParity(const char* directory) {
    prediction::Model legacy;
    Require(legacy.Open(kBuiltinTrie, sizeof kBuiltinTrie, kBuiltinIndex, sizeof kBuiltinIndex) && legacy.FormatVersion() == 3,
            "Frozen legacy v3 model did not open as v3");
    auto packed = prediction::ConvertToRadix(kBuiltinTrie, sizeof kBuiltinTrie, kBuiltinIndex, sizeof kBuiltinIndex);
    prediction::Model radix;
    Require(radix.Open(packed.trie.data(), packed.trie.size(), packed.index.data(), packed.index.size()), "Converted radix did not open");
    Require(radix.NodeCount() == legacy.NodeCount() && radix.PhysicalNodeCount() < radix.NodeCount(), "Logical/physical node counts are incorrect");
    Require(packed.Bytes() < sizeof kBuiltinTrie + sizeof kBuiltinIndex, "Radix encoding did not reduce combined model/index bytes");
    if(directory) {
        std::ofstream(std::string(directory)+"/legacy.ngtr",std::ios::binary).write(reinterpret_cast<const char*>(kBuiltinTrie),sizeof kBuiltinTrie);
        std::ofstream(std::string(directory)+"/legacy.ngix",std::ios::binary).write(reinterpret_cast<const char*>(kBuiltinIndex),sizeof kBuiltinIndex);
        std::ofstream(std::string(directory)+"/radix.ngtr",std::ios::binary).write(reinterpret_cast<const char*>(packed.trie.data()),packed.trie.size());
        std::ofstream(std::string(directory)+"/radix.ngix",std::ios::binary).write(reinterpret_cast<const char*>(packed.index.data()),packed.index.size());
    }
    prediction::PrefixScratch before{}, after{};
    for(const auto* prefix : {"", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "th", "ther", "hel", "missing"})
        for(const auto* previous : {"", "in", "the", "was"}) {
            std::array<prediction::Suggestion,3> expected{}, actual{};
            const auto a = prediction::Suggest(legacy, before, prefix, previous, "it", expected.data(), expected.size());
            const auto b = prediction::Suggest(radix, after, prefix, previous, "it", actual.data(), actual.size());
            Require(a == b, "Radix changed legacy suggestion count");
            for(std::size_t i = 0; i != a; ++i) Require(expected[i].Word() == actual[i].Word() && expected[i].mass == actual[i].mass, "Radix changed legacy suggestion order/count");
        }
    prediction::AutocorrectScratch old_scratch{}, new_scratch{};
    for(const auto* word : {"teh", "helo", "wrold", "ths", "the", "monday", "123"}) {
        prediction::AutocorrectResult old_result{}, new_result{};
        const auto a = prediction::BestAutocorrectionUtf8(legacy, old_scratch, word, old_result, "in", "was");
        const auto b = prediction::BestAutocorrectionUtf8(radix, new_scratch, word, new_result, "in", "was");
        Require(a == b && old_result.word == new_result.word && std::abs(old_result.probability - new_result.probability) < 1e-12, "Radix changed legacy Unicode autocorrection");
    }
    std::vector<uint8_t> v2{'N','G','T','R'}; Put32(v2,2); Put32(v2,4); Put32(v2,12);
    PutRecord(v2,2,17,0); PutRecord(v2,3,17,0); PutRecord(v2,0,17,'x'); PutRecord(v2,0,17,'x');
    prediction::Model fixed;
    Require(fixed.Open(v2.data(),v2.size(),nullptr,0), "Legacy v2 model did not open");
    prediction::Node node{};
    Require(fixed.GetNode(fixed.FindPrefix("x"),node) && node.terminal == 17, "Legacy v2 terminal count changed");
    Require(prediction::ConvertToRadix(v2.data(),v2.size(),nullptr,0).logical_nodes == 4, "Legacy v2 conversion failed");
    if(directory) std::ofstream(std::string(directory)+"/fixed.ngtr",std::ios::binary).write(reinterpret_cast<const char*>(v2.data()),v2.size());
    std::cout << "Legacy v2/v3 parity: " << packed.logical_nodes << " logical / " << packed.physical_nodes << " physical nodes; "
              << sizeof kBuiltinTrie + sizeof kBuiltinIndex << " -> " << packed.Bytes() << " bytes\n";
}
void ScoredAndWide(const char* directory) {
    std::vector<prediction::RadixEntry> records{{"cat",20,.2,2.0/7}, {"car",10,.1}, {"dog",60,.6},
        {"cat cat",1,.05}, {"cat car",15,.75}, {"dog cat",0,-1,7.0/58}, {"dog cat dog",10,.9}};
    auto packed = prediction::EncodeRadixModel(records,{true,.1});
    prediction::Model model;
    Require(model.Open(packed.trie.data(),packed.trie.size(),packed.index.data(),packed.index.size()) && model.HasLanguageScores(), "Smoothed radix model did not open");
    Require(std::abs(std::exp(model.ConditionalLogProbability("dog","cat","dog")) - .9) < .002, "Observed trigram probability changed");
    double total = 0;
    for(const auto* word : {"cat","car","dog","missing"}) total += std::exp(model.ConditionalLogProbability(word,"cat","dog"));
    Require(std::abs(total - 1) < .006, "Quantized backoff model is not normalized within documented error");
    prediction::PrefixScratch scratch{}; std::array<prediction::Suggestion,3> output{};
    Require(prediction::Suggest(model,scratch,"","cat","dog",output.data(),output.size()) == 3 &&
        output[0].Word() == "dog" && output[1].Word() == "car" && output[2].Word() == "cat", "Smoothed layers did not merge by full conditional probability");
    Require(prediction::Suggest(model,scratch,"c","cat","dog",output.data(),output.size()) == 2 && output[0].Word() == "car", "Prefix ending inside a compressed edge failed");
    allocations = 0; count_allocations = true;
    for(int i = 0; i != 25; ++i) prediction::Suggest(model,scratch,"c","cat","dog",output.data(),output.size());
    count_allocations = false; Require(!allocations, "Prediction allocated memory per query");
    if(directory) {
        std::ofstream(std::string(directory)+"/scored.ngtr",std::ios::binary).write(reinterpret_cast<const char*>(packed.trie.data()),packed.trie.size());
        std::ofstream(std::string(directory)+"/scored.ngix",std::ios::binary).write(reinterpret_cast<const char*>(packed.index.data()),packed.index.size());
    }
    auto corrupt = packed.index; corrupt.pop_back();
    prediction::Model invalid;
    Require(!invalid.Open(packed.trie.data(),packed.trie.size(),corrupt.data(),corrupt.size()), "Truncated radix index was accepted");
    corrupt = packed.index;
    const auto mass_start = 16 + ((packed.physical_nodes+31)/32)*12;
    auto offset = mass_start; while(corrupt[offset++] & 128) {}
    corrupt[offset] = 0; corrupt[offset+1] = 128;
    Require(!invalid.Open(packed.trie.data(),packed.trie.size(),corrupt.data(),corrupt.size()), "Unsafe probability pruning bound was accepted");
    const auto wide = uint64_t(UINT32_MAX)+123;
    auto large = prediction::EncodeRadixModel({{"wide",wide},{"wider",wide+1},{u8"école",7},{u8"éclair",8}});
    Require(model.Open(large.trie.data(),large.trie.size(),large.index.data(),large.index.size()), "64-bit radix counts did not open");
    prediction::Node node{};
    Require(model.GetNode(model.FindPrefix("wide"),node) && node.terminal == wide && node.mass == wide+wide+1, "64-bit terminal/aggregate count changed");
    Require(prediction::Suggest(model,scratch,"wid",{},{},output.data(),output.size()) == 2 && output[0].Word() == "wider" && output[0].mass == wide+1, "Counts were saturated before ranking");
    if(directory) {
        std::ofstream(std::string(directory)+"/wide.ngtr",std::ios::binary).write(reinterpret_cast<const char*>(large.trie.data()),large.trie.size());
        std::ofstream(std::string(directory)+"/wide.ngix",std::ios::binary).write(reinterpret_cast<const char*>(large.index.data()),large.index.size());
    }
    auto accents = prediction::EncodeRadixModel({{u8"école",100000},{u8"éclair",1000},{u8"café",500}});
    Require(model.Open(accents.trie.data(),accents.trie.size(),accents.index.data(),accents.index.size()), "Accented radix model did not open");
    prediction::AutocorrectScratch correction{}; prediction::AutocorrectResult result{};
    // French corpus preparation retains accents; test the already-prepared API.
    prediction::BestAutocorrection(model,correction,u8"écloe",result);
    Require(result.word == u8"école", "Unicode transposition across compressed edges failed");
    allocations = 0; count_allocations = true;
    for(int i = 0; i != 25; ++i) prediction::BestAutocorrection(model,correction,u8"écloe",result);
    count_allocations = false; Require(!allocations, "Unicode autocorrect allocated memory per query");
    const std::string long_word(300,'x');
    auto long_span = prediction::EncodeRadixModel({{long_word,9}});
    Require(model.Open(long_span.trie.data(),long_span.trie.size(),long_span.index.data(),long_span.index.size()) &&
        model.GetNode(model.FindPrefix(long_word),node) && node.terminal == 9, "A path longer than one virtual-handle span failed");
    std::cout << "Scored backoff, validated pruning, mid-edge prefixes, Unicode DP, 64-bit counts and allocation-free lookup passed\n";
}
void ZeroProbabilityCorrection() {
    auto packed = prediction::EncodeRadixModel({
        {"car",100,.3}, {"cat",100,.3}, {"cot",1000000,.3}, {"one",1,.09,2},
        {"one car",1,0}, {"one cat",1,0}, {"one cot",1,.8}
    }, {true,.01});
    prediction::Model model;
    Require(model.Open(packed.trie.data(),packed.trie.size(),packed.index.data(),packed.index.size()),
        "Exact-zero scored model did not open");
    Require(std::isinf(model.ConditionalLogProbability("car","one")) &&
        model.ConditionalLogProbability("car","one") < 0 &&
        std::isinf(model.ConditionalLogProbability("cat","one")) &&
        model.ConditionalLogProbability("cat","one") < 0,
        "Observed zero probabilities were replaced by lower-order backoff");
    // Observed cot has probability .8. The unseen lower-order mass (.1)
    // receives backoff 2. Observed car/cat stay at zero, totaling one.
    double total = 0;
    for (const auto* word : {"car","cat","cot","one","unknown"})
        total += std::exp(model.ConditionalLogProbability(word,"one"));
    Require(std::abs(total - 1) < .006, "Exact-zero fixture is not normalized");
    prediction::PrefixScratch prefix{}; std::array<prediction::Suggestion,3> output{};
    Require(prediction::Suggest(model,prefix,"ca","one",{},output.data(),output.size()) == 0,
        "A forbidden observed spelling leaked through lower-order completion");
    prediction::AutocorrectScratch scratch{}; prediction::AutocorrectResult result{};
    // The exact typed cat and the earlier visited edit candidate car both
    // score -infinity. A later finite cot must still receive finite confidence.
    Require(prediction::BestAutocorrection(model,scratch,"cat",result,"one") && result.word == "cot" &&
        std::isfinite(result.probability) && result.probability > 0 && result.probability <= 1,
        "Impossible original/candidates poisoned the finite correction confidence");
    Require(!prediction::BestAutocorrection(model,scratch,"cot",result,"one"),
        "A clean positive-probability spelling changed to an impossible candidate");
    std::cout << "Exact-zero observed probability, exclusion and finite correction confidence passed\n";
}
void CorrectionSafety(const char* directory) {
    std::ofstream parity;
    if (directory) { parity.open(std::string(directory) + "/autocorrect-policy.tsv"); parity << std::setprecision(17); }
    auto check = [&](const char* name, const std::vector<prediction::RadixEntry>& entries,
                     std::string_view typed, std::string_view expected) {
        auto packed = prediction::EncodeRadixModel(entries); prediction::Model model;
        Require(model.Open(packed.trie.data(),packed.trie.size(),packed.index.data(),packed.index.size()), "Safety fixture did not open");
        prediction::AutocorrectScratch scratch{}; prediction::AutocorrectResult result{};
        const bool changed = prediction::BestAutocorrection(model,scratch,typed,result);
        Require(changed == !expected.empty() && (!changed || result.word == expected), name);
        Require(!changed || std::isfinite(result.probability), "Safety-gated correction confidence is nonfinite");
        if (parity.is_open()) parity << name << '\t' << changed << '\t' << result.word << '\t' << result.probability << '\n';
        allocations = 0; count_allocations = true;
        for (int i = 0; i < 10; ++i) prediction::BestAutocorrection(model,scratch,typed,result);
        count_allocations = false; Require(!allocations, "Autocorrect safety gate allocated per query");
    };
    check("known-retention", {{"car",1000000000000ull},{"cat",1}}, "cat", {});
    check("deletion", {{"garden",10000000000ull}}, "garde", "garden");
    check("transposition", {{"garden",10000000000ull}}, "gadren", "garden");
    check("unicode-deletion", {{u8"école",10000000000ull}}, u8"écle", u8"école");
    check("supplementary-deletion", {{u8"𠀀cole",10000000000ull}}, u8"𠀀cle", u8"𠀀cole");
    check("multiple-edits", {{"garden",1000000000000ull}}, "gdn", {});
    check("keep-margin", {{"garden",3900}}, "garde", {});
    check("ambiguous-runner", {{u8"café",10000000000ull},{u8"cafà",10000000000ull}}, "cafz", {});
    auto packed = prediction::EncodeRadixModel({{"car",1000000000000ull},{"cat",1}}); prediction::Model model;
    Require(model.Open(packed.trie.data(),packed.trie.size(),packed.index.data(),packed.index.size()), "Manual suggestion fixture did not open");
    prediction::PrefixScratch scratch{}; std::array<prediction::Suggestion,3> output{};
    Require(prediction::Suggest(model,scratch,"ca",{},{},output.data(),output.size()) == 2 &&
        output[0].Word() == "car" && output[1].Word() == "cat", "Automatic safety gate changed manual prefix ranking");
    std::cout << "Known spelling, scalar edit, ambiguity/original margins, manual ranking and allocation-free correction policy passed\n";
}
}
int main(int argc,char** argv) {
    try { const auto* directory = argc > 1 ? argv[1] : nullptr; LegacyParity(directory); ScoredAndWide(directory); ZeroProbabilityCorrection(); CorrectionSafety(directory); return 0; }
    catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
