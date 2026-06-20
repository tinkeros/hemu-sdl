//============================================================================
// qcow2.cpp -- minimal qcow2 reader (translated from Hemu-wasm/qcow2.js).
// Supports read-only access with an in-memory write overlay.
// All multi-byte qcow2 header/table fields are big-endian.
//============================================================================
#include "qcow2.h"
#include <cstring>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------
static inline U32 be32(const U8* p)
{
    return ((U32)p[0] << 24) | ((U32)p[1] << 16) | ((U32)p[2] << 8) | (U32)p[3];
}

static inline U64 be64(const U8* p)
{
    return ((U64)p[0] << 56) | ((U64)p[1] << 48) | ((U64)p[2] << 40) | ((U64)p[3] << 32) |
           ((U64)p[4] << 24) | ((U64)p[5] << 16) | ((U64)p[6] <<  8) | (U64)p[7];
}

// ---------------------------------------------------------------------------
// Qcow2 constructor
// ---------------------------------------------------------------------------
Qcow2::Qcow2(U8* buf_, size_t len)
    : buf(buf_), bufLen(len)
{
    if (len < 72 || be32(buf) != 0x514649fb)
        throw std::runtime_error("not a qcow2 image");

    clusterBits = (int)be32(buf + 20);
    clusterSize = (U64)1 << clusterBits;
    l1Size      = (int)be32(buf + 36);
    l1Off       = be64(buf + 40);
    l2Entries   = (int)(clusterSize >> 3);
    l2Bits      = clusterBits - 3;
    m_virtualSize = be64(buf + 24);
}

// ---------------------------------------------------------------------------
// clusterAt -- host file offset of a guest cluster, or -1 (unallocated)
// ---------------------------------------------------------------------------
static const U64 OFF_MASK = 0x00FFFFFFFFFFFE00ULL;  // bits 9..55

int64_t Qcow2::clusterAt(U64 gIdx)
{
    int l1i = (int)(gIdx >> l2Bits);
    if (l1i >= l1Size) return -1;

    U64 l1e = be64(buf + l1Off + (U64)l1i * 8) & OFF_MASK;
    if (l1e == 0) return -1;

    int l2i = (int)(gIdx & (U64)(l2Entries - 1));
    U64 l2e = be64(buf + l1e + (U64)l2i * 8);

    if (l2e & ((U64)1 << 62))
        throw std::runtime_error("qcow2 compressed cluster (unsupported)");

    U64 off = l2e & OFF_MASK;
    return off == 0 ? -1 : (int64_t)off;
}

// ---------------------------------------------------------------------------
// readInto -- fill count*512 bytes at dst[dstOff]
// ---------------------------------------------------------------------------
void Qcow2::readInto(U64 lba, U64 count, U8* dst, U64 dstOff)
{
    for (U64 s = 0; s < count; s++) {
        U64 o = dstOff + s * 512;

        // Check overlay first (prior writes)
        auto it = overlay.find(lba + s);
        if (it != overlay.end()) {
            memcpy(dst + o, it->second.data(), 512);
            continue;
        }

        U64 gOff = (lba + s) * 512;
        U64 gIdx = gOff / clusterSize;
        U64 inC  = gOff % clusterSize;

        int64_t c = clusterAt(gIdx);
        if (c < 0) {
            memset(dst + o, 0, 512);
        } else {
            U64 srcOff = (U64)c + inC;
            if (srcOff + 512 <= bufLen) {
                memcpy(dst + o, buf + srcOff, 512);
            } else {
                memset(dst + o, 0, 512);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// writeInto -- ATA writes land in the overlay
// ---------------------------------------------------------------------------
void Qcow2::writeInto(U64 lba, U64 count, U8* src, U64 srcOff)
{
    for (U64 s = 0; s < count; s++) {
        std::array<U8, 512> sector;
        memcpy(sector.data(), src + srcOff + s * 512, 512);
        overlay[lba + s] = sector;
    }
}
