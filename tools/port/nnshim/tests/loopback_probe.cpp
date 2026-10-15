// loopback_probe: measures what plays on the default audio output (WASAPI loopback).
//
//   loopback_probe.exe <seconds> [output.wav]
//
// Prints one line per second with peak and RMS (dBFS) and, if requested, writes a 16-bit WAV.
// Used to check without ears that the game makes sound (nnshim audio). It captures EVERYTHING
// playing on the machine, so no other application should be playing audio.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    const int seconds = argc > 1 ? std::atoi(argv[1]) : 10;
    const char* wav = argc > 2 ? argv[2] : nullptr;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) ||
        FAILED(ac->GetMixFormat(&fmt)) ||
        FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, fmt, nullptr)) ||
        FAILED(ac->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap)))) {
        std::printf("ERROR: could not open the loopback capture\n");
        return 2;
    }
    // The shared mix format is float32 in practice.
    const bool is_float = fmt->wBitsPerSample == 32;
    const int ch = fmt->nChannels;
    std::printf("device: %u Hz, %d channels, %u bits\n", static_cast<unsigned>(fmt->nSamplesPerSec), ch,
                fmt->wBitsPerSample);
    std::vector<int16_t> out;
    ac->Start();
    double total_peak = 0;
    int loud_seconds = 0;
    for (int sec = 0; sec < seconds; ++sec) {
        double peak = 0, sum = 0;
        size_t n = 0;
        const DWORD end = GetTickCount() + 1000;
        while (GetTickCount() < end) {
            Sleep(10);
            UINT32 pkt = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&pkt)) && pkt) {
                BYTE* data;
                UINT32 frames;
                DWORD flags;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                for (UINT32 i = 0; i < frames * static_cast<UINT32>(ch); ++i) {
                    double v = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                        v = is_float ? reinterpret_cast<float*>(data)[i] : reinterpret_cast<int16_t*>(data)[i] / 32768.0;
                    peak = std::fmax(peak, std::fabs(v));
                    sum += v * v;
                    ++n;
                    if (wav && (i % ch) < 2) out.push_back(static_cast<int16_t>(std::fmax(-1.0, std::fmin(1.0, v)) * 32767));
                }
                cap->ReleaseBuffer(frames);
            }
        }
        const double rms = n ? std::sqrt(sum / n) : 0;
        const auto db = [](double x) { return x > 0 ? 20 * std::log10(x) : -999.0; };
        std::printf("t=%2ds peak %6.1f dBFS  rms %6.1f dBFS\n", sec + 1, db(peak), db(rms));
        std::fflush(stdout);
        total_peak = std::fmax(total_peak, peak);
        if (peak > 0.01) ++loud_seconds;
    }
    ac->Stop();
    std::printf("SUMMARY loud_seconds=%d/%d peak_max=%.4f\n", loud_seconds, seconds, total_peak);
    if (wav && !out.empty()) {
        FILE* f = std::fopen(wav, "wb");
        if (f) {
            const uint32_t rate = fmt->nSamplesPerSec, bytes = static_cast<uint32_t>(out.size() * 2);
            const uint16_t chans = 2, bits = 16, align = 4, pcm = 1;
            const uint32_t byte_rate = rate * align, fmt_size = 16, riff = 36 + bytes;
            std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
            std::fwrite(&fmt_size, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&chans, 2, 1, f);
            std::fwrite(&rate, 4, 1, f); std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&align, 2, 1, f);
            std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
            std::fwrite(out.data(), 2, out.size(), f);
            std::fclose(f);
        }
    }
    return 0;
}
