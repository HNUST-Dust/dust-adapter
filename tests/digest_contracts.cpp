#include "ethercat/digest.hpp"
#include <cassert>
#include <cstdio>
#include <vector>
static uint32_t reference(const uint8_t* p, size_t size) {
    uint32_t crc = ~0U;
    for (size_t n=0; n<size; ++n) {
        crc ^= p[n];
        for (unsigned bit=0; bit<8; ++bit)
            crc = (crc>>1)^((crc&1)?0xedb88320U:0U);
    }
    return crc^~0U;
}
int main() {
    using dust::ecat::detail::FastDigest;
    const uint8_t known[]="123456789";
    assert(FastDigest(known,9)==0xcbf43926U);
    assert(FastDigest(nullptr,0)==0);
    std::vector<uint8_t> bytes(4100); uint32_t random=0x12345678;
    for (auto &v:bytes) { random^=random<<13; random^=random>>17; random^=random<<5; v=random; }
    for (unsigned alignment=0; alignment<4; ++alignment)
        for (unsigned size=0; size<=4096; ++size)
            assert(FastDigest(bytes.data()+alignment,size)==reference(bytes.data()+alignment,size));
    for (unsigned bit=0; bit<96*8; ++bit) {
        bytes[bit/8]^=1U<<(bit%8);
        assert(FastDigest(bytes.data(),96)==reference(bytes.data(),96));
    }
    puts("CRC known/random/all-length/unaligned/mutation equivalence PASS");
}
