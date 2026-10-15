// nvntrace .nvt file format (little endian). Read by tools/nvntrace_view.py.
//
//   FileHeader
//   num_fns x { u16 len; char name[len]; }   (padded to 8)
//   records: RecHeader + payload (+ padding to 8; RecHeader.size includes everything)
//
// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace nvt {

constexpr char kMagic[8] = {'N', 'V', 'N', 'T', 'R', 'C', '0', '1'};
constexpr uint32_t kVersion = 1;

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    uint32_t version;
    uint32_t num_fns;
    uint64_t sdk_base;
    uint64_t main_base;
};

enum RecType : uint8_t {
    REC_CALL = 1,   // CallRec: entry into an NVN function
    REC_RET = 2,    // RetRec: return (only functions with a post)
    REC_BLOB = 3,   // BlobHeader + bytes: guest memory
    REC_FRAME = 4,  // nvnQueuePresentTexture: the next frame starts here
    REC_NOTE = 5,   // text
};

struct RecHeader {
    uint32_t size;  // bytes of the whole record, header included
    uint8_t type;
    uint8_t pad;
    uint16_t fn;    // NVN index (0xFFFF = none)
    uint32_t tid;   // host thread
    uint32_t frame;
    uint64_t seq;
};

struct CallRec {
    uint64_t x[8];  // X0-X7
    uint64_t v[4];  // low half of V0-V3 (float arguments)
    uint64_t lr;    // return address in the guest (caller)
    uint64_t sp;
};

struct RetRec {
    uint64_t x0;
    uint64_t v0;
};

enum BlobKind : uint32_t {
    BLOB_ARG = 0,             // memory pointed to by an argument, on entry
    BLOB_OBJECT = 1,          // X0 object after Initialize (driver's internal layout)
    BLOB_SHADER_CONTROL = 2,  // control section of a ShaderData
    BLOB_SHADER_CODE = 3,     // shader code (addr = GPU address)
    BLOB_OUT = 4,             // output pointers, on return
};

struct BlobHeader {
    uint64_t addr;
    uint32_t len;
    uint32_t kind;
};
#pragma pack(pop)

}  // namespace nvt
