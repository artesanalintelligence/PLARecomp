// nnshim::opus: native Opus decoder (host libopus) for nn::codec::HardwareOpusDecoder
//(v1.5 stage 5). Without it, every packet goes over IPC to the HLE hwopus service.
//
// nn::codec packet format (the same the hwopus service accepts): 8-byte header
// {u32 big-endian Opus data size, u32 big-endian "final range"} followed by the data.
// Output: interleaved s16 PCM.
#pragma once

#include <cstddef>
#include <cstdint>

namespace nnshim::opus {

/// true if the shim was built with libopus (NNSHIM_WITH_OPUS).
bool Available();
/// Sample rates and channel counts the console hardware accepts (8/12/16/24/48 kHz, 1-2 channels).
bool ValidParams(int sample_rate, int channels);
/// Creates (or recreates) the decoder bound to `key` (address of the HardwareOpusDecoderType).
bool Create(uint64_t key, int sample_rate, int channels);
void Destroy(uint64_t key);
bool Exists(uint64_t key);
/// Samples per channel in an Opus packet (without the 8-byte header); < 0 if invalid.
int PacketSamples(const uint8_t* data, size_t len, int sample_rate);
/// Decodes a packet (without header). Returns samples per channel, or < 0 (libopus error).
/// `reset` clears the state first (ResetHardwareOpusDecoder).
int Decode(uint64_t key, const uint8_t* data, size_t len, int16_t* out, int max_samples_per_channel,
           bool reset);
/// Statistics for the log.
uint64_t decoded_packets();
uint64_t live_decoders();

}  // namespace nnshim::opus
