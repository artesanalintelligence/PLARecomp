/* pla_api_selftest: checks the pla_api 1.1 extension inside the game.
 *
 * On the first call to GetNpadStyleSet (a game CPU thread):
 *   - creates an event, gets a game handle for its readable end and
 *     imports it back with event_from_guest_handle;
 *   - a host thread waits on the event (event_wait) while the hook signals it;
 *   - checks the timeout after event_clear and the signal through the imported id,
 *   - compares host_pointer/host_backing_pointer with read_mem over the thread stack,
 *   - checks that guest_ticks advances;
 *   - 1.2 (if present): guest_alloc of 1 MiB from a host thread, that host_pointer and
 *     host_backing_pointer see the same bytes, guest_free, and frame_presented.
 * Writes "SELFTEST ok=N fail=M" to the suyu log. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "pla_mod_api.h"

static const pla_api* api;
static volatile LONG started;
static uint64_t ev_main, ev_imported;
static volatile LONG wait_result = -2, wait2_result = -2, wait3_result = -2;
static int ok, fail;

static void check(int cond, const char* what) {
    char msg[200];
    if (cond) ok++; else fail++;
    snprintf(msg, sizeof msg, "%s: %s", cond ? "OK  " : "FAIL", what);
    api->log(cond ? PLA_LOG_INFO : PLA_LOG_ERROR, msg);
}

static DWORD WINAPI waiter(LPVOID unused) {
    (void)unused;
    /* 1: signaled by the hook after creating this thread */
    wait_result = api->event_wait(ev_main, 2000000000LL);
    api->event_clear(ev_main);
    Sleep(20);
    /* 2: nobody signals it -> timeout */
    wait2_result = api->event_wait(ev_main, 30000000LL);
    /* 3: signal through the id imported from the game handle */
    api->event_signal(ev_imported);
    wait3_result = api->event_wait(ev_main, 2000000000LL);
    check(wait_result == 1, "event_wait sees the signal from the hook (host thread)");
    check(wait2_result == 0, "event_wait times out after event_clear");
    check(wait3_result == 1, "signal through event_from_guest_handle reaches the same KEvent");
    if (PLA_API_HAS(api, guest_free)) {
        void* host = NULL;
        const size_t size = 1u << 20;
        uint64_t va = api->guest_alloc(size, &host);
        char msg[160];
        snprintf(msg, sizeof msg, "guest_alloc: va=%#llx host=%p", (unsigned long long)va, host);
        api->log(PLA_LOG_INFO, msg);
        check(va != 0 && host != NULL, "guest_alloc (1 MiB) with host_pointer");
        if (va && host) {
            unsigned char* h = (unsigned char*)host;
            unsigned char* b;
            size_t i;
            int zero = 1, same = 1;
            for (i = 0; i < size; i++) zero &= h[i] == 0;
            check(zero, "guest_alloc returns zeroed memory");
            for (i = 0; i < size; i++) h[i] = (unsigned char)(i * 7 + 3);
            b = (unsigned char*)api->host_backing_pointer(va + 0x1000, 0x1000);
            if (b) for (i = 0; i < 0x1000; i++) same &= b[i] == (unsigned char)((i + 0x1000) * 7 + 3);
            check(b != NULL && same, "host_backing_pointer sees what was written through host_pointer");
            check(api->host_pointer(va, size) == host, "host_pointer(va) == host");
            check(api->guest_free(va, size) == 0, "guest_free");
            check(api->host_pointer(va, size) == NULL, "after guest_free, host_pointer -> NULL");
        }
        api->frame_presented();
        check(1, "frame_presented");
    }
    {
        char msg[96];
        snprintf(msg, sizeof msg, "SELFTEST ok=%d fail=%d", ok, fail);
        api->log(fail ? PLA_LOG_ERROR : PLA_LOG_INFO, msg);
    }
    api->event_destroy(ev_imported);
    api->event_destroy(ev_main);
    return 0;
}

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config_json) {
    (void)config_json;
    api = a;
    if (!PLA_API_HAS(api, guest_ticks)) {
        api->log(PLA_LOG_ERROR, "the runtime does not have the pla_api 1.1 extension");
        return 1;
    }
    return 0;
}

PLA_MOD_EXPORT pla_action style_set_pre(pla_ctx* ctx) {
    if (InterlockedExchange(&started, 1) != 0) return PLA_CONTINUE;
    {
        uint64_t t0 = api->guest_ticks();
        uint32_t handle;
        uint64_t sp = api->get_sp(ctx);
        unsigned char via_read[256], *hp, *bp;
        char msg[200];

        ev_main = api->event_create("pla_api_selftest");
        check(ev_main != 0, "event_create");
        handle = api->event_guest_handle(ev_main);
        snprintf(msg, sizeof msg, "game handle = %#x", handle);
        api->log(PLA_LOG_INFO, msg);
        check(handle != 0, "event_guest_handle");
        ev_imported = api->event_from_guest_handle(handle);
        check(ev_imported != 0 && ev_imported != ev_main, "event_from_guest_handle");
        check(api->event_from_guest_handle(0xdeadbeefu) == 0, "invalid handle -> 0");

        api->read_mem(ctx, sp, via_read, sizeof via_read);
        hp = (unsigned char*)api->host_pointer(sp, sizeof via_read);
        bp = (unsigned char*)api->host_backing_pointer(sp, sizeof via_read);
        snprintf(msg, sizeof msg, "sp=%#llx host_pointer=%p host_backing_pointer=%p window=%p",
                 (unsigned long long)sp, (void*)hp, (void*)bp, api->window_handle());
        api->log(PLA_LOG_INFO, msg);
        check(hp && memcmp(hp, via_read, sizeof via_read) == 0, "host_pointer == read_mem");
        check(bp && memcmp(bp, via_read, sizeof via_read) == 0, "host_backing_pointer == read_mem");
        check(api->host_pointer(0x10, 16) == NULL, "host_pointer of unmapped memory -> NULL");
        check(api->window_handle() != NULL, "window_handle");

        CloseHandle(CreateThread(NULL, 0, waiter, NULL, 0, NULL));
        Sleep(5);
        check(api->event_signal(ev_main) == 0, "event_signal from a hook");
        check(api->guest_ticks() > t0, "guest_ticks advances");
    }
    return PLA_CONTINUE;
}
