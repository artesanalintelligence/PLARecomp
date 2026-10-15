// nnshim: AAPCS64 adapters for nn::codec::HardwareOpusDecoder ("audio" family,
// v1.5 stage 5). With them the SDK's "hardware" Opus decoder is host libopus and the HLE hwopus
// service stops receiving requests.
//
// HardwareOpusDecoderType layout (own disassembly of the PLA 1.1.1 sdk module):
//   +0x00 pointer to the service object (IHardwareOpusDecoder proxy): the shim stores a non-zero
//         sentinel (HardwareOpusDecoder::IsInitialized only checks that it is not 0)
//   +0x08 work buffer, +0x10 its size
//   +0x18 s32 sample rate (GetHardwareOpusDecoderSampleRate), +0x1c s32 channels
//   +0x20 u64 processing time of the last packet (GetHardwareOpusDecoderProcessingTime x 1000)
//   +0x28 u8 "reset pending" (ResetHardwareOpusDecoder sets it to 1, decoding clears it)
// The HardwareOpusDecoder class holds it at +8 and calls the C functions through the sdk PLT,
// except HardwareOpusDecoder::Finalize, which frees the proxy through its vtable: hooked too.
// Decode return codes (the SDK's): 0 ok, 6 invalid or incomplete packet, 7 output buffer too
// small; 4 = decoder error.
//
// Per-resource routing: a decoder the shim did not create (not in nnshim::opus) stays in the
// SDK (PLA_CONTINUE), so nothing gets mixed even if the shim is enabled midway.
#include <atomic>
#include <cstring>
#include <vector>

#include "abi_common.hpp"
#include "nnshim/opus.hpp"

namespace nnshim {
namespace {

constexpr uint64_t kSentinel = 0x5355504F48534E4Eull;   // "NNSHOPUS"
std::atomic<uint64_t> g_opus_last_log{0}, g_opus_last_n{0};

void LogRate() {
    const uint64_t now = UptimeMs();
    uint64_t prev = g_opus_last_log.load();
    if (now - prev < 10000 || !g_opus_last_log.compare_exchange_strong(prev, now)) return;
    const uint64_t n = opus::decoded_packets();
    Log(PLA_LOG_INFO, "nnshim opus: %llu packets/s, %llu live decoders, %llu total",
        static_cast<unsigned long long>((n - g_opus_last_n.exchange(n)) * 1000 / (now - prev ? now - prev : 1)),
        static_cast<unsigned long long>(opus::live_decoders()), static_cast<unsigned long long>(n));
}

bool FinalizeType(PlaGuest& g, uint64_t type) {
    if (!opus::Exists(type)) return false;
    opus::Destroy(type);
    const uint8_t zero[0x20] = {};
    g.write(type, zero, sizeof zero);
    Log(PLA_LOG_INFO, "nnshim opus: decoder %#llx closed", static_cast<unsigned long long>(type));
    return true;
}

}  // namespace
}  // namespace nnshim

using namespace nnshim;

// size_t nn::codec::GetHardwareOpusDecoderWorkBufferSize(int sampleRate, int channelCount)
NNSHIM_ADAPTER(Audio, nnshim_opus_GetHardwareOpusDecoderWorkBufferSize) {
    PlaGuest g(ctx);
    if (!opus::Available()) return PLA_CONTINUE;
    const int rate = static_cast<int32_t>(g.x(0)), ch = static_cast<int32_t>(g.x(1));
    // The game's buffer is unused (state lives on the host); any valid size will do.
    g.set_x(0, opus::ValidParams(rate, ch) ? 0x1000 : 0);
    return PLA_SKIP;
}

// Result nn::codec::InitializeHardwareOpusDecoder(HardwareOpusDecoderType*, int sampleRate,
//                                                  int channelCount, void* buffer, size_t size, int)
NNSHIM_ADAPTER(Audio, nnshim_opus_InitializeHardwareOpusDecoder) {
    PlaGuest g(ctx);
    if (!opus::Available()) return PLA_CONTINUE;
    const uint64_t type = g.x(0), buf = g.x(3), size = g.x(4);
    const int rate = static_cast<int32_t>(g.x(1)), ch = static_cast<int32_t>(g.x(2));
    if (!type || !opus::Create(type, rate, ch)) {
        Log(PLA_LOG_WARNING, "nnshim opus: could not create the decoder (%d Hz, %d channels): left to the SDK",
            rate, ch);
        return PLA_CONTINUE;
    }
    uint8_t st[0x30] = {};
    std::memcpy(st + 0x00, &kSentinel, 8);
    std::memcpy(st + 0x08, &buf, 8);
    std::memcpy(st + 0x10, &size, 8);
    std::memcpy(st + 0x18, &rate, 4);
    std::memcpy(st + 0x1c, &ch, 4);
    g.write(type, st, 0x29);
    Log(PLA_LOG_INFO, "nnshim opus: decoder %#llx (%d Hz, %d channels)", static_cast<unsigned long long>(type),
        rate, ch);
    g.set_x(0, 0);
    return PLA_SKIP;
}

// int nn::codec::DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType*, size_t* consumed,
//     int* sampleCount, int16_t* out, size_t outSize, const void* in, size_t inSize)
NNSHIM_ADAPTER(Audio, nnshim_opus_DecodeOpusInterleavedWithHardware) {
    PlaGuest g(ctx);
    const uint64_t type = g.x(0);
    if (!opus::Exists(type)) return PLA_CONTINUE;
    const uint64_t consumed_va = g.x(1), samples_va = g.x(2), out_va = g.x(3), out_size = g.x(4),
                   in_va = g.x(5), in_size = g.x(6);
    const auto ret = [&](uint64_t r) { g.set_x(0, r); return PLA_SKIP; };
    if (in_size < 8) return ret(6);
    uint8_t hdr[8];
    if (g.read(in_va, hdr, 8) != 8) return ret(6);
    const uint32_t len = (uint32_t{hdr[0]} << 24) | (uint32_t{hdr[1]} << 16) | (uint32_t{hdr[2]} << 8) | hdr[3];
    if (uint64_t{len} + 8 > in_size || len == 0 || len > 0x10000) return ret(6);
    uint8_t st[0x11] = {};   // +0x18 sample rate, +0x1c channels, +0x20 time, +0x28 reset
    if (g.read(type + 0x18, st, sizeof st) != sizeof st) return ret(6);
    int32_t rate, ch;
    std::memcpy(&rate, st + 0, 4);
    std::memcpy(&ch, st + 4, 4);
    const uint8_t reset_flag = st[0x10];
    if (!opus::ValidParams(rate, ch)) return ret(6);
    std::vector<uint8_t> pkt(len);
    if (g.read(in_va + 8, pkt.data(), len) != len) return ret(6);
    const int per_ch = opus::PacketSamples(pkt.data(), len, rate);
    if (per_ch <= 0) return ret(6);
    if (out_size < static_cast<uint64_t>(per_ch) * static_cast<uint64_t>(ch) * 2) return ret(7);
    std::vector<int16_t> pcm(static_cast<size_t>(per_ch) * static_cast<size_t>(ch));
    const int n = opus::Decode(type, pkt.data(), len, pcm.data(), per_ch, reset_flag != 0);
    if (reset_flag) {
        const uint8_t z = 0;
        g.write(type + 0x28, &z, 1);
    }
    if (n < 0) return ret(4);
    g.write(out_va, pcm.data(), static_cast<size_t>(n) * static_cast<size_t>(ch) * 2);
    const uint64_t consumed = uint64_t{len} + 8, zero_time = 0;
    const int32_t samples = n;
    if (consumed_va) g.write(consumed_va, &consumed, 8);
    if (samples_va) g.write(samples_va, &samples, 4);
    g.write(type + 0x20, &zero_time, 8);
    LogRate();
    return ret(0);
}

// void nn::codec::FinalizeHardwareOpusDecoder(HardwareOpusDecoderType*)
NNSHIM_ADAPTER(Audio, nnshim_opus_FinalizeHardwareOpusDecoder) {
    PlaGuest g(ctx);
    return FinalizeType(g, g.x(0)) ? PLA_SKIP : PLA_CONTINUE;
}

// void nn::codec::HardwareOpusDecoder::Finalize()   (this + 8 = HardwareOpusDecoderType)
NNSHIM_ADAPTER(Audio, nnshim_opus_HardwareOpusDecoder_Finalize) {
    PlaGuest g(ctx);
    return FinalizeType(g, g.x(0) + 8) ? PLA_SKIP : PLA_CONTINUE;
}
