#pragma once
#include <cstdint>
#include <cstddef>
inline uint32_t crc32_ieee_update(uint32_t crc,const uint8_t *p,size_t size) {
    crc=~crc;
    for(size_t i=0;i<size;++i) {crc^=p[i];for(int b=0;b<8;++b)crc=(crc>>1)^(0xedb88320U & (0U-(crc&1)));}
    return ~crc;
}
