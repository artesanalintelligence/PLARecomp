# tools/extract - from the dump to `main.elf`

These tools convert your copy of the game (XCI/NSP) into the project's
working files: the **1.1.1** ExeFS, `data/main.nso`,
`data/main.elf` and `data/target.json`.

Nothing they generate is pushed to git: the game binaries, the RomFS and the
keys are in `.gitignore`. All outputs (including `data/target.json`) are generated locally from your own dump and
are not in the public repo (see docs/PUBLISHING.md).

## Step by step

### 1. Requirements (one time only)

- Python 3.10 or later (on this machine: `C:\Python312`).
- The keys from **your** console, dumped with Lockpick_RCM, in
  `%USERPROFILE%\.switch\prod.keys` (and, if you have it, `title.keys`).
- Install the Python modules and the external tools:

```powershell
python tools/extract/bootstrap.py
```

`bootstrap.py` runs `pip install -r tools/extract/requirements.txt`
(lz4, pycryptodome, pytest) and downloads **hactool 1.4.0** into `tools/bin/`,
checking its SHA-256. It also tries to download **hactoolnet**, but the
LibHac repository is no longer on GitHub; if you have a release zip,
pass it like this:

```powershell
python tools/extract/bootstrap.py --hactoolnet-zip C:\path\hactoolnet-0.19.0-win.zip
```

Neither of the two is essential for the ExeFS (see "How it works").

### 2. Copy the game into `dump/`

Leave in `dump/` (in any subfolder) whatever you have:

| Format | What is needed |
|---|---|
| Base game NSP + 1.1.1 update NSP | The usual case. The update's ticket (`.tik`) is inside the NSP. |
| XCI + update NSP | The XCI provides the base game. |
| NSZ / XCZ | Decompressed automatically with `nsz` (`python -m pip install nsz`) and the NSP is left in `dump/` for next time. |
| Loose NCAs (`*.nca`, `*.cnmt.nca`) + `.tik` | Identified by their CNMT; if the names are not the content IDs, by their header. |
| Already extracted ExeFS folder (`main`, `main.npdm`, `sdk`...) | Used as is (the version cannot be checked). |

### 3. Run the extraction

From the repo root:

```powershell
python tools/extract/extract.py
```

If you only want to see what is in `dump/` without extracting anything:

```powershell
python tools/extract/extract.py --list
```

### 4. Result

```
data/exefs/main, main.npdm, rtld, sdk, subsdk0, subsdk1   1.1.1 ExeFS
data/main.nso                 copy of exefs/main (compressed, as is)
data/main.uncompressed.nso    the same NSO uncompressed (like nx2elf --export-uncompressed)
data/main.elf                 AArch64 ELF64 for Ghidra, IDA, objdump and viking
data/target.json              version, SHA-256, build IDs, segments, SDK versions
```

For the RomFS (about 6 GB more; needs hactool or hactoolnet and the base game):

```powershell
python tools/extract/extract.py --romfs
```

While it runs, the base game's program NCA (6 GB) is copied to
`dump/.extract-work/` and deleted when finished. Make sure you have at least
15 GB free.

## Useful options

| Option | Purpose |
|---|---|
| `--list` | Lists NCAs, CNMTs and tickets found and exits. |
| `--romfs` | Also extracts `data/romfs/`. |
| `--keys PATH` / `--title-keys PATH` | Keys in another location. |
| `--allow-base` | Continue without the update (extracts the 1.0.0 executable; **not** useful for matching). |
| `--any-version` | Accept an update other than 1.1.1. |
| `--tool hactool\|hactoolnet\|none` | Choose the external tool (by default, whichever is in `tools/bin`). |
| `--no-verify` | Skip the cross-verification of the ExeFS. |
| `--raw-elf` | `main.elf` without filling in `.got` (bytes identical to the NSO). |
| `--elf-base 0x7100000000` | ELF base (default 0, which is what viking expects). |
| `--keep-work` | Do not delete `dump/.extract-work/`. |

## Common error messages

- **"No ...prod.keys found"**: copy your keys to `%USERPROFILE%\.switch\prod.keys` or use `--keys`.
- **"the header does not decrypt to 'NCA3'"**: `header_key` is wrong or the file is not an NCA.
- **"Missing key 'titlekek_0c'"** (or another): your `prod.keys` is from a firmware older than the game. Run Lockpick_RCM again with the updated console.
- **"missing titlekey for Rights ID ..."**: the update NSP does not carry its `.tik`. Dump it again with nxdumptool (which includes the ticket) or add the titlekey to `title.keys`.
- **"The ticket is personalized"**: the ticket is tied to your console; dump again with the nxdumptool option that removes console-specific data.
- **"Update not found"**: the NSP `[01001F5010DFA800]` is missing. Dump it from the console.
- **"The update found is X, but 1.1.1 is expected"**: the update is a different version (1.1.1 = `v262144`).

## How it works

1. **Inventory**: reads PFS0 (NSP), HFS0 (XCI), tickets and NCA headers
   directly from the containers, without copying them.
2. **Keys**: `prod.keys` + `title.keys` + the tickets in the NSPs. The
   titlekeys are decrypted with `titlekek_XX`; NCAs without a Rights ID use the
   *key area* with `key_area_key_application_XX`.
3. **Selection**: with the CNMTs it identifies the base game (`Application`,
   `01001F5010DFA000`) and the update (`Patch`, `01001F5010DFA800`), and reads
   `control.nacp` to check the displayed version (`1.1.1`).
4. **ExeFS**: the update's program NCA contains the complete ExeFS (only
   the RomFS comes as a BKTR patch over the base), so it is decrypted in Python
   (`nxfmt.py`: AES-XTS for the header, AES-CTR for the section) and the
   PFS0 is extracted. If hactool/hactoolnet is present, it is also extracted with them and
   the SHA-256s are required to match.
5. **RomFS** (`--romfs`): hactool/hactoolnet with `--basenca` (BKTR patch).
6. **ELF**: `nso2elf.py` (below) and `target.json`.

The external tools are run with a temporary `HOME` folder that
contains a copy of `prod.keys` and only the necessary titlekeys; it is deleted
when finished and keys are never shown on screen.

## nso2elf.py

```powershell
python tools/extract/nso2elf.py data/main.nso output.elf [--base 0x7100000000] [--resolve-got]
                                [--export-uncompressed output.nso] [--info]
```

Pure Python + `lz4`. Decompresses the segments, validates sizes and SHA-256 of
the NSO header, locates MOD0 (`.dynamic`, `.bss`, `.eh_frame_hdr`) and
rebuilds: `.text .rodata .data .bss .dynamic .dynsym .dynstr .rela.dyn
.rela.plt .relr.dyn .plt .got .got.plt .hash .gnu.hash .init .fini
.init_array .fini_array .note.gnu.build-id .eh_frame_hdr .eh_frame`. Gaps
that cannot be named are left as `.text.N`, `.rodata.N`, `.data.N`.
Program headers: 3 x `PT_LOAD`, `PT_DYNAMIC`, `PT_GNU_EH_FRAME`, `PT_NOTE`.

### Compatibility with viking (nx-decomp-tools) and differences from nx2elf

viking (`tools/check`) loads `data/main.elf` and needs: `PT_LOAD` to go
from address to offset, `PT_DYNAMIC` with `DT_HASH` (from which it gets the number of
symbols), `.rela.dyn` linked to `.dynsym`, `.plt` (the PLT entries
are at `.plt + (i + 2) * 0x10`) and, optionally, `.got`. The addresses in the
function CSV carry the base `0x7100000000`, but the ELF must be at
base 0 (viking subtracts the base). nso2elf meets all of that; the tests
check it.

Differences from nx2elf (the converter used by botw and OdysseyDecomp):

| | nx2elf | nso2elf |
|---|---|---|
| File layout | segments one after another following the headers | `offset = vaddr + 0x1000` (page-congruent; the ELF is loadable by standard tools) |
| `.got` | looks for `&_DYNAMIC` after the `JUMP_SLOT`s; in binaries linked with lld (like PLA) it does not exist and it **does not create `.got`** | rebuilds it from the `GLOB_DAT` relocations (lld style) or from `_GLOBAL_OFFSET_TABLE_[0]` (GNU ld style) |
| `.got` contents | as is (in PLA all entries are 0: lld does not write the addend) | same by default; with `--resolve-got` (what `extract.py` uses) each entry contains its target, so viking can verify data accesses through the GOT |
| `.init` / `.fini` | `.init` up to the first `ret`; `.fini` up to the first `b` (max. 32 instructions) | both up to the first `ret` or `b` (max. 256 instructions; in PLA, `.init` has 77) |
| `.dynamic` without `DT_NULL` (libnx) | reads too far | stops at the first invalid tag |
| `DT_RELR` | no | `.relr.dyn` |
| Unnamed gaps | each segment is trimmed to the largest gap | additionally `.text.N`/`.rodata.N`/`.data.N` are created |
| Section indices | those of `st_shndx` in `.dynsym` | same |
| Base | fixed 0 | 0 by default, `--base` rebases `.dynamic`, `.dynsym` and `.rela.*` (not RELR's implicit addends) |
| Hashes | does not check them | aborts if a SHA-256 in the header does not match (`--no-strict` to warn) |

Neither generates `.symtab`: names are assigned afterwards by Ghidra or the
function CSV.

## target.json

```json
{
  "game": "<game title>",
  "title_id": "01001F5010DFA000",
  "update_title_id": "01001F5010DFA800",
  "version": {"display": "1.1.1", "number": 262144, "source": "update"},
  "base_version": {"display": "1.0.0", "number": 0},
  "program_nca": {"content_id": "...", "key_generation": 12, "sdk_addon_version": "11.4.6", "rights_id": true},
  "verified_with": "hactool",
  "sdk_versions": ["SDK MW+Nintendo+NintendoSdk_nnSdk-11_4_0-Release", "..."],
  "exefs": {
    "main": {"size": 0, "sha256": "...", "build_id": "...", "build_id_short": "...",
             "segments": {"text": {"mem_offset": "0x0", "size": 0, "file_size": 0, "compressed": true, "sha256": "..."},
                          "rodata": {}, "data": {}},
             "bss_size": 0},
    "main.npdm": {"size": 0, "sha256": "...", "name": "Application", "program_id": "01001F5010DFA000"}
  },
  "main_elf": {"path": "data/main.elf", "sha256": "...", "base": "0x0", "resolve_got": true}
}
```

The `sha256` of each file is that of the NSO as it comes in the ExeFS; that of
each segment is that of the decompressed segment (the same one stored in the
NSO header).

## Tests

```powershell
python -m pytest tools/extract/tests -q
```

They need neither the game nor your keys:

- `test_nso2elf.py` compiles `tests/fixture/testmod.cpp` (classes, vtables,
  RTTI, exceptions, static constructors, unresolved imports) with
  devkitA64 + libnx, in two variants (with and without `DT_RELR`), converts it to
  NSO with `elf2nso` and checks that the `nso2elf` ELF has `.text`,
  `.rodata`, `.data` and all the other sections byte-identical to the
  original, that all dynamic symbols are recovered, that the structural
  sections are in place and that what viking asks for is met.
  Needs `C:\devkitPro` (skipped if it is not there).
- `test_extract.py` generates synthetic NCAs, NSPs, XCIs and tickets with
  random keys and tests `extract.py` end to end: base + update, XCI,
  loose NCAs, ExeFS folder, and the errors (no update, wrong version,
  no ticket, no keys). If hactool is in `tools/bin`, it also checks
  that hactool extracts the same thing.

## Limitations

- hactoolnet can no longer be downloaded (LibHac was removed from GitHub). hactool
  1.4.0 is used, which works with PLA (including the patched RomFS).
- The RomFS depends on an external tool; the Python reader does not apply
  BKTR patches.
- Only NCA3 (all modern content). Personalized tickets need
  `eticket_rsa_keypair` in `prod.keys`.
- `.rodata`, `.data` and `.text` are reconstructions: the exact boundaries of
  `.data.rel.ro`, `.gcc_except_table`, etc. are not preserved in an NSO.
- There is no `PT_TLS`: the NSO does not store the TLS template.
