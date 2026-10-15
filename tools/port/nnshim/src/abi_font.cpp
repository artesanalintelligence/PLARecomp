// nnshim: adapters for nn::pl (shared fonts, 4 imports from main).
//
// RequestSharedFontLoad / GetSharedFontLoadState: always native.
// GetSharedFontAddress / GetSharedFontSize: the SDK (sdk+0x359a20 / +0x359bc0, own
// disassembly) returns `shared memory base + GetSharedMemoryAddressOffset(type)` and
// `GetSize(type)` from the pl:u service. The shim serves them if it has its own fonts:
//
//   "font": { "dir": "D:/PLARecomp/fonts" }
//
// In that folder, for each of the 7 system fonts (kSlots), the user's firmware .bfttf
// or a .ttf with the same name (tools/port/nnshim/extract_suyu_fonts.py extracts the free
// replacements bundled with suyu). At startup, a host thread allocates guest memory with
// pla_api 1.2 (`guest_alloc`, which cannot be called from a hook) and copies there the same
// image suyu's HLE builds (IPlatformServiceManager): per font, header
// {0x18029a7f, size ^ key} and the plaintext TrueType data. While loading,
// GetSharedFontLoadState returns Loading (0). With no folder, no fonts or no `guest_alloc`
// (runtime older than 1.2), Address/Size stay in the HLE as in v1.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "abi_common.hpp"
#include "nnshim/services.hpp"

using namespace nnshim;

namespace nnshim::font {
namespace {

constexpr uint32_t kExpectedResult = 0x7f9a0218;   // first 4 decrypted bytes (big endian)
constexpr uint32_t kExpectedMagic = 0x36f81a1e;    // first 4 bytes of the .bfttf (big endian)

// HLE region order (SHARED_FONTS in platform_service_manager.h): the index is the
// SharedFontType (0 Standard, 1 ChineseSimplified, 2 ExtChineseSimplified, 3 ChineseTraditional,
// 4 KoreanHangul, 5 NintendoExtended) as long as none is missing.
constexpr const char* kSlots[7] = {
    "nintendo_udsg-r_std_003",       "nintendo_udsg-r_org_zh-cn_003", "nintendo_udsg-r_ext_zh-cn_003",
    "nintendo_udjxh-db_zh-tw_003",   "nintendo_udsg-r_ko_003",        "nintendo_ext_003",
    "nintendo_ext2_003",
};

uint32_t Swap32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

bool ReadAll(const std::filesystem::path& p, std::vector<uint8_t>& out) {
    out.clear();
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n > 0) {
        out.resize(static_cast<size_t>(n));
        if (std::fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
    }
    std::fclose(f);
    return !out.empty();
}

void PutU32(std::vector<uint8_t>& img, uint32_t v) {
    for (int i = 0; i < 4; ++i) img.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

} // namespace

bool BuildSharedFontImage(const std::string& dir, std::vector<uint8_t>& image, std::vector<Region>& regions,
                          std::string* log) {
    namespace fs = std::filesystem;
    image.clear();
    regions.clear();
    const fs::path base = fs::u8path(dir);
    for (const char* slot : kSlots) {
        std::vector<uint8_t> data;
        bool bfttf = ReadAll(base / (std::string(slot) + ".bfttf"), data);
        if (!bfttf && !ReadAll(base / (std::string(slot) + ".ttf"), data)) {
            if (log) *log += std::string(" -") + slot;
            continue;
        }
        const size_t words = data.size() / 4;   // the HLE drops the remainder (< 4 bytes)
        uint32_t key = kExpectedResult ^ kExpectedMagic;
        const uint32_t start = static_cast<uint32_t>(image.size());
        if (bfttf) {
            // HLE's DecryptSharedFont: key = first word (big endian) ^ kExpectedResult.
            if (words < 2) continue;
            uint32_t w0 = 0;
            std::memcpy(&w0, data.data(), 4);
            key = Swap32(w0) ^ kExpectedResult;
            if (Swap32(w0) != kExpectedMagic && log) *log += std::string(" (") + slot + ": unexpected magic)";
            PutU32(image, Swap32(kExpectedResult));
            uint32_t w1 = 0;
            std::memcpy(&w1, data.data() + 4, 4);
            PutU32(image, Swap32(w1));   // firmware size ^ key (the HLE leaves it unchanged)
            for (size_t i = 2; i < words; ++i) {
                uint32_t w = 0;
                std::memcpy(&w, data.data() + 4 * i, 4);
                PutU32(image, Swap32(Swap32(w) ^ key));
            }
            regions.push_back({start + 8, static_cast<uint32_t>(words * 4 - 8)});
        } else {
            // Plaintext TTF: EncryptSharedFont (when packing) + DecryptSharedFont (when mapping)
            // cancel out except for the header.
            PutU32(image, Swap32(kExpectedResult));
            PutU32(image, static_cast<uint32_t>(words * 4) ^ key);
            image.insert(image.end(), data.begin(), data.begin() + static_cast<ptrdiff_t>(words * 4));
            regions.push_back({start + 8, static_cast<uint32_t>(words * 4)});
        }
        if (log) *log += std::string(" +") + slot + (bfttf ? ".bfttf" : ".ttf");
    }
    return !regions.empty();
}

} // namespace nnshim::font

namespace {

// 0 = no own fonts (HLE), 1 = loading, 2 = ready, 3 = failed (HLE).
std::atomic<int> g_state{0};
std::atomic<bool> g_stop{false};
uint64_t g_base = 0;   // written before g_state = 2
std::vector<font::Region> g_regions;
ExitSafeThread g_loader;

void Loader(std::vector<uint8_t> image) {
    void* host = nullptr;
    uint64_t va = 0;
    // The game process may not exist yet when the mod loads: retry for up to 60 s.
    for (int i = 0; i < 1200 && !g_stop.load(); ++i) {
        va = g_api->guest_alloc(image.size(), &host);
        if (va) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!va || !host) {
        Log(PLA_LOG_WARNING, "nnshim: fonts: guest_alloc failed; staying in the HLE");
        g_state.store(3);
        return;
    }
    std::memcpy(host, image.data(), image.size());
    g_base = va;
    g_state.store(2, std::memory_order_release);
    Log(PLA_LOG_INFO, "nnshim: native fonts at 0x%llx (%zu bytes, %zu fonts)", static_cast<unsigned long long>(va),
        image.size(), g_regions.size());
}

void InitFont(const cfg::Value& c) {
    const std::string dir = c.GetString("font.dir", c.GetString("pl.font_dir", ""));
    if (dir.empty() || FamilyMode(Family::Font) != Mode::Native) return;
    if (!PLA_API_HAS(g_api, guest_free) || !g_api->guest_alloc) {
        Log(PLA_LOG_INFO, "nnshim: fonts: runtime lacks guest_alloc (pla_api 1.2); staying in the HLE");
        return;
    }
    std::vector<uint8_t> image;
    std::string log;
    if (!font::BuildSharedFontImage(dir, image, g_regions, &log)) {
        Log(PLA_LOG_WARNING, "nnshim: fonts: none in %s; staying in the HLE", dir.c_str());
        return;
    }
    Log(PLA_LOG_INFO, "nnshim: fonts from %s:%s", dir.c_str(), log.c_str());
    g_stop.store(false);
    g_state.store(1);
    g_loader = std::thread(Loader, std::move(image));
}

void ShutdownFont() {
    g_stop.store(true);
    if (g_loader.joinable()) g_loader.join();
}

ModuleHooks g_font_module{"font", &InitFont, &ShutdownFont};

} // namespace

// void RequestSharedFontLoad(SharedFontType)
NNSHIM_ADAPTER(Font, nnshim_pl_RequestSharedFontLoad) { return PLA_SKIP; }

// SharedFontLoadState GetSharedFontLoadState(SharedFontType)   (0 = Loading, 1 = Loaded)
NNSHIM_ADAPTER(Font, nnshim_pl_GetSharedFontLoadState) {
    PlaGuest g(ctx);
    g.ret(g_state.load(std::memory_order_acquire) == 1 ? 0 : 1);
    return PLA_SKIP;
}

// const void* GetSharedFontAddress(SharedFontType)
NNSHIM_ADAPTER(Font, nnshim_pl_GetSharedFontAddress) {
    if (g_state.load(std::memory_order_acquire) != 2) return PLA_CONTINUE;
    PlaGuest g(ctx);
    const uint32_t type = static_cast<uint32_t>(g.x(0));
    // Type with no region: the HLE returns the empty region {0, 0} -> base + 0.
    g.ret(g_base + (type < g_regions.size() ? g_regions[type].offset : 0));
    return PLA_SKIP;
}

// size_t GetSharedFontSize(SharedFontType)
NNSHIM_ADAPTER(Font, nnshim_pl_GetSharedFontSize) {
    if (g_state.load(std::memory_order_acquire) != 2) return PLA_CONTINUE;
    PlaGuest g(ctx);
    const uint32_t type = static_cast<uint32_t>(g.x(0));
    g.ret(type < g_regions.size() ? g_regions[type].size : 0);
    return PLA_SKIP;
}
