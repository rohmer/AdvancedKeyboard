#include "RadixModelBuilder.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>
namespace fs=std::filesystem;
std::vector<unsigned char> Read(const fs::path& path) {
    std::ifstream input(path,std::ios::binary);
    if(!input)throw std::runtime_error("Cannot read model or index");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void Write(const fs::path& path,const std::vector<unsigned char>& data) {
    std::ofstream output(path,std::ios::binary|std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));
    output.close();if(!output)throw std::runtime_error("Cannot write converted model or index");
}
int main(int argc,char** argv) {
    try {
        if(argc!=4)throw std::runtime_error("Usage: RadixConvert INPUT.ngtr INPUT.ngix OUTPUT.ngtr");
        const fs::path source=fs::absolute(argv[1]),source_index=fs::absolute(argv[2]);
        const fs::path target=fs::absolute(argv[3]);auto target_index=target;target_index.replace_extension(".ngix");
        if(source==target||source==target_index||source_index==target||source_index==target_index)
            throw std::runtime_error("Conversion outputs must differ from the source files");
        const auto raw=Read(source),index=Read(source_index);
        const auto result=espos::keyboard::prediction::ConvertToRadix(raw.data(),raw.size(),index.data(),index.size());
        Write(target,result.trie);Write(target_index,result.index);
        std::cout<<"{\"sourceCombinedBytes\":"<<(raw.size()+index.size())<<",\"convertedCombinedBytes\":"<<result.Bytes()
                 <<",\"logicalNodes\":"<<result.logical_nodes<<",\"physicalNodes\":"<<result.physical_nodes<<"}\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
