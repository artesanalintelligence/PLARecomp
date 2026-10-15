# Toolchain

> **Status (2026-10-03):** the project shifted priority toward a *recomp*
> (static AArch64 -> x86-64 recompilation). This *matching* decomp infrastructure is
> **functional but minimal** and paused. See "Pending" at the end.

## What is here

| File | Purpose |
|---|---|
| `fetch_clang.py` | Downloads the official clang/LLVM for Windows and extracts only what is needed to `cache/clang-<ver>/` |
| `ToolchainNX64.cmake` | CMake toolchain: AArch64 objects + linking of a botw-style shared ELF |
| `musl/` | Submodule `open-ead/botw-lib-musl` (libc headers, same as botw/SMO) |
| `cache/` | Downloaded clangs (in `.gitignore`) |
| `../CMakeLists.txt` | Target `pla` (all of `src/**/*.cpp` except `src/_smoke`) and `pla_smoke` |
| `../tools/common/` | Submodule `open-ead/nx-decomp-tools` (viking, asm-differ, scripts) |
| `../tools/config.toml` | viking configuration (`data/main_functions.csv`, target `pla`) |
| `../tools/bin/` | `check.exe`, `listsym.exe`, `decompme.exe` (compiled viking; in `.gitignore`) |

## Installed clangs

| Version | Size | Notes |
|---|---|---|
| 9.0.0 | 160 MiB | default |
| 10.0.0 | 172 MiB | |

Each contains `bin/clang.exe` (+ `clang++.exe` as a hard link), `ld.lld.exe`,
`llvm-objdump.exe`, `llvm-objcopy.exe`, the small MSVC runtime DLLs,
`lib/clang/<ver>/include` and `libcxx-include/` (libc++ headers of the same
version, with a generated `__config_site`). The Windows installers of those
versions do **not** ship `llvm-readelf`; use `llvm-objdump -x/-t` or
`C:\devkitPro\devkitA64\bin\aarch64-none-elf-readelf.exe`.

To add more (8.0.0, 11.0.0, 12.0.0, 13.0.0 are supported; ~170-240 MiB
download and ~160-200 MiB installed each):

```sh
python toolchain/fetch_clang.py 11.0.0 12.0.0   # or --all, --list
```

Requires 7-Zip (`C:\Program Files\7-Zip\7z.exe`, or `scoop install 7zip`).

## Building

```sh
cmake -G Ninja -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/ToolchainNX64.cmake -DPLA_CLANG=9.0.0
ninja -C build            # target pla  -> build/pla (what viking compares)
ninja -C build pla_smoke  # smoke test -> build/pla_smoke
```

Use the CMake from `C:\Program Files\CMake\bin` (the one in `C:\devkitPro\msys2` comes
first in the Git Bash PATH and is a different one).

Options (`-D...`):

| Option | Default | Alternatives |
|---|---|---|
| `PLA_CLANG` (or environment variable `PLA_CLANG`) | `9.0.0` | another version from `cache/`, or the path to a clang directory |
| `PLA_TRIPLE` | `aarch64-none-elf` | `aarch64-linux-elf` (what botw and SMO use with their musl sysroot) |
| `PLA_OPT` | `-O3` | `-O2`, `-Os` |
| `PLA_EXTRA_FLAGS` | empty | e.g. `-fno-omit-frame-pointer` |
| `PLA_LINK_FLAGS` | `-shared -Bsymbolic-functions --hash-style=sysv -z notext --no-gc-sections` | |

Fixed flags: `-mcpu=cortex-a57+fp+simd+crypto+crc -mno-implicit-float -fPIC
-ffunction-sections -fdata-sections -fno-exceptions -fstandalone-debug -g
-std=c++17`, and `-fno-rtti` on the targets. `PLA_OPT` is always applied,
regardless of `CMAKE_BUILD_TYPE`.

### Decisions

- **libc / libc++:** libc++ of the same LLVM version + musl (`botw-lib-musl`),
  like botw and SMO, because the NintendoSDK uses libc++ on a musl-derived
  libc. The devkitA64 headers (newlib + GCC 16 libstdc++) were discarded:
  they change the STL ABI and the `std::` *mangling*. The include order
  is fixed by hand (`-nostdinc`, then libc++, then clang's internal headers
  and finally musl) so that nothing from the host slips in.
- **Linking:** `ld.lld` is called directly, not the clang driver (the
  driver's behavior with `aarch64-none-elf` changes from one version to
  another). A `-shared` ELF is generated with `.plt`, `.dynsym` and `DT_HASH`, like botw's
  `uking`, so that viking can compare symbol by symbol and resolve
  calls to non-decompiled functions through the PLT.
- **Triple:** `aarch64-none-elf` by default (as requested). Nintendo's clang
  uses `aarch64-nintendo-nx-elf`; upstream does not know that OS and treats it as
  generic ELF. If there are codegen differences, try `aarch64-linux-elf`.

## viking (nx-decomp-tools)

Built **natively on Windows** with Rust stable MSVC (rustup in
`~/.cargo/bin`, Rust 1.99, VS 2019 Build Tools). Neither WSL nor the
Python plan B was needed. To rebuild:

```sh
cargo build --release --manifest-path tools/common/viking/Cargo.toml
cp tools/common/viking/target/release/{check,listsym,decompme}.exe tools/bin/
rm -rf tools/common/viking/target   # takes ~740 MB
```

Usage (from the repo root): `tools/bin/check.exe [symbol]`,
`tools/bin/listsym.exe`. Needs `data/main.elf`, `data/main_functions.csv`
(header `Address,Quality,Size,Name`; addresses with base `0x7100000000`;
qualities `O` matching, `m` minor difference, `M` major, `W` in progress, `U` not
decompiled, `L` library), `data/data_symbols.csv` (header
`Address,Name`; **mandatory even if empty**) and `build/pla`. (The `data/` files are
generated locally from your own game dump and are not in the public repo; see docs/PUBLISHING.md.)

Limitations on Windows: `check <symbol>` launches asm-differ when it finds a
mismatch, and the nx-decomp-tools shim assumes `.venv/bin` (Linux). For the visual
diff use WSL or call `tools/common/asm-differ/diff.py` by hand. `check` without
arguments ("all functions" mode) does not use asm-differ.

## Smoke test performed

`src/_smoke/smoke.cpp` (3 trivial functions, includes `<cstdint>`,
`<cstring>`, `<type_traits>` from libc++/musl) compiles and links with clang 9.0.0 and
10.0.0. Useful result: `dot()` comes out identical in both, but `hashFnv1a()`
differs in the prologue (order of `stp x29,x30` / `stp x20,x19`) and in a jump,
so codegen does let us tell clang versions apart.

## Pending (not done because of the priority change)

1. `tools/setup.py` (verify `data/main.elf`, prepare `build/`, check the
   toolchain) - not written.
2. Full smoke test with viking: generate a "fake target" with functions at
   known addresses + `data/smoke_functions.csv` and demonstrate OK/mismatch.
   Note: viking requires `.plt` in the original ELF and `.rela.dyn` in the decomp
   (the current `pla_smoke` has no `.rela.dyn` because it references no data).
3. `toolchain/compare_candidates.py` (clang x flags matrix against the real
   binary) - not written. Planned sizing:
   clangs x {`-O2`,`-O3`} x {`aarch64-none-elf`,`aarch64-linux-elf`} x {with/without `-fno-omit-frame-pointer`}.
4. Install more candidates (8, 11, 12, 13) once the NintendoSDK version is
   read from the dump.
5. `tools/check.py` in Python: unnecessary as long as viking builds on Windows.
