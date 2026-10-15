# Native mods for recompiled PLA 1.1.1

A mod is a **host DLL (x86-64)** plus a **`mod.json`** that maps game functions
(module + offset) to functions exported by the DLL. The suyu fork's runtime
(`third_party/suyu/src/core/arm/recomp/recomp_mods.cpp`, branch `pla-mods`) loads them when
the game starts with the recompiled DLLs and redirects those functions to the mod's code.

**There is no need to re-export or recompile `recompiled_image.dll` (main)** to enable,
disable or change a mod: restarting the game is enough.

The C API is in [`mods/include/pla_mod_api.h`](mods/include/pla_mod_api.h) (a copy of the
fork's `core/arm/recomp/pla_mod_api.h`; `build-mod.ps1` warns if they differ). The example
mod is [`mods/hid_input_logger/`](mods/hid_input_logger/).

## 1. How it works (and why no recompilation is needed)

In the generated code, **no block calls another directly**: every block ends by leaving the
target PC in the context and returning (`c->pc = ...; return;`), for `B`/`BL` as well as
for `BR`/`BLR`/`RET`. Then the module's local loop (`recomp_image_run_slice`) and the host
dispatcher look the PC up in the image's **flat block index**: one function pointer per
instruction word, built when the DLL is loaded (`recomp_image_set_base`), living in
writable memory and exposed through `recomp_image_index`. (The emitter has `ChainTo`, to
chain direct calls, but in v0.0.11 it is not used anywhere.)

That is why it is enough to **rewrite the index entry** for the function's first
`instruction word`: every call (`BL`, indirect calls through a vtable or pointer, `B` tail
jumps) goes through the mod. The game DLL does not change; the original pointer is saved
and is the "trampoline" used to run the original function.

Startup flow:
1. The frontend (Qt `main.cpp` or `suyu-cmd`) calls `recomp_image_set_base` and registers
   each image's index (`RecompMods::RegisterImage`).
2. On the process's first `ArmRecomp::RunThread`, with the modules already placed and
   before running any block, `RecompMods::Activate` reads the title's mods, validates
   each hook, calls `pla_mod_init` and patches the index entries.
3. When the session ends (`~ArmRecomp`), `Deactivate` restores the entries, calls
   `pla_mod_shutdown`, unloads the DLLs and logs how many times each hook was called.

### Hooks: `pre`, `post` and replacement

- **`pre`** (`pla_action fn(pla_ctx*)`) runs on entry, with the arguments in
  X0-X7 / V0-V7 (AAPCS64). It can read and change them, and returns:
  - `PLA_CONTINUE`: the original function (its AOT code) runs;
  - `PLA_SKIP`: it does not run; execution returns to the caller (X30) with whatever the mod
    left in X0/V0. This is a **full replacement** (equivalent to `RECOMP_PATCH`);
  - `PLA_REDIRECT`: execution continues at the PC the mod set with `set_pc` (X30 untouched, so
    a game function jumped to returns to the original caller).
- **`post`** (`void fn(pla_ctx*)`) runs when the original returns, with the result
  in X0/V0 (equivalent to `RECOMP_HOOK_RETURN`). To achieve this, the runtime replaces X30 with
  a trap address (`0xFFFFFFFE00000000`) and saves the real address on a per-guest-thread
  shadow stack (key: `TPIDRRO_EL0`). When the function does `RET` to the trap,
  `ArmRecomp`'s dispatch loop runs the `post` hooks and continues at the real address.
  `call_data()` gives 4 values that travel from the `pre` to the `post` of the same call
  (for example, an output pointer that was in X0 on entry).
- Several mods can hook the same address: the `pre` hooks run in folder order and the
  `post` hooks in reverse order.

**Why there is no synchronous "call the original" inside the mod**: a guest function
can stop at any instruction because of a system call (IPC with services, mutexes, waits)
that the HLE kernel handles *after* the CPU backend returns to the scheduler, and the guest
thread may even move to another core. A host stack frame of the mod does not survive that.
`pre` + `PLA_CONTINUE` + `post` is that same call, split exactly where the guest runs.

### Known limits

- Only addresses that have an AOT block (what the export translated) can be hooked.
  If the address has no block, the mod is not loaded and the log says so.
- Only code that runs recompiled goes through the hook. If the fallback JIT
  (Hybrid mode) runs the caller, the call goes straight to the original. With PLA's current
  export (SHL translated, 0 lookup misses) the JIT is not used in the menu.
- A hook with `post` must not return through the JIT (the JIT does not know the trap
  address). Same reason as the previous point.
- With `post`, game code that reads its own X30 (e.g. `__builtin_return_address`
  for debugging) sees the trap address. C++ exceptions crossing a function with `post`
  could not unwind that frame; `longjmp` is handled (abandoned frames are discarded by SP).
- An entry reached by falling through from the previous instruction inside the same
  block (without going through the dispatcher) does not go through the hook. It does not
  happen in normal functions: the previous block ends in `RET`/`B`/`BL`.
- The statically linked "Build" launcher (`suyu-cmd-static`): since commit
  `a05ec9d6e8` of `pla-mods`, the generated registration (`recomp_registration.c`) exports
  `suyu_recomp_static_index_v1` with each module's block index and the launcher
  registers the images as with the DLLs, so mods also load there. You need to
  **re-export** for the registration to include it (old registrations still have no mods).
  It compiles, but has not been tested with a full static build of PLA (about 45 min).
- Do not write to guest code with `write_mem`: the code guard would send those blocks to the
  JIT (Hybrid) or stop the thread (strict).

## 2. Writing a mod

```c
#include "pla_mod_api.h"

static const pla_api* api;

PLA_MOD_EXPORT uint32_t pla_mod_api_version(void) { return PLA_MOD_API_VERSION; }  /* required */

PLA_MOD_EXPORT int pla_mod_init(const pla_api* a, const char* config_json) {        /* optional */
    api = a;                       /* config_json = the "config" object of mod.json */
    return 0;                      /* != 0: the mod is not loaded */
}

/* pre: X0 = first argument */
PLA_MOD_EXPORT pla_action my_pre(pla_ctx* ctx) {
    api->call_data(ctx)[0] = api->get_x(ctx, 0);
    return PLA_CONTINUE;
}

/* post: X0 = return value */
PLA_MOD_EXPORT void my_post(pla_ctx* ctx) {
    char msg[96];
    snprintf(msg, sizeof msg, "arg %#llx -> %#llx",
             (unsigned long long)api->call_data(ctx)[0], (unsigned long long)api->get_x(ctx, 0));
    api->log(PLA_LOG_INFO, msg);
}
```

API (`pla_api`): `get_x/set_x` (X0-X30), `get_sp/set_sp`, `get_pc/set_pc`, `get_v/set_v`
(V0-V31 as two 64-bit halves; helpers `pla_get_s/pla_get_d/pla_set_s/pla_set_d`),
`read_mem/write_mem` (guest virtual memory, page by page; they return the bytes
copied), `module_base("main")`, `call_data` and `log`. Hooks run on the guest CPU
threads, sometimes in parallel: they must be short and thread-safe.

### `pla_api` extension 1.1: kernel and host objects

Fields added at the end of `pla_api` (the version is still 1; a mod checks that they
exist with `PLA_API_HAS(api, field)`, which looks at `api->size`). Implementation:
the fork's `core/arm/recomp/recomp_native.cpp`.

| Field | What it does |
|---|---|
| `uint64_t event_create(const char* name)` | creates an HLE kernel `KEvent` (owned by a runtime process, like those of an HLE service). Returns an opaque id (0 = error) |
| `uint32_t event_guest_handle(uint64_t ev)` | adds the readable end to the game's handle table and returns the handle: this is what a replaced SDK function hands the game for `os::AttachReadableHandleToSystemEvent` / `os::SystemEvent`. The game owns the handle and closes it |
| `uint64_t event_from_guest_handle(uint32_t h)` | imports an event the game already has (readable or writable handle) to signal it or wait on it from the host |
| `int event_signal(ev)`, `int event_clear(ev)` | 0 = queued (applied in order, within microseconds); -1 = unknown id |
| `int event_wait(ev, int64_t timeout_ns)` | **host threads only** (never from a hook: it would stall a guest core). 1 = signaled, 0 = timeout, -1 = error. Does not clear it |
| `void event_destroy(ev)` | releases the runtime's reference (the game's handles remain valid) |
| `void* host_pointer(va, size)` | host pointer to the guest range (fastmem arena: any mapped range is contiguous) or NULL. Reading/writing there is the same as `read_mem`/`write_mem`, without copies |
| `void* host_backing_pointer(va, size)` | pointer to the physical backing memory (a single host reservation; contiguous only where the physical pages are). Useful to import memory (e.g. `VK_EXT_external_memory_host`) |
| `void* window_handle(void)` | the game window (HWND), for a native renderer |
| `uint64_t guest_ticks(void)` | the guest's `CNTPCT_EL0` (19.2 MHz), what `os::GetSystemTick` sees |

Why there is an intermediate thread: a hook runs inside a guest core's `Run()`; a
kernel call from there may reschedule and switch fibers in the middle of an AOT block, and
`ArmRecomp`'s per-core state does not support that. All kernel work is done by a host
thread registered in the kernel (a "dummy" `KThread`, like the HLE services); the functions
that return a value wait for it (microseconds). `event_signal` works the same from a hook as
from the host's audio callback.

Tested in the game with the `recomp/mods/pla_api_selftest` mod (13/13: create, guest handle,
import, signal from a hook seen by `event_wait` on a host thread, timeout after
`event_clear`, signal through the imported id, `host_pointer`/`host_backing_pointer` equal to
`read_mem` over the thread's stack, NULL outside the map, window, `guest_ticks`).

### `pla_api` extension 1.2 (2026-10-04, fork `pla-mods` 81b8750a17 and later)

| Field | What it does |
|---|---|
| `void frame_presented(void)` | for a native renderer that presents on its own (and skips the `vi`/nvnflinger queue): one call per presented frame. Feeds the FPS counter (window title, `PERF` samples, the loading splash, which closes on the first frame) |
| `uint64_t guest_alloc(size_t size, void** host)` | maps `size` bytes (rounded to 4 KiB) of new, zeroed, read/write memory in the game's address space (stack region, like the main thread's stack; outside the heap) and returns the guest address, or 0. `host` (optional) receives the range's `host_pointer`. For data the game reads through a guest pointer (shared fonts...). Not from a hook |
| `int guest_free(uint64_t va, size_t size)` | unmaps what `guest_alloc` returned (0 = ok) |

Tested with `pla_api_selftest` (20/20 with 1.2: a 1 MiB `guest_alloc`, `host_pointer` and
`host_backing_pointer` seeing the same bytes, `guest_free` and `frame_presented`).

### `mod.json`

```json
{
  "id": "my_mod",
  "title_id": "01001F5010DFA000",
  "build_id": "AEE8F150DDA1B5A838806E1A5EA6827AD9F3C51E",
  "library": "my_mod.dll",
  "enabled": true,
  "hooks": [
    { "module": "main", "offset": "0x32a47a0", "pre": "my_pre", "post": "my_post",
      "expect": ["0x...", "0x..."] }
  ],
  "config": { "whatever_the_mod_wants": 1 }
}
```

- `offset` is relative to the start of the module (the same as `data/main_functions.csv`
  minus `0x7100000000`, the pchtxt files with `@flag offset_shift 0x100` and `tools/ghidra/out/`).
- `module`: `main`, `sdk`, `subsdk0`, `subsdk1` or `rtld` (name of the DLL's folder).
  Hooks in `sdk` are tested: the nnshim (`tools/port/nnshim`) hooks 26 `nn::fs`
  functions in the sdk and serves the RomFS from a host folder (see `STATUS.md`).
- `title_id` and `build_id` (optional) are checked against the process; if they do not match,
  the mod is not loaded. `expect` (optional) compares the instruction words at that
  address: it is the finest version check (they do not need to be published: they
  can be set only in the local copy).
- A mod is applied entirely or not at all: if a single entry fails (offset without a block,
  symbol not exported, different version), nothing of that mod is patched.
- To disable it: `"enabled": false`, create a `disabled` file in its folder, or
  delete the folder. `SUYU_RECOMP_MODS=0` disables all of them.

### Choosing functions

- `data/main_functions.csv` and `tools/ghidra/out/main_functions_named.tsv`: addresses and
  names (RTTI, imported `nn::` symbols). Named library functions are good entry points.
  The `nn::` PLT stubs (16 bytes at `main+0x32a4xxx`) are ideal: the whole game calls them
  with `BL` and their signature is the public SDK's.
- Watch out for the offsets in `main_functions.csv`: they are in the Ghidra base
  (`0x7100000000`); the module offset is the address minus that base.

## 3. Build and install

```powershell
# Compiles with clang-cl to D:\PLARecomp\mods-build\<id>\ and copies DLL + mod.json to
# %APPDATA%\suyu\recomp\mods\01001F5010DFA000\<id>\
.\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Install

# Changing "config" values only in the installed copy:
.\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Install `
    -Config @{ force_buttons = '0x8000'; force_after_seconds = 60 }

# Uninstall
.\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Uninstall
```

Mods folder: `<suyu user directory>\recomp\mods\<TITLEID>\<id>\`
(`%APPDATA%\suyu\...`, or `<suyu>\user\...` in a portable install). The
`SUYU_RECOMP_MODS_DIR` variable changes the root (`<root>\<TITLEID>\<id>\`).

Then start PLA in Hybrid mode as usual (`SUYU_RECOMP_DIR` with the DLLs from
`build\recomp\pla`). To test without touching other people's `%APPDATA%\suyu`:

```powershell
.\recomp\scripts\setup-portable-suyu.ps1           # D:\PLARecomp\suyu-portable (+ user\)
.\recomp\scripts\build-mod.ps1 -Mod recomp\mods\hid_input_logger -Install `
    -ModsRoot D:\PLARecomp\suyu-portable\user\recomp\mods
.\recomp\scripts\run-pla-portable.ps1 -Mode hybrid -RecompDir D:\PLARecomp\mk8-recomp\build\recomp\pla `
    -Seconds 120 -Screenshots 60,100
```

The suyu log shows `recomp mods: loaded ...`, the mod's `[mod <id>] ...` lines
and, on exit, `recomp mods: main+0x... was called N time(s)`.

## 4. The example mod: `hid_input_logger`

Hooks the PLT stubs of `nn::hid::GetNpadState` (five overloads: FullKey, Handheld,
JoyDual, JoyLeft, JoyRight, at `main+0x32a47a0..0x32a47e0`) and `nn::hid::GetNpadStyleSet`
(`main+0x32a4850`):

- `pre` saves the output pointer (X0) and the controller id pointer (X1);
- `post` reads the buttons (`+8` of the `NpadXxxState`), counts calls and every
  `report_seconds` writes a line to the log;
- with `force_buttons` (`NpadButton` mask: A=bit0, B=1, ..., Left=12, Up=13, Right=14,
  Down=15) it ORs those buttons into the state during `[force_after_seconds,
  force_after_seconds + force_for_seconds)` from when the mod is loaded. The game sees it
  as a real press: with `0x8000` for 0.3 s the cursor on the language screen moved down
  from English to Spanish in one run and to French in another (the menu repeats the held
  key);
- with `skip_style_set_value` (>= 0), the GetNpadStyleSet `pre` **replaces** the function
  (`PLA_SKIP`) and returns that value without doing the IPC to hid. With `1` (FullKey, what the game
  already receives in suyu) the behavior does not change: it is useful to test the replacement path.

Result on PLA 1.1.1 (language screen, 150 s, DLLs from the `-ShlImm` export): FullKey and
Handheld are queried 30 times per second (4273 calls each), GetNpadStyleSet 4277
(all replaced), 30 FPS, 0 transitions to the JIT. See `STATUS.md`.
