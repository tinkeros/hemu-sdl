#pragma once
#include "types.h"
#include <unordered_map>
#include <array>
#include <cstddef>

class Qcow2 {
public:
    Qcow2(U8* buf, size_t len);

    void readInto(U64 lba, U64 count, U8* dst, U64 dstOff);
    void writeInto(U64 lba, U64 count, U8* src, U64 srcOff);

    U64 virtualSize() const { return m_virtualSize; }

private:
    U8*    buf;
    size_t bufLen;
    int    clusterBits;
    int    l1Size;
    U64    clusterSize;
    U64    l1Off;
    int    l2Entries;
    int    l2Bits;
    U64    m_virtualSize;

    std::unordered_map<U64, std::array<U8, 512>> overlay;

    int64_t clusterAt(U64 gIdx);
};
