#pragma once
#include <cstdint>
#include <vector>

namespace RadixFixtures {
inline void U32(std::vector<uint8_t>& data, uint32_t value) {
    for(unsigned shift=0;shift!=32;shift+=8)data.push_back(static_cast<uint8_t>(value>>shift));
}
inline void U64(std::vector<uint8_t>& data,uint64_t value) {
    while(value>=128){data.push_back(static_cast<uint8_t>((value&127)|128));value>>=7;}data.push_back(static_cast<uint8_t>(value));
}
inline std::vector<uint8_t> Fixed() {
    std::vector<uint8_t> result{'N','G','T','R'};U32(result,2);U32(result,12);U32(result,12);
    for(unsigned node=0;node!=12;++node) {
        const auto first=node<10?node+2:0;
        U32(result,first);U32(result,19);result.push_back(first?1:0);result.push_back(0);
        result.push_back(node<2?0:"hello"[(node-2)/2]);result.push_back(0);
    }
    return result;
}
inline std::vector<uint8_t> Radix(bool scored) {
    std::vector<uint8_t> result{'N','G','T','R'};U32(result,4);U32(result,4);U32(result,12);
    result.push_back(0);result.push_back(scored?10:8);if(scored){result.push_back(0);result.push_back(0);}
    result.push_back(0);result.push_back(scored?10:8);if(scored){result.push_back(174);result.push_back(252);} // log2(.1)*256 ~= -850
    for(int root=0;root!=2;++root) {
        result.push_back(5);for(const char c:"hello")if(c)result.push_back(static_cast<uint8_t>(c));
        result.push_back(scored?3:1);U64(result,uint64_t(UINT32_MAX)+7);
        if(scored){result.push_back(217);result.push_back(255);} // log2(.9)*256 ~= -39
    }
    return result;
}
}
