/* nnshim: exported replacement table (C API). */
#ifndef NNSHIM_TABLE_H
#define NNSHIM_TABLE_H

#include <stddef.h>
#include <stdint.h>

#include "pla_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nnshim_entry {
    const char* mangled;        /* SDK symbol being replaced */
    pla_pre_fn adapter;         /* pre hook: PLA_SKIP = handled; PLA_CONTINUE = to the original */
    uint64_t main_plt_offset;   /* PLT stub in main (offset from main's base), or 0 */
    const char* sdk_module;     /* "sdk", "subsdk0", "subsdk1" or "none" */
    uint64_t sdk_offset;        /* definition in that module, or 0 */
    int hook_sdk;               /* 1: hook sdk_module+sdk_offset; 0: main+main_plt_offset */
    const char* family;         /* inventory family: hid, fs, time, settings, applet, ... */
} nnshim_entry;

/* NATIVE entries of shim_table.inc already resolved to addresses (no duplicates). */
PLA_MOD_EXPORT const nnshim_entry* nnshim_get_table(size_t* count);

/* Shim statistics as JSON (families, per-adapter counters). */
PLA_MOD_EXPORT const char* nnshim_stats_json(void);

#ifdef __cplusplus
}
#endif

#endif /* NNSHIM_TABLE_H */
