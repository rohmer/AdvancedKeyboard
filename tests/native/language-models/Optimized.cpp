#include "Generator.h"
#include "TrainingModel.h"
#include <nlohmann/json.hpp>
#include <lv_advanced_keyboard_prediction.h>
#include <PredictionModel.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;
void Require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Bytes Read(const fs::path& path) { std::ifstream input(path, std::ios::binary); Require(bool(input), "Cannot read model"); return Bytes(std::istreambuf_iterator<char>(input), {}); }
void Write(const fs::path& path, const std::string& value) { std::ofstream output(path, std::ios::binary); output << value; Require(bool(output), "Cannot write fixture"); }
std::uint32_t U32(const Bytes& data, unsigned offset) { return data.at(offset) | (std::uint32_t(data.at(offset+1))<<8) | (std::uint32_t(data.at(offset+2))<<16) | (std::uint32_t(data.at(offset+3))<<24); }
template<class Action> void Reject(Action action) { bool rejected = false; try { action(); } catch (const std::exception&) { rejected = true; } Require(rejected, "Invalid generator request accepted"); }
void CheckModel(const fs::path& path, bool scored) {
    auto indexPath = path; indexPath.replace_extension(".ngix"); auto bytes = Read(path), index = Read(indexPath);
    Require(U32(bytes,4)==4 && U32(index,4)==2, "Wrong radix/index version");
    espos::keyboard::prediction::Model model;
    Require(model.Open(bytes.data(),bytes.size(),index.data(),index.size()), "Generated model/index failed structural validation");
    Require(model.HasLanguageScores()==scored, "Scoring metadata was lost");
    void* rebuilt = nullptr; std::size_t size = 0;
    Require(lv_advanced_keyboard_build_trie_index(bytes.data(),bytes.size(),&rebuilt,&size), "Public index builder rejected radix model");
    Require(size==index.size() && std::equal(index.begin(),index.end(),static_cast<const std::uint8_t*>(rebuilt)), "C facade and encoder indices differ");
    lv_advanced_keyboard_free_trie_index(rebuilt);
    auto* predictor=lv_advanced_keyboard_predictor_create(bytes.data(),bytes.size()); Require(predictor, "Public C predictor cannot load radix output");
    lv_advanced_keyboard_suggestion_t suggestions[3]{};
    Require(lv_advanced_keyboard_predictor_suggest_folded(predictor,"",0,"the",3,nullptr,0,suggestions,3)>0,
            "Generated radix model gives no real context suggestions");
    lv_advanced_keyboard_predictor_destroy(predictor);
}
int main(int argc, char** argv) {
    try {
        Require(argc==2,"Expected output directory"); const auto work=fs::absolute(fs::u8path(argv[1])) /
            ("run-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));fs::create_directories(work);
        const auto input=work/"corpus.txt";std::string corpus;
        for(int i=0;i<100;++i) corpus += "the cat sat on the mat. the dog ran down the road. the caterpillar crawled along the road.\n";
        Write(input,corpus);
        LanguageModels::Options options;options.inputs={input};options.useWordList=false;options.workers=4;
        options.memoryBytes=32ull*1024*1024;options.chunkEntries=7;options.lowMemory=true;options.useCache=true;
        options.cacheDirectory=work/"cache";options.scratchDirectory=work/"scratch";
        options.format=LanguageModels::ModelFormat::Radix;options.maximumBytes=4096;options.profileSizes={2048,4096,2048};
        options.trieOutput=work/"counts.ngtr";
        const auto cold=LanguageModels::Generate(options);Require(!cold.trainingCacheHit,"Cold run reused stale training artifact");
        Require(cold.profileOutputs.size()==2 && cold.trieBytes+cold.indexBytes<=4096,"Profile size/dedup incorrect");
        CheckModel(options.trieOutput,false);CheckModel(work/"counts-2048.ngtr",false);
        Require(Read(work/"counts-2048.ngtr").size()+Read(work/"counts-2048.ngix").size()<=2048,"Batch profile exceeds combined budget");
        const auto before=Read(options.trieOutput);options.workers=1;
        const auto warm=LanguageModels::Generate(options);Require(warm.trainingCacheHit && warm.cacheHits==1,"Merged/ranked artifact not reused");
        Require(before==Read(options.trieOutput),"Cached model bytes differ");
        options.smoothing=LanguageModels::Smoothing::ModifiedKneserNey;options.trieOutput=work/"smoothed.ngtr";options.jsonOutput=work/"smoothed.json";
        const auto smoothed=LanguageModels::Generate(options);Require(smoothed.trainingCacheHit,"Smoothing needlessly recounted corpus");CheckModel(options.trieOutput,true);
        nlohmann::json json;std::ifstream jsonInput(options.jsonOutput);jsonInput>>json;jsonInput.close();
        Require(json.at("smoothing")=="modified-kn" && json.at("unknownProbability").is_number(),"JSON omits real model scores");
        const auto smoothedBefore=Read(options.trieOutput);const auto cached=LanguageModels::Generate(options);
        Require(cached.trainingCacheHit && smoothedBefore==Read(options.trieOutput),"Cached scored profile changed");
        auto invalid=options;invalid.memoryBytes=1024;Reject([&]{LanguageModels::Generate(invalid);});
        invalid=options;invalid.maximumBytes=63;Reject([&]{LanguageModels::Generate(invalid);});
        invalid=options;invalid.trieOutput=work/"collision.ngix";Reject([&]{LanguageModels::Generate(invalid);});
        invalid=options;invalid.jsonOutput=work/"smoothed-2048.ngix";Reject([&]{LanguageModels::Generate(invalid);});
#ifdef _WIN32
        invalid=options;invalid.jsonOutput=work/"SMOOTHED-2048.NGIX";Reject([&]{LanguageModels::Generate(invalid);});
#endif
        invalid=options;invalid.format=LanguageModels::ModelFormat::Compatibility;Reject([&]{LanguageModels::Generate(invalid);});
        // Cancellation at the commit boundary preserves the complete old pair.
        auto publishing=options;publishing.profileSizes.clear();publishing.jsonOutput.clear();
        publishing.smoothing=LanguageModels::Smoothing::Counts;publishing.maximumBytes=128;
        auto originalIndex=options.trieOutput;originalIndex.replace_extension(".ngix");
        const auto indexBefore=Read(originalIndex);std::atomic_bool cancel{false};bool publishingReached=false;
        Reject([&]{LanguageModels::Generate(publishing,[&](const LanguageModels::Progress& progress){
            if(progress.stage=="Publishing model and index"){publishingReached=true;cancel=true;}
        },&cancel);});
        Require(publishingReached && Read(options.trieOutput)==smoothedBefore && Read(originalIndex)==indexBefore,
                "Cancellation changed the previously valid model/index pair");
        // Inject an OS rename failure after preflight: the second replacement
        // meets a directory. Restore our held model, then verify sidecar rollback.
        auto held=options.trieOutput;held += ".held";bool blocked=false;
        Reject([&]{LanguageModels::Generate(publishing,[&](const LanguageModels::Progress& progress){
            if(progress.stage=="Publishing model and index"){
                fs::rename(options.trieOutput,held);fs::create_directory(options.trieOutput);blocked=true;
            }
        });});
        Require(blocked,"Publication failure injection was not reached");
        fs::remove(options.trieOutput);fs::rename(held,options.trieOutput);
        Require(Read(options.trieOutput)==smoothedBefore && Read(originalIndex)==indexBefore,
                "Failed model replacement did not restore the old index");CheckModel(options.trieOutput,true);
        // A finite requested vocabulary fits even when the complete candidate
        // universe would exceed the unlimited candidate-pool reservation.
        auto limited=publishing;limited.memoryBytes=8ull*1024*1024;limited.workers=1;limited.useCache=false;
        limited.chunkEntries=1000;limited.maximumBytes.reset();limited.limits={100,0,0};
        limited.trieOutput=work/"limited.ngtr";const auto finiteInput=work/"finite.txt";std::string finiteCorpus;
        for(unsigned word=0;word<5000;++word){
            auto value=word;std::string spelling="word";
            for(int digit=0;digit<4;++digit){spelling+=char('a'+value%26);value/=26;}
            finiteCorpus+=spelling+".\n";
        }
        Write(finiteInput,finiteCorpus);limited.inputs={finiteInput};
        const auto finite=LanguageModels::Generate(limited);Require(finite.counts[0]==100,"Finite unlimited-profile limit was rejected or truncated");
        limited.limits[2].reset();
        Require(LanguageModels::Generate(limited).counts[0]==100,"Absent higher-order grams made a fitting partial limit fail");
        const auto longInput=work/"long.txt";std::string longCorpus;
        for(int word=0;word<1000;++word)longCorpus+="small.\n";
        for(int word=0;word<12;++word)longCorpus+=std::string(16384,char('a'+word))+".\n";
        Write(longInput,longCorpus);limited.inputs={longInput};limited.maximumBytes=256;limited.limits={};limited.trieOutput=work/"long.ngtr";
        const auto bounded=LanguageModels::Generate(limited);
        Require(bounded.trieBytes+bounded.indexBytes<=256 && bounded.counts[0]>0,"Encoder memory probe rejected a fitting smaller profile");
        const auto longBefore=Read(limited.trieOutput);auto longIndex=limited.trieOutput;longIndex.replace_extension(".ngix");const auto longIndexBefore=Read(longIndex);
        limited.maximumBytes.reset();bool memoryRejected=false;
        try{LanguageModels::Generate(limited);}catch(const std::exception& error){memoryRejected=std::string(error.what()).find("RAM")!=std::string::npos;}
        Require(memoryRejected && Read(limited.trieOutput)==longBefore && Read(longIndex)==longIndexBefore,
                "Oversized unlimited encoder failed to preserve its old outputs");
        // Corrupt the scored and ranked layers: the original corpus still wins,
        // and valid lower-level file counts may be reused safely.
        for(const auto& item:fs::directory_iterator(options.cacheDirectory))
            if(item.path().extension()==".scores" || item.path().extension()==".training") Write(item.path(),"bad");
        const auto recovered=LanguageModels::Generate(options);Require(smoothedBefore==Read(options.trieOutput),"Corrupt training cache changed model");
        Require(!recovered.trainingCacheHit,"Corrupt ranked artifact accepted");
        std::cout<<"Word-ID count parity, combined profiles, scored cache, public runtime, corruption and memory validation passed\n";return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
