// nnshim: adapters for nn::swkbd (11 imports). The nn::pl ones are in abi_font.cpp.
//
// ShowKeyboard (keyboard applet) -> native text input:
//   "swkbd.mode": "dialog" (default, Win32 window with the window title, the header
//   text and the length limit), "auto" (returns "swkbd.auto_text" immediately, for
//   automated tests) or "cancel" (as if the user cancelled).
// ShowKeyboardArg layout (libnx SwkbdConfigCommon, verified in the SDK):
//   +0x24 headerText u16[0x41], +0xA6 subText u16[0x81], +0x1A8 guideText u16[0x101],
//   +0x3AC stringLenMax, +0x3B0 stringLenMin, +0x3B4 passwordFlag,
//   +0x3C0 initialStringOffset (bytes into the work buffer), +0x3C4 initialStringSize (u16),
//   +0x4D0 pointer to the work buffer. String = { void* ptr; size_t size } (UTF-16).
// Functions that only fill KeyboardConfig (MakePreset, Set*Text, SetUserWordList,
// SetTextCheckCallback) are SDK user code and stay recompiled (PASS).
#include <algorithm>
#include <string>

#include "abi_common.hpp"
#include "nnshim/services.hpp"

using namespace nnshim;

namespace {

constexpr uint32_t kResultCanceled = (1u << 9) | 159;   // 2159-0001 (swkbd: cancelled)

struct SwkbdConfig {
    std::string mode = "dialog";
    std::string auto_text = "Ari";
} g_sk;

void InitSwkbd(const cfg::Value& c) {
    g_sk.mode = c.GetString("swkbd.mode", "dialog");
    g_sk.auto_text = c.GetString("swkbd.auto_text", "Ari");
}

ModuleHooks g_swkbd_module{"swkbd", &InitSwkbd, nullptr};

} // namespace

// Result ShowKeyboard(String* out, const ShowKeyboardArg& arg)
NNSHIM_ADAPTER(Swkbd, nnshim_swkbd_ShowKeyboard) {
    PlaGuest g(ctx);
    const uint64_t out = g.x(0), arg = g.x(1);
    swkbd::Request r;
    LoadU16String(g, arg + 0x24, r.header, 0x40);
    LoadU16String(g, arg + 0xA6, r.sub, 0x80);
    LoadU16String(g, arg + 0x1A8, r.guide, 0x100);
    uint32_t max_len = 0, min_len = 0, pass = 0, init_off = 0, init_len = 0;
    uint64_t work = 0;
    g.load(arg + 0x3AC, max_len);
    g.load(arg + 0x3B0, min_len);
    g.load(arg + 0x3B4, pass);
    g.load(arg + 0x3C0, init_off);
    g.load(arg + 0x3C4, init_len);
    g.load(arg + 0x4D0, work);
    r.max_length = max_len ? static_cast<int>(std::min<uint32_t>(max_len, 500)) : 500;
    r.min_length = static_cast<int>(std::min<uint32_t>(min_len, 500));
    r.password = pass != 0;
    if (PLA_API_HAS(g_api, window_handle)) r.owner = g_api->window_handle();
    if (work && init_len && init_len <= 500) {
        std::u16string s(init_len, u'\0');
        g.read(work + init_off, s.data(), init_len * 2);
        r.initial = s;
    }
    std::u16string text;
    bool ok;
    if (g_sk.mode == "auto") {
        text = Utf8ToUtf16(g_sk.auto_text);
        if (text.empty()) text = r.initial;
        ok = true;
    } else if (g_sk.mode == "cancel") {
        ok = false;
    } else {
        ok = swkbd::Prompt(r, text);
    }
    if (static_cast<int>(text.size()) > r.max_length) text.resize(static_cast<size_t>(r.max_length));
    Log(PLA_LOG_INFO, "nnshim: swkbd \"%s\" -> %s \"%s\"", Utf16ToUtf8(r.header).c_str(), ok ? "accepted" : "cancelled",
        Utf16ToUtf8(text).c_str());
    Trace("swkbd %s", ok ? Utf16ToUtf8(text).c_str() : "(cancelled)");
    if (!ok) {
        g.ret(kResultCanceled);
        return PLA_SKIP;
    }
    uint64_t str[2] = {};   // { ptr, size }
    g.read(out, str, sizeof str);
    if (str[0] && str[1] >= 2) {
        const size_t cap = static_cast<size_t>(str[1] / 2) - 1;
        if (text.size() > cap) text.resize(cap);
        text.push_back(u'\0');
        g.write(str[0], text.data(), text.size() * 2);
    }
    g.ret(0);
    return PLA_SKIP;
}

// size_t GetRequiredWorkBufferSize(bool useDictionary)
NNSHIM_ADAPTER(Swkbd, nnshim_swkbd_GetRequiredWorkBufferSizeBool) {
    PlaGuest g(ctx);
    g.ret((g.w(0) & 1) ? 0xD000 : 0x1000);
    return PLA_SKIP;
}

// size_t GetRequiredWorkBufferSize(int userWordCount)   (0 if there are more than 5000 words)
NNSHIM_ADAPTER(Swkbd, nnshim_swkbd_GetRequiredWorkBufferSizeInt) {
    PlaGuest g(ctx);
    const int32_t n = static_cast<int32_t>(g.w(0));
    uint64_t size = 0x1000;
    if (n > 5000) size = 0;
    else if (n != 0) size = (static_cast<uint64_t>(static_cast<int64_t>(n) * 100 + 0x17D3)) & ~0xFFFull;
    g.ret(size);
    return PLA_SKIP;
}

// size_t GetRequiredStringBufferSize()  /  GetRequiredTextCheckWorkBufferSize()
NNSHIM_ADAPTER(Swkbd, nnshim_swkbd_GetRequiredStringBufferSize) {
    PlaGuest g(ctx);
    g.ret(0x7D4);
    return PLA_SKIP;
}
NNSHIM_ADAPTER(Swkbd, nnshim_swkbd_GetRequiredTextCheckWorkBufferSize) {
    PlaGuest g(ctx);
    g.ret(0x7D4);
    return PLA_SKIP;
}
