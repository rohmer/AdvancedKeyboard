// Native lookup microbenchmark. Query parsing, normalization and output are
// outside both the measured calls and C++ allocation-operator accounting.
#include "PredictionModel.h"
#include "PredictionPrefix.h"
#include "PredictionAutocorrect.h"
#include "Utf8Fold.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _MSC_VER
#include <malloc.h>
#endif
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sched.h>
#endif

namespace {
thread_local bool count_allocations = false;
thread_local std::uint64_t allocation_calls = 0;
void CountAllocation() noexcept { if(count_allocations) ++allocation_calls; }
void* Allocate(std::size_t size) {
    CountAllocation();
    if(void* result = std::malloc(size ? size : 1)) return result;
    throw std::bad_alloc();
}
void* AllocateAligned(std::size_t size, std::size_t alignment) {
    CountAllocation();
    void* result = nullptr;
#ifdef _MSC_VER
    result = _aligned_malloc(size ? size : 1, alignment);
#else
    if(posix_memalign(&result, alignment, size ? size : 1) != 0) result = nullptr;
#endif
    if(!result) throw std::bad_alloc();
    return result;
}
void FreeAligned(void* pointer) noexcept {
#ifdef _MSC_VER
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}
void* operator new(std::size_t size) { return Allocate(size); }
void* operator new[](std::size_t size) { return Allocate(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void* operator new(std::size_t size, std::align_val_t align) { return AllocateAligned(size, std::size_t(align)); }
void* operator new[](std::size_t size, std::align_val_t align) { return AllocateAligned(size, std::size_t(align)); }
void operator delete(void* pointer, std::align_val_t) noexcept { FreeAligned(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { FreeAligned(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { FreeAligned(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { FreeAligned(pointer); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch(...) { return nullptr; } }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { try { return Allocate(size); } catch(...) { return nullptr; } }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }

namespace prediction = espos::keyboard::prediction;
using Clock = std::chrono::steady_clock;
#if BENCH_MODEL_TRACE
namespace espos::keyboard::prediction { extern std::uint64_t benchmark_model_trace[8]; }
#endif
namespace {
std::vector<unsigned char> Read(const char* path) {
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
struct Query { std::array<std::string, 6> fields; int prefix = -1; };
struct Measurement {
    std::vector<double> microseconds;
    std::uint64_t allocations = 0;
    std::array<std::uint64_t, 8> operations{};
};
double Percentile(std::vector<double>& values, double fraction) {
    if(values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[std::size_t((values.size()-1)*fraction)];
}
std::uint64_t Hash(std::string_view value, std::uint64_t hash) {
    for(unsigned char c:value) { hash ^= c; hash *= 1099511628211ull; }
    return (hash ^ 0) * 1099511628211ull;
}
void Write(Measurement& value) {
    std::cout << "{\"queries\":" << value.microseconds.size()
        << ",\"p50_us\":" << Percentile(value.microseconds, .5)
        << ",\"p95_us\":" << Percentile(value.microseconds, .95)
        << ",\"cpp_allocation_calls\":" << value.allocations << ",\"operations\":[";
    for(std::size_t i=0;i<value.operations.size();++i) { if(i) std::cout << ','; std::cout << value.operations[i]; }
    std::cout << "]}";
}
}
int main(int argc, char** argv) {
    try {
        if(argc != 4 && argc != 5) throw std::runtime_error("Usage: RuntimeProfile MODEL INDEX QUERIES.tsv [LOGICAL_CPU]");
        int pinned_cpu=-1;
        if(argc==5) {
            pinned_cpu=std::stoi(argv[4]);
#ifdef _WIN32
            if(pinned_cpu<0 || pinned_cpu>=int(sizeof(DWORD_PTR)*8) ||
                !SetThreadAffinityMask(GetCurrentThread(),DWORD_PTR{1}<<pinned_cpu))
                throw std::runtime_error("Cannot pin benchmark thread to requested CPU");
#else
            if(pinned_cpu<0 || pinned_cpu>=CPU_SETSIZE) throw std::runtime_error("Invalid requested CPU");
            cpu_set_t set; CPU_ZERO(&set); CPU_SET(pinned_cpu,&set);
            if(sched_setaffinity(0,sizeof(set),&set)!=0) throw std::runtime_error("Cannot pin benchmark thread to requested CPU");
#endif
        }
        auto raw = Read(argv[1]), index = Read(argv[2]);
        prediction::Model model;
        const auto started = Clock::now();
        if(!model.Open(raw.data(),raw.size(),index.data(),index.size())) throw std::runtime_error("Invalid model/index");
        const auto open_ms = std::chrono::duration<double,std::milli>(Clock::now()-started).count();
        std::ifstream input(argv[3]);
        if(!input) throw std::runtime_error("Cannot read query panel");
        std::vector<Query> queries;
        std::string line;
        while(std::getline(input,line)) {
            if(!line.empty() && line.back()=='\r') line.pop_back();
            if(line.empty() || line[0]=='#') continue;
            auto values = Fields(line);
            if(values.size()!=6) throw std::runtime_error("Invalid query fields");
            Query query;
            for(std::size_t i=0;i<values.size();++i) {
                if(i==0) { query.fields[i]=values[i]; continue; }
                std::array<char,1024> buffer{}; std::string_view normalized;
                if(!prediction::PrepareKeyboardQuery(values[i],buffer.data(),buffer.size(),normalized,"fr-FR",i!=4))
                    throw std::runtime_error("Invalid query normalization");
                query.fields[i].assign(normalized);
            }
            const auto& kind = query.fields[0];
            if(kind.size()==2 && kind[0]=='p' && kind[1]>='0' && kind[1]<='3') query.prefix=kind[1]-'0';
            else if(kind!="clean" && kind!="delete") throw std::runtime_error("Unknown query kind");
            queries.push_back(std::move(query));
        }
        prediction::PrefixScratch prefix_scratch{};
        prediction::AutocorrectScratch correction_scratch;
        std::array<prediction::Suggestion,prediction::kSuggestionCount> suggestions{};
        std::array<Measurement,6> measurements;
        for(auto& measurement:measurements) measurement.microseconds.reserve(queries.size());
        std::uint64_t completion_hash=14695981039346656037ull,correction_hash=completion_hash;
        for(int pass=0;pass<2;++pass) {
            for(const auto& query:queries) {
                const auto& fields=query.fields;
                auto& measurement=measurements[query.prefix>=0 ? query.prefix : fields[0]=="clean" ? 4 : 5];
#if BENCH_MODEL_TRACE
                std::fill(std::begin(prediction::benchmark_model_trace),std::end(prediction::benchmark_model_trace),0);
#endif
                const auto allocations_before=allocation_calls;
                prediction::AutocorrectResult correction;
                std::size_t count=0; bool found=false;
                const auto call_started=Clock::now();
                count_allocations=true;
                if(query.prefix>=0) count=prediction::Suggest(model,prefix_scratch,fields[4],fields[2],fields[3],suggestions.data(),suggestions.size());
                else found=prediction::BestAutocorrection(model,correction_scratch,fields[5],correction,fields[2],fields[3]);
                count_allocations=false;
                const auto elapsed=std::chrono::duration<double,std::micro>(Clock::now()-call_started).count();
                if(pass==0) continue;
                measurement.microseconds.push_back(elapsed);
                measurement.allocations+=allocation_calls-allocations_before;
#if BENCH_MODEL_TRACE
                for(std::size_t i=0;i<measurement.operations.size();++i) measurement.operations[i]+=prediction::benchmark_model_trace[i];
#endif
                if(query.prefix>=0) for(std::size_t i=0;i<count;++i) completion_hash=Hash(suggestions[i].Word(),completion_hash);
                else correction_hash=Hash(found && correction.probability>=.3 && correction.word!=fields[5] ? correction.word : std::string_view(fields[5]),correction_hash);
            }
        }
        std::cout<<std::setprecision(12)<<"{\"model_bytes\":"<<raw.size()<<",\"index_bytes\":"<<index.size()
            <<",\"open_ms\":"<<open_ms<<",\"warmup_passes\":1,\"pinned_logical_cpu\":"<<pinned_cpu
            <<",\"model_object_bytes\":"<<sizeof(model)<<",\"cursor_object_bytes\":"<<sizeof(prediction::Model::Cursor)
            <<",\"prefix_scratch_bytes\":"<<sizeof(prefix_scratch)<<",\"correction_scratch_bytes\":"<<sizeof(correction_scratch)
            <<",\"completion_fingerprint\":"<<completion_hash<<",\"correction_fingerprint\":"<<correction_hash<<",\"by_kind\":[";
        for(std::size_t i=0;i<measurements.size();++i) { if(i) std::cout<<','; Write(measurements[i]); }
        std::cout<<"]}\n";
        return 0;
    } catch(const std::exception& error) { count_allocations=false; std::cerr<<error.what()<<'\n'; return 1; }
}
