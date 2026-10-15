// nnshim: network/shop services the game queries while offline (v1.5 stage 5: keep them out of
// HLE IPC). suyu's HLE already answers "no network": the shim gives the same answers natively.
//   aoc     CountAddOnContent / ListAddOnContent   -> 0 add-on contents (the game has no DLC)
//   nifm    IsNetworkAvailable                     -> false
//   friends GetFriendList                          -> 0 friends, Result 0
//   prepo   PlayReport::Save                       -> Result 0 (the report is discarded)
// With gen_mod_json.py --validate, in observe mode each one compares its answer with the HLE's.
#include "abi_common.hpp"
#include "validate.hpp"

using namespace nnshim;

// int aoc::CountAddOnContent()
NNSHIM_ADAPTER(Net, nnshim_aoc_CountAddOnContent) {
    PlaGuest g(ctx);
    g.ret(0);
    return PLA_SKIP;
}
NNSHIM_EXPORT void nnshim_aoc_CountAddOnContent_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    ValidateResult("aoc.CountAddOnContent", g.w(0) == 0, Fmt("%d", static_cast<int>(g.w(0))), "0");
}

// int aoc::ListAddOnContent(int* out, int offset, int count)
NNSHIM_ADAPTER(Net, nnshim_aoc_ListAddOnContent) {
    PlaGuest g(ctx);
    g.ret(0);
    return PLA_SKIP;
}
NNSHIM_EXPORT void nnshim_aoc_ListAddOnContent_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    ValidateResult("aoc.ListAddOnContent", g.w(0) == 0, Fmt("%d", static_cast<int>(g.w(0))), "0");
}

// bool nifm::IsNetworkAvailable()
NNSHIM_ADAPTER(Net, nnshim_nifm_IsNetworkAvailable) {
    PlaGuest g(ctx);
    g.ret(0);
    return PLA_SKIP;
}
NNSHIM_EXPORT void nnshim_nifm_IsNetworkAvailable_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    ValidateResult("nifm.IsNetworkAvailable", (g.w(0) & 0xff) == 0, Fmt("%u", g.w(0) & 0xff), "0");
}

// Result friends::GetFriendList(int* outCount, Friend* out, int offset, int count, const FriendFilter&)
NNSHIM_ADAPTER(Net, nnshim_friends_GetFriendList) {
    PlaGuest g(ctx);
    if (g.x(0)) g.store<int32_t>(g.x(0), 0);
    g.ret(0);
    return PLA_SKIP;
}
NNSHIM_OBSERVER(nnshim_friends_GetFriendList) { g_api->call_data(ctx)[0] = g_api->get_x(ctx, 0); }
NNSHIM_EXPORT void nnshim_friends_GetFriendList_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    int32_t n = -1;
    if (g.call_data()[0]) g.load(g.call_data()[0], n);
    ValidateResult("friends.GetFriendList", g.w(0) == 0 && n == 0, Fmt("r=%#x n=%d", g.w(0), n), "r=0 n=0");
}

// Result prepo::PlayReport::Save(const account::Uid&)
NNSHIM_ADAPTER(Net, nnshim_prepo_PlayReportSave) {
    PlaGuest g(ctx);
    CounterAdd("prepo.dropped");
    g.ret(0);
    return PLA_SKIP;
}
NNSHIM_EXPORT void nnshim_prepo_PlayReportSave_post(pla_ctx* ctx) {
    PlaGuest g(ctx);
    ValidateResult("prepo.PlayReportSave", g.w(0) == 0, Fmt("r=%#x", g.w(0)), "r=0");
}
