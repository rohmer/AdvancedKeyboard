#include "RadixModelBuilder.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs=std::filesystem;
std::vector<uint8_t> Read(const fs::path& path) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file)throw std::runtime_error("Cannot open input: "+path.u8string());
    const auto length=file.tellg();
    if(length<0||length>16*1024*1024)throw std::runtime_error("Input exceeds 16 MiB");
    std::vector<uint8_t> data(static_cast<size_t>(length));file.seekg(0);
    if(!data.empty())file.read(reinterpret_cast<char*>(data.data()),data.size());
    if(!file)throw std::runtime_error("Cannot read input: "+path.u8string());return data;
}
void WriteNew(const fs::path& path,const std::vector<uint8_t>& data) {
    if(fs::exists(path))throw std::runtime_error("Output already exists: "+path.u8string());
    if(path.has_parent_path())fs::create_directories(path.parent_path());
    auto temporary=path;temporary+=".writing";
    if(fs::exists(temporary))throw std::runtime_error("Temporary output already exists: "+temporary.u8string());
    try {
        {std::ofstream file(temporary,std::ios::binary);file.write(reinterpret_cast<const char*>(data.data()),data.size());
            file.close();if(!file)throw std::runtime_error("Cannot write output");}
        // copy_options::none refuses a destination created by another process.
        fs::copy_file(temporary,path,fs::copy_options::none);fs::remove(temporary);
    }catch(...){std::error_code error;fs::remove(temporary,error);throw;}
}
int main(int argc,char** argv) {
    try {
        if(argc!=5){std::cerr<<"Usage: UICreatorRadixConvert input.ngtr input.ngix|- output.ngtr output.ngix\n";return 2;}
        const auto source=fs::u8path(argv[1]),indexPath=fs::u8path(argv[2]);
        const auto output=fs::u8path(argv[3]),outputIndex=fs::u8path(argv[4]);
        if(fs::weakly_canonical(output)==fs::weakly_canonical(outputIndex)||fs::exists(output)||fs::exists(outputIndex))
            throw std::invalid_argument("Choose two different new output files");
        const auto model=Read(source),index=std::string(argv[2])=="-"?std::vector<uint8_t>{}:Read(indexPath);
        auto encoded=espos::keyboard::prediction::ConvertToRadix(model.data(),model.size(),index.data(),index.size());
        WriteNew(output,encoded.trie);WriteNew(outputIndex,encoded.index);
        std::cout<<"Logical nodes "<<encoded.logical_nodes<<"; physical edges "<<encoded.physical_nodes
                 <<"; model+index bytes "<<model.size()+index.size()<<" -> "<<encoded.Bytes()<<'\n';return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
