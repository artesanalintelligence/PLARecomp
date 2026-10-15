// nnshim::opus: host libopus decoders, one per guest HardwareOpusDecoderType.
// See include/nnshim/opus.hpp. Without NNSHIM_WITH_OPUS everything returns "unavailable" and the
// adapters leave the calls to the SDK (HLE hwopus service).
#include "nnshim/opus.hpp"

#include <atomic>
#include <map>
#include <mutex>

#ifdef NNSHIM_WITH_OPUS
#include <opus.h>
#endif

namespace nnshim::opus {

namespace {
#ifdef NNSHIM_WITH_OPUS
std::mutex g_m;
std::map<uint64_t, OpusDecoder*> g_dec;
#endif
std::atomic<uint64_t> g_packets{0};
}  // namespace

bool Available() {
#ifdef NNSHIM_WITH_OPUS
    return true;
#else
    return false;
#endif
}

bool ValidParams(int sample_rate, int channels) {
    const bool rate_ok = sample_rate == 8000 || sample_rate == 12000 || sample_rate == 16000 ||
                         sample_rate == 24000 || sample_rate == 48000;
    return rate_ok && (channels == 1 || channels == 2);
}

bool Create(uint64_t key, int sample_rate, int channels) {
#ifdef NNSHIM_WITH_OPUS
    if (!ValidParams(sample_rate, channels)) return false;
    int err = 0;
    OpusDecoder* d = opus_decoder_create(sample_rate, channels, &err);
    if (!d || err != OPUS_OK) return false;
    std::lock_guard lk(g_m);
    auto it = g_dec.find(key);
    if (it != g_dec.end()) {
        opus_decoder_destroy(it->second);
        it->second = d;
    } else {
        g_dec.emplace(key, d);
    }
    return true;
#else
    (void)key, (void)sample_rate, (void)channels;
    return false;
#endif
}

void Destroy(uint64_t key) {
#ifdef NNSHIM_WITH_OPUS
    std::lock_guard lk(g_m);
    auto it = g_dec.find(key);
    if (it == g_dec.end()) return;
    opus_decoder_destroy(it->second);
    g_dec.erase(it);
#else
    (void)key;
#endif
}

bool Exists(uint64_t key) {
#ifdef NNSHIM_WITH_OPUS
    std::lock_guard lk(g_m);
    return g_dec.count(key) != 0;
#else
    (void)key;
    return false;
#endif
}

int PacketSamples(const uint8_t* data, size_t len, int sample_rate) {
#ifdef NNSHIM_WITH_OPUS
    if (!data || len == 0) return -1;
    return opus_packet_get_nb_samples(data, static_cast<opus_int32>(len), sample_rate);
#else
    (void)data, (void)len, (void)sample_rate;
    return -1;
#endif
}

int Decode(uint64_t key, const uint8_t* data, size_t len, int16_t* out, int max_samples_per_channel,
           bool reset) {
#ifdef NNSHIM_WITH_OPUS
    // A decoder is used by one thread at a time (Wwise's); the global lock only protects
    // the map, but holding it during the call is cheap and simple.
    std::lock_guard lk(g_m);
    auto it = g_dec.find(key);
    if (it == g_dec.end()) return OPUS_INVALID_STATE;
    if (reset) opus_decoder_ctl(it->second, OPUS_RESET_STATE);
    const int n = opus_decode(it->second, data, static_cast<opus_int32>(len), out,
                              max_samples_per_channel, 0);
    if (n >= 0) g_packets.fetch_add(1, std::memory_order_relaxed);
    return n;
#else
    (void)key, (void)data, (void)len, (void)out, (void)max_samples_per_channel, (void)reset;
    return -1;
#endif
}

uint64_t decoded_packets() { return g_packets.load(); }

uint64_t live_decoders() {
#ifdef NNSHIM_WITH_OPUS
    std::lock_guard lk(g_m);
    return g_dec.size();
#else
    return 0;
#endif
}

}  // namespace nnshim::opus
