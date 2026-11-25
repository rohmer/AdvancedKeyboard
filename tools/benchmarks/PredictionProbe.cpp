#include "PredictionModel.h"
#include "PredictionPrefix.h"
#include "PredictionAutocorrect.h"
#include "Utf8Fold.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace prediction = espos::keyboard::prediction;
using Clock = std::chrono::steady_clock;
namespace {
std::vector<unsigned char> Read(const char* path) {
    if(std::string(path) == "-") return {};
    std::ifstream input(path, std::ios::binary);
    if(!input) throw std::runtime_error(std::string("Cannot read ") + path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    for(;;) {
        const auto end = line.find('\t', begin);
        result.push_back(line.substr(begin, end == std::string::npos ? end : end - begin));
        if(end == std::string::npos) return result;
        begin = end + 1;
    }
}
double Ratio(double numerator, std::size_t count) { return count ? numerator / count : 0; }
std::uint64_t Fingerprint(std::string_view word,std::uint64_t hash=14695981039346656037ull) {
    for(unsigned char scalar:word) { hash^=scalar;hash*=1099511628211ull; }
    hash^=0;hash*=1099511628211ull;
    return hash;
}
double Percentile(std::vector<double>& values, double fraction) {
    if(values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::size_t((values.size() - 1) * fraction)];
}
struct Metrics {
    std::size_t count = 0, known = 0, top1 = 0, top3 = 0, known1 = 0, known3 = 0;
    double reciprocal = 0, log_loss = 0;
};
}
int main(int argc, char** argv) {
    try {
        if(argc != 4 && argc != 5) throw std::runtime_error("Usage: PredictionProbe MODEL INDEX|- QUERIES.tsv [OUTCOMES.tsv]");
        auto raw = Read(argv[1]), index = Read(argv[2]);
        prediction::Model model;
        const auto open_start = Clock::now();
        if(!model.Open(raw.data(), raw.size(), index.empty() ? nullptr : index.data(), index.size()))
            throw std::runtime_error("Runtime rejected the model/index");
        const double open_ms = std::chrono::duration<double, std::milli>(Clock::now() - open_start).count();
        prediction::PrefixScratch prefix_scratch{};
        prediction::AutocorrectScratch correction_scratch;
        std::array<prediction::Suggestion, prediction::kSuggestionCount> suggestions{};
        std::ifstream queries(argv[3], std::ios::binary);
        if(!queries) throw std::runtime_error("Cannot read query TSV");
        std::array<Metrics, 4> by_prefix{};
        Metrics aggregate;
        std::size_t corrupt_count = 0, corrupt_fixed = 0, corrupt_changed = 0, clean_count = 0, clean_changed = 0;
        std::size_t nll_count = 0;
        double token_log_loss = 0;
        std::vector<double> completion_us, correction_us;
        std::ofstream outcomes;
        if(argc == 5) {
            outcomes.open(argv[4],std::ios::binary);
            if(!outcomes) throw std::runtime_error("Cannot write query outcomes");
            outcomes << "row\tkind\tdocument\tknown\ttop1\ttop3\treciprocal\tcorrected\tchanged\tnll\toutputFingerprint\n";
            outcomes << std::setprecision(17);
        }
        std::string line, document = "ungrouped";
        std::size_t row = 0;
        while(std::getline(queries, line)) {
            if(!line.empty() && line.back() == '\r') line.pop_back();
            if(line.rfind("#document\t",0)==0) { document=line.substr(10); continue; }
            if(line.empty() || line.front() == '#') continue;
            ++row;
            auto fields = Fields(line);
            if(fields.size() != 6) throw std::runtime_error("Query must have kind,expected,previous,older,prefix,typed");
            // Normalize with the real French keyboard path, outside the timed
            // lookup. This preserves the corpus's accents and Unicode casing.
            for(std::size_t field = 1; field < fields.size(); ++field) {
                std::array<char, 1024> buffer{};
                std::string_view prepared;
                if(!prediction::PrepareKeyboardQuery(fields[field],buffer.data(),buffer.size(),prepared,"fr-FR",field!=4))
                    throw std::runtime_error("Cannot normalize benchmark query");
                fields[field].assign(prepared);
            }
            const auto &kind = fields[0], &expected = fields[1], &previous = fields[2], &older = fields[3];
            if(kind.size() == 2 && kind[0] == 'p' && kind[1] >= '0' && kind[1] <= '3') {
                auto& metrics = by_prefix[std::size_t(kind[1] - '0')];
                const auto started = Clock::now();
                const auto count = prediction::Suggest(model, prefix_scratch, fields[4], previous, older,
                                                       suggestions.data(), suggestions.size());
                completion_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
                prediction::Node node{};
                const auto id = model.FindPrefix(expected, prediction::Root::Unigrams);
                const bool known = id != prediction::Model::kNotFound && model.GetNode(id, node) && node.terminal != 0;
                ++metrics.count; ++aggregate.count;
                if(known) { ++metrics.known; ++aggregate.known; }
                std::size_t matched_rank = 0;
                for(std::size_t rank = 0; rank < count; ++rank) if(suggestions[rank].Word() == expected) {
                    matched_rank = rank + 1;
                    ++metrics.top3; ++aggregate.top3;
                    metrics.reciprocal += 1.0 / (rank + 1); aggregate.reciprocal += 1.0 / (rank + 1);
                    if(known) { ++metrics.known3; ++aggregate.known3; }
                    if(rank == 0) { ++metrics.top1; ++aggregate.top1; if(known) { ++metrics.known1; ++aggregate.known1; } }
                    break;
                }
                double nll = -1;
#if BENCH_HAS_SCORES
                if(model.HasLanguageScores()) {
                    const auto score = model.ConditionalLogProbability(expected, previous, older);
                    if(!std::isfinite(score) || score > 1e-10) throw std::runtime_error("Invalid calibrated log probability");
                    metrics.log_loss -= score; aggregate.log_loss -= score;
                    if(kind == "p0") { ++nll_count; token_log_loss -= score; }
                    nll = -score;
                }
#endif
                if(outcomes.is_open()) {
                    std::uint64_t fingerprint=14695981039346656037ull;
                    for(std::size_t rank=0;rank<count;++rank)fingerprint=Fingerprint(suggestions[rank].Word(),fingerprint);
                    outcomes << row << '\t' << kind << '\t' << document << '\t' << known << '\t'
                        << (matched_rank==1) << '\t' << (matched_rank!=0) << '\t' << (matched_rank ? 1.0/matched_rank : 0)
                        << "\t0\t0\t" << nll << '\t' << fingerprint << '\n';
                }
            } else if(kind == "clean" || kind == "delete") {
                const auto started = Clock::now();
                prediction::AutocorrectResult correction;
                const bool found = prediction::BestAutocorrection(model, correction_scratch, fields[5], correction,
                                                                  previous, older);
                correction_us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - started).count());
                const bool changed = found && correction.probability >= 0.3 && correction.word != fields[5];
                if(kind == "clean") { ++clean_count; if(changed) ++clean_changed; }
                else { ++corrupt_count; if(changed) ++corrupt_changed; if(changed && correction.word == expected) ++corrupt_fixed; }
                prediction::Node node{};
                const auto id=model.FindPrefix(expected,prediction::Root::Unigrams);
                const bool known=id!=prediction::Model::kNotFound && model.GetNode(id,node) && node.terminal!=0;
                if(outcomes.is_open()) outcomes << row << '\t' << kind << '\t' << document << '\t' << known
                    << "\t0\t0\t0\t" << (changed && correction.word==expected) << '\t' << changed
                    << "\t-1\t" << Fingerprint(changed?correction.word:std::string_view(fields[5])) << '\n';
            } else throw std::runtime_error("Unknown query kind");
        }
        if(outcomes.is_open()) { outcomes.flush(); if(!outcomes) throw std::runtime_error("Cannot publish query outcomes"); }
        const auto write_metrics = [](const Metrics& value) {
            std::cout << "{\"queries\":" << value.count << ",\"vocabulary_coverage\":" << Ratio(value.known,value.count)
                      << ",\"top1_all\":" << Ratio(value.top1,value.count) << ",\"top3_all\":" << Ratio(value.top3,value.count)
                      << ",\"mrr_all\":" << Ratio(value.reciprocal,value.count)
                      << ",\"top1_in_vocabulary\":" << Ratio(value.known1,value.known)
                      << ",\"top3_in_vocabulary\":" << Ratio(value.known3,value.known) << '}';
        };
        std::cout << std::setprecision(12) << "{\"model_bytes\":" << raw.size() << ",\"index_bytes\":" << index.size()
                  << ",\"logical_nodes\":" << model.NodeCount() << ",\"physical_nodes\":";
#if BENCH_HAS_PHYSICAL_NODES
        std::cout << model.PhysicalNodeCount();
#else
        std::cout << model.NodeCount();
#endif
        std::cout << ",\"workspace_bytes\":" << (sizeof(model)+sizeof(prefix_scratch)+sizeof(correction_scratch))
                  << ",\"open_ms\":" << open_ms << ",\"completion\":";
        write_metrics(aggregate);
        std::cout << ",\"by_prefix\":[";
        for(std::size_t i=0;i<by_prefix.size();++i) { if(i) std::cout << ','; write_metrics(by_prefix[i]); }
        std::cout << "],\"completion_p50_us\":" << Percentile(completion_us,.50)
                  << ",\"completion_p95_us\":" << Percentile(completion_us,.95)
                  << ",\"autocorrect_p50_us\":" << Percentile(correction_us,.50)
                  << ",\"autocorrect_p95_us\":" << Percentile(correction_us,.95)
                  << ",\"autocorrect\":{\"corrupted_queries\":" << corrupt_count
                  << ",\"corrected_to_expected\":" << Ratio(corrupt_fixed,corrupt_count)
                  << ",\"changed_rate\":" << Ratio(corrupt_changed,corrupt_count)
                  << ",\"clean_queries\":" << clean_count << ",\"false_change_rate\":" << Ratio(clean_changed,clean_count)
                  << "},\"calibrated_nll_nats\":";
#if BENCH_HAS_SCORES
        if(model.HasLanguageScores()) std::cout << Ratio(token_log_loss,nll_count);
        else
#endif
            std::cout << "null";
        std::cout << "}\n";
        return 0;
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
