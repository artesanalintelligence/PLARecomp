/* frame_dt_probe: checks the unlocked FPS patch at run time (recomp/patches/unlocked-fps).
 *
 * The main loop calls vf1 of the clock manager (main+0x2783048, patched to the code cave) with
 * (i, mode); with i = 0 and mode = 1 the cave measures the frame. On that call g.frame_ns (main+0x4350F08)
 * still holds the PREVIOUS frame, so it is compared with the wall time between two consecutive
 * calls with i = 0. Every `report_seconds` it logs:
 *   frames/s, average and maximum game dt, measured wall time and the ratio game_time / wall_time
 *   (1.00 = the simulation runs at real speed; < 1 = it slows down, e.g. because of the 1/15 s cap). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "pla_mod_api.h"

static const pla_api* api;
static uint64_t g_main;
static LARGE_INTEGER g_freq;
static double g_report_s = 5.0;

static int64_t g_last_qpc;     /* QPC of the previous call with i = 0 */
static double g_wall_ns;       /* sum of wall time between calls */
static double g_game_ns;       /* sum of the game frame_ns */
static double g_max_game_ns, g_max_wall_ns;
static uint64_t g_frames, g_clamped;
static int64_t g_window_qpc;

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config) {
    const char* p;
    api = a;
    g_main = api->module_base("main");
    QueryPerformanceFrequency(&g_freq);
    p = config ? strstr(config, "\"report_seconds\"") : NULL;
    if (p && (p = strchr(p, ':'))) g_report_s = atof(p + 1);
    return 0;
}

PLA_MOD_EXPORT pla_action clock_set_mode_pre(pla_ctx* ctx) {
    LARGE_INTEGER now;
    uint64_t frame_ns = 0;
    double wall_ns;
    if ((uint32_t)api->get_x(ctx, 1) != 0 || (uint32_t)api->get_x(ctx, 2) != 1) return PLA_CONTINUE;
    QueryPerformanceCounter(&now);
    if (g_last_qpc == 0) {
        g_last_qpc = g_window_qpc = now.QuadPart;
        return PLA_CONTINUE;
    }
    api->read_mem(ctx, g_main + 0x4350F08, &frame_ns, 8);
    wall_ns = (double)(now.QuadPart - g_last_qpc) * 1e9 / (double)g_freq.QuadPart;
    g_last_qpc = now.QuadPart;
    g_wall_ns += wall_ns;
    g_game_ns += (double)frame_ns;
    if ((double)frame_ns > g_max_game_ns) g_max_game_ns = (double)frame_ns;
    if (wall_ns > g_max_wall_ns) g_max_wall_ns = wall_ns;
    if (frame_ns >= 66666667ULL) ++g_clamped;
    ++g_frames;
    if ((double)(now.QuadPart - g_window_qpc) >= g_report_s * (double)g_freq.QuadPart) {
        char line[256];
        snprintf(line, sizeof line,
                 "DT frames=%llu fps=%.1f dt_avg=%.2fms dt_max=%.2fms real_avg=%.2fms real_max=%.2fms "
                 "game/real=%.4f clamped=%llu",
                 (unsigned long long)g_frames, g_frames * 1e9 / g_wall_ns, g_game_ns / g_frames / 1e6,
                 g_max_game_ns / 1e6, g_wall_ns / g_frames / 1e6, g_max_wall_ns / 1e6, g_game_ns / g_wall_ns,
                 (unsigned long long)g_clamped);
        api->log(PLA_LOG_INFO, line);
        g_wall_ns = g_game_ns = g_max_game_ns = g_max_wall_ns = 0;
        g_frames = g_clamped = 0;
        g_window_qpc = now.QuadPart;
    }
    return PLA_CONTINUE;
}
