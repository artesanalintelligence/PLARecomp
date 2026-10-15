#!/usr/bin/env python3
"""Port launcher: from the user's dump (NSP/NSZ/XCI + their keys) to the playable folder.

Console prototype (design in docs/port/LAUNCHER.md). It chains the scripts that already exist
(tools/extract, tools/patch, tools/analysis, recomp/scripts, tools/port) into phases with state
so it can resume. The game-specific part is in tools/launcher/profiles/<profile>.json.

  python tools/launcher/launcher.py --input "D:\\ISOS\\PLA" --dest D:\\Games\\PLA --dry-run
  python tools/launcher/launcher.py --input dump --dest D:\\Games\\PLA                # cheap/medium phases
  python tools/launcher/launcher.py ... --allow-heavy                                  # everything
  python tools/launcher/launcher.py ... --only export,compile --allow-heavy           # some phases

Costs: "cheap" (seconds, always runs), "medium" (minutes or a few GB), "heavy"
(export/compile/decompress the base game: only with --allow-heavy). --dry-run runs
nothing except the requirements and the identification (read-only).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Dict, List, Optional

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))

import identify as idf  # noqa: E402
import requirements as rq  # noqa: E402

PY = sys.executable
PS = [str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"),
      "-NoProfile", "-ExecutionPolicy", "Bypass", "-File"]
COSTS = ("cheap", "medium", "heavy")
EVENTS = False  # --events: besides the text, one "@@ {json}" line per event (read by the graphical installer)


def event(kind: str, **kw) -> None:
    if EVENTS:
        print("@@ " + json.dumps({"ev": kind, **kw}, ensure_ascii=False, default=str), flush=True)


# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

@dataclass
class Step:
    desc: str
    argv: Optional[List[str]] = None            # external command
    env: Dict[str, str] = field(default_factory=dict)
    fn: Optional[Callable[[], None]] = None      # internal step (Python)

    def text(self) -> str:
        if self.argv is None:
            return f"(internal) {self.desc}"
        e = " ".join(f"{k}={v}" for k, v in self.env.items())
        return (f"[{e}] " if e else "") + subprocess.list2cmdline(self.argv)


@dataclass
class Phase:
    id: str
    title: str
    cost: str
    steps: List[Step]
    outputs: List[Path]
    gb: float = 0.0          # estimated disk writes (GB)
    minutes: float = 0.0     # estimated duration on this class of machine
    note: str = ""
    skip: str = ""           # reason not to run it (e.g. already solved by the input)

    def fingerprint(self, previous: str) -> str:
        h = hashlib.sha256(previous.encode())
        for p in self.steps:
            h.update(p.text().encode())
        return h.hexdigest()[:16]


@dataclass
class Ctx:
    profile: dict
    inputs: List[Path]
    work: Path
    dest: Path
    fork: Path
    keys: Optional[Path]
    title_keys: Optional[Path]
    jobs: int
    mcp_port: int
    toolchain: str = "vs"          # "vs" (VS Build Tools + LLVM + CMake/Ninja) or "portable" (tools/toolchain)
    toolchain_dir: Optional[Path] = None
    build_cmd: str = ""            # custom build command (replaces build-modules.ps1), see --build-cmd
    split_mb: float = 8.0          # portable toolchain: split units into ~N MB parts (0 = whole units, old build)
    exporter: str = "cli"          # "cli" (headless aot_export.exe + romfs_bktr.py) or "gui" (fork suyu.exe over MCP)
    isa: str = "x86-64"            # portable toolchain: target ISA of the DLLs (auto = this CPU's level, TOOLCHAIN.md §8)
    ident: dict = field(default_factory=dict)

    # work paths
    @property
    def input_dir(self) -> Path: return self.work / "input"
    @property
    def data(self) -> Path: return self.work / "data"
    @property
    def suyu(self) -> Path: return self.work / "suyu"
    @property
    def suyu_user(self) -> Path: return self.work / "appdata" / "suyu"
    @property
    def export(self) -> Path: return self.work / "export"
    @property
    def build(self) -> Path: return self.work / "build"
    @property
    def tid(self) -> str: return self.profile["title_id"]
    @property
    def exporter_exe(self) -> Path:
        """The install-time exporter: the headless aot_export.exe (fork tools/recomp_reemit, built into
        <fork>\\build\\aot_export) or, with --exporter gui, the isolated copy of the fork's suyu.exe."""
        return self.suyu / "suyu.exe" if self.exporter == "gui" else aot_export_exe(self.fork)

    def env_scripts(self) -> Dict[str, str]:
        # env.ps1 derives the fork from PLA_RECOMP_ROOT (<root>\mk8-recomp) and puts the logs in <root>\logs.
        return {"PLA_RECOMP_ROOT": str(self.fork.parent)}

    def input_files(self) -> List[dict]:
        return self.ident.get("files", [])

    def source(self, key: str) -> Optional[Path]:
        v = self.ident.get(key)
        return Path(v["file"]) if v else None

    def dump_dir(self) -> Path:
        """Folder that tools/extract will see: the user's own if it works as-is, otherwise input/."""
        files = self.input_files()
        dirs = {Path(a["path"]).parent for a in files}
        compressed = any(a["format"] in ("nsz", "xcz", "ncz") for a in files)
        if len(dirs) == 1 and not compressed:
            return next(iter(dirs))
        return self.input_dir

    def decompressed_nsp(self, src: Optional[Path]) -> Optional[Path]:
        if not src:
            return None
        ext = {".nsz": ".nsp", ".xcz": ".xci"}.get(src.suffix.lower())
        if ext:
            return self.input_dir / (src.stem + ext)
        return src if self.dump_dir() != self.input_dir else self.input_dir / src.name


# ---------------------------------------------------------------------------
# Internal steps
# ---------------------------------------------------------------------------

def aot_export_exe(fork: Path) -> Path:
    return fork / "build/aot_export/aot_export.exe"


def export_title(ctx: "Ctx") -> str:
    """Game name the exporter writes into the generated sources (the NACP title, as the GUI export used it).
    Read from the user's dump at install time; never stored in the repo."""
    return ctx.ident.get("game") or ""


def nsz_cmd() -> List[str]:
    sys.path.insert(0, str(REPO / "tools" / "extract"))
    import extract as ex  # noqa: E402
    return ex.find_nsz() or ["nsz"]


def nsz_home(ctx: "Ctx") -> Path:
    """nsz has no --keys option: it only reads <home>\\.switch\\prod.keys (and a few fixed paths). The keys the user
    selected are copied into a private home inside the work folder and nsz runs with USERPROFILE/HOME pointing there."""
    return ctx.work / "nsz-home"


def prepare_nsz_home(ctx: "Ctx") -> None:
    d = nsz_home(ctx) / ".switch"
    d.mkdir(parents=True, exist_ok=True)
    for src, name in ((ctx.keys or Path.home() / ".switch/prod.keys", "prod.keys"),
                      (ctx.title_keys or Path.home() / ".switch/title.keys", "title.keys")):
        if src and Path(src).is_file():
            shutil.copy2(src, d / name)
    if not (d / "prod.keys").is_file():
        raise RuntimeError("no prod.keys to give to nsz: select your keys file")


def nsz_step(ctx: "Ctx", src: Path) -> List["Step"]:
    home = str(nsz_home(ctx))
    return [Step("keys for nsz", fn=lambda: prepare_nsz_home(ctx)),
            Step(f"decompress {src.name}", nsz_cmd() + ["-D", "-w", "-o", str(ctx.input_dir), str(src)],
                 env={"USERPROFILE": home, "HOME": home})]


def link_or_copy(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        return
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)


def junction(link: Path, target: Path) -> None:
    target.mkdir(parents=True, exist_ok=True)
    if link.exists():
        return
    link.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(["cmd.exe", "/c", "mklink", "/J", str(link), str(target)], check=True, capture_output=True)


def copy_extra(name: str, mod_id: str, dst: Path, fork: Path) -> None:
    cands = [REPO / "build/port" / mod_id / name, fork / "build/suyu/bin" / name]
    cands += sorted((Path.home() / ".nuget/packages/ultz.native.sdl").glob(f"*/runtimes/win-x64/native/{name}"), reverse=True)
    src = next((c for c in cands if c.is_file()), None)
    if not src:
        print(f"    warning: {name} not found; the {mod_id} mod works without it with fewer features (SDL controllers)")
        return
    shutil.copy2(src, dst / name)


def copy_prebuilt_mod(src: Path, dst: Path) -> None:
    """Copies a prebuilt mod folder; config values already saved in dst/mod.json (in-game menu) are kept."""
    old = {}
    if (dst / "mod.json").is_file():
        try:
            old = json.loads((dst / "mod.json").read_text(encoding="utf-8")).get("config") or {}
        except (OSError, ValueError):
            old = {}
    shutil.copytree(src, dst, dirs_exist_ok=True)
    if old:
        doc = json.loads((dst / "mod.json").read_text(encoding="utf-8"))
        doc["config"] = {**doc.get("config", {}), **{k: v for k, v in old.items() if k in doc.get("config", {})}}
        (dst / "mod.json").write_text(json.dumps(doc, indent=2), encoding="utf-8")


LOCAL_MOD_BUILDERS = {"islands": REPO / "tools/islands/build_islands.py"}


def build_local_mod(ctx, mod_id: str, dst: Path) -> None:
    """Builds a mod that is generated from the user's main.elf (profile dist.mods_local) and installs it like a
    prebuilt one (config values saved in dst/mod.json are kept). Needs the portable toolchain (with llvm-objdump);
    without it the mod is left out (it is an optimization) and the step says so."""
    tc = ctx.toolchain_dir or default_portable_toolchain()
    if not (tc / "bin/llvm-objdump.exe").is_file():
        log_line(ctx, "assemble", f"mod {mod_id} skipped: the portable toolchain ({tc}) with llvm-objdump is not available")
        return
    out = ctx.build / "mods-local" / mod_id
    r = subprocess.run([PY, str(LOCAL_MOD_BUILDERS[mod_id]), "--elf", str(ctx.data / "main.elf"), "--toolchain", str(tc),
                        "--out", str(out)], capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        raise RuntimeError(f"mod {mod_id}: build failed\n{(r.stdout + r.stderr)[-2000:]}")
    log_line(ctx, "assemble", r.stdout.strip())
    copy_prebuilt_mod(out, dst)


NVNVK_TEMPLATE = REPO / "build/port/nvnvk/mod.template.json"
# The game's PLA.exe: shipped in the platform package; in a development checkout, tools/port/plahost/build.ps1.
PLAHOST_EXE = REPO / "build/port/plahost/PLA.exe"


def nvnvk_mod_json(dst: Path, mode: str, log_dir: str, pipeline_cache_out: str, extra: str) -> None:
    """Writes nvnvk's mod.json from the hook template shipped in the platform package (generated once at release
    time by tools/port/nvnvk/gen_tables.py --mod nvnvk). The config is the one gen_tables would write."""
    tpl = NVNVK_TEMPLATE
    if not tpl.is_file():  # development checkout: generate the template with gen_tables (needs the local SDK map)
        gen = REPO / "tools/port/nvnvk/gen_tables.py"
        tmp = REPO / "build/port/nvnvk/template-gen"
        subprocess.run([PY, str(gen), "--mod", "nvnvk", "--mod-json", str(tmp)], check=True, capture_output=True)
        tpl = tmp / "mod.json"
    doc = json.loads(tpl.read_text(encoding="utf-8"))
    cfg = {"mode": mode, "log_dir": log_dir, "capture_dir": "", "capture_every": 0, "capture_max": 0,
           "validation": False, "present": True, "vsync": False, "shader_cache_dir": "",
           "pipeline_cache": "", "pipeline_cache_out": pipeline_cache_out, "capture_path": "", "capture_frames": 1200,
           # Performance settings of the tested play setup (unified memory: ~+40 % FPS; see docs/HANDOFF.md
           # sprint 9) and the final-pass defaults (FXAA that leaves the HUD sharp, sharp upscale, 16x AF).
           "import_memory": True, "game_wait": 3, "ssbo_persist_rw_only": True, "chunk_draws": 0,
           "anisotropy": 16, "aa": "fxaa", "aa_hud": True, "sharpen": 0, "upscale": "sharp"}
    managed = {"mode", "log_dir", "pipeline_cache", "pipeline_cache_out"}
    for kv in filter(None, extra.split(";")):  # "key=value;key=value" (JSON or text value)
        k, _, v = kv.partition("=")
        managed.add(k.strip())
        try:
            cfg[k.strip()] = json.loads(v)
        except json.JSONDecodeError:
            cfg[k.strip()] = v
    try:  # re-assembly: keep what the in-game menu saved, except the keys the launcher manages
        old = json.loads((dst / "mod.json").read_text(encoding="utf-8")).get("config") or {}
        cfg.update({k: v for k, v in old.items() if k not in managed})
    except (OSError, ValueError):
        pass
    doc["config"] = cfg
    dst.mkdir(parents=True, exist_ok=True)
    (dst / "mod.json").write_text(json.dumps(doc, indent=1), encoding="utf-8")


NNSHIM_TEMPLATE = REPO / "build/port/nnshim/mod.template.json"
NNSHIM_CONTROLS = REPO / "build/port/nnshim/controls.default.json"


def nnshim_mod_json(dst: Path, game: Path) -> None:
    """Writes nnshim's mod.json from the hook template shipped in the platform package ({GAME} = game folder),
    and a default controls.json if there is none. Re-assembly keeps what the in-game menu saved."""
    tpl = NNSHIM_TEMPLATE
    if not tpl.is_file():  # development checkout: build the template (needs nnshim/generated/*.inc)
        tpl = REPO / "build/port/nnshim/template-gen/mod.template.json"
        subprocess.run([PY, str(REPO / "tools/port/nnshim/nnshim_template.py"), "--out", str(tpl)], check=True,
                       capture_output=True)
    doc = json.loads(tpl.read_text(encoding="utf-8").replace("{GAME}", game.as_posix()))
    managed = {"save_root", "romfs_bin", "font", "hid", "ui", "err"}
    try:
        old = json.loads((dst / "mod.json").read_text(encoding="utf-8")).get("config") or {}
        doc["config"].update({k: v for k, v in old.items() if k not in managed})
    except (OSError, ValueError):
        pass
    dst.mkdir(parents=True, exist_ok=True)
    (dst / "mod.json").write_text(json.dumps(doc, indent=1), encoding="utf-8")
    ctl = dst / "controls.json"
    if not ctl.exists():
        src = NNSHIM_CONTROLS if NNSHIM_CONTROLS.is_file() else REPO / "tools/port/nnshim/controls.default.json"
        shutil.copy2(src, ctl)
    for sub_dir in ("user/log", "user/ui-dump"):
        (game / sub_dir).mkdir(parents=True, exist_ok=True)


def file_identity(p: Path) -> str:
    """Size and date of a platform binary. Part of the phase fingerprints: a newer exporter or PLA.exe (a new
    platform package) redoes the export, the compile and the assembly instead of reusing what an older binary made."""
    try:
        st = p.stat()
    except OSError:
        return "missing"
    return f"{st.st_size}/{time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(st.st_mtime))}"


# Emitter environment variable -> key the exporter writes into aot_manifest.json when the option took effect.
EMITTER_MANIFEST_KEYS = {
    "SUYU_AOT_CHAIN": "chain", "SUYU_AOT_FPXV": "fpxv", "SUYU_AOT_FPXFZ": "fpxfz", "SUYU_AOT_FPXOOL": "fpxool",
    "SUYU_AOT_IDXMISS": "idxmiss", "SUYU_AOT_XCHAIN": "xchain", "SUYU_AOT_FMINLINE": "fminline",
    "SUYU_AOT_FMINLINEST": "fminline_st", "SUYU_AOT_FM2": "fm2",
    "SUYU_AOT_EXCLINLINE": "exclinline", "SUYU_AOT_FPXIX": "fpxix", "SUYU_AOT_DCHAIN": "dchain",
}
IMAGE_FEATURE_FPX1 = 4  # Core::RecompImageFeature::ExactFpX1 (FastmemPT1 = 1, GuardGen1 = 2)
IMAGE_FEATURE_FM2 = 8   # FM2 direct fastmem (plahost only; docs/port/PROGRESS-FPS-FASTMEM.md)


def verify_emitter(ctx: Ctx) -> None:
    """The exporter silently ignores the options it does not know: an older suyu.exe exports plain C that runs at
    half the FPS (2026-10-09: a platform package with the 2026-10-04 exporter). Checks that every option of the
    profile is recorded in the export's manifest."""
    em = ctx.profile["emitter"]
    man_p = ctx.export / "pkg/aot_cache/aot_manifest.json"
    man = json.loads(man_p.read_text(encoding="utf-8"))
    missing = [f"{k} ({v})" for k, v in EMITTER_MANIFEST_KEYS.items()
               if em["env"].get(k, "0") not in ("", "0") and man.get(v) is not True]
    if em.get("shl_imm") and man.get("translate_shl_imm") is not True:
        missing.append("-ShlImm (translate_shl_imm)")
    if em["env"].get("SUYU_AOT_FPX", "0") not in ("", "0") and not int(man.get("image_features", 0)) & IMAGE_FEATURE_FPX1:
        missing.append("SUYU_AOT_FPX (image_features & 4)")
    if em["env"].get("SUYU_AOT_FM2", "0") not in ("", "0") and not int(man.get("image_features", 0)) & IMAGE_FEATURE_FM2:
        missing.append("SUYU_AOT_FM2 (image_features & 8)")
    if missing:
        raise RuntimeError(f"the exporter ({ctx.exporter_exe}, {file_identity(ctx.exporter_exe)}) did not apply "
                           f"these profile options: {', '.join(missing)}. It is older than the profile: update the "
                           "platform package (fork build) and run again.")
    print(f"  emitter options verified in {man_p.name}: "
          + ", ".join(v for k, v in EMITTER_MANIFEST_KEYS.items() if man.get(v) is True))


def verify_exefs(ctx: Ctx) -> None:
    exefs = idf.exefs_from_folder(ctx.data / "exefs")
    diff = idf.compare_exefs(exefs, ctx.profile)
    if diff:
        raise RuntimeError("the extracted ExeFS does not match the profile: " + "; ".join(diff))
    print(f"  ExeFS verified: {len(exefs)} files with the profile's SHA-256 and build ids")


# Original (Spanish) PLA.ini key names -> English ones. PLA.exe and nvnvk accept both; an older
# profile that still uses the original names is written out with the English ones.
LEGACY_INI_KEYS = {
    "limitador": "limiter", "estricto": "strict", "pantalla_completa": "fullscreen",
    "shaders_asincronos": "async_shaders", "precision_gpu_alta": "high_gpu_accuracy",
    "titulo_detallado": "detailed_title", "renderizador": "renderer", "espera_activa": "busy_wait",
    "hilos_nativos": "native_threads", "tope_fps_nvnvk": "nvnvk_fps_cap", "informe_cuelgue": "hang_report",
    "nombre": "name",
}


# Keys the in-game menu (nvnvk) writes into PLA.ini: a re-assembly keeps the player's values.
INI_MENU_KEYS = ("fullscreen", "nvnvk_fps_cap")


def write_ini(ctx: Ctx) -> None:
    d = ctx.profile["dist"]
    values = {LEGACY_INI_KEYS.get(k, k): v for k, v in d["ini_values"].items()}
    path = ctx.dest / d["ini"]
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            k, sep, v = line.partition("=")
            k = LEGACY_INI_KEYS.get(k.strip().lower(), k.strip().lower())
            if sep and k in INI_MENU_KEYS and v.strip():
                values[k] = v.strip()
    except OSError:
        pass
    lines = ["; Generated by tools/launcher (profile %s). See README.txt." % ctx.profile["id"], "[PLA]"]
    lines += [f"{k}={v}" for k, v in values.items()]
    path.write_text("\r\n".join(lines) + "\r\n", encoding="ascii")



def manifest(ctx: Ctx) -> None:
    D = ctx.dest
    missing, items = [], {}
    for m, info in ctx.profile["modules"].items():
        p = D / info["dll"]
        if not p.is_file():
            missing.append(info["dll"])
        else:
            items[info["dll"]] = p.stat().st_size
    for rel in (ctx.profile["dist"]["exe"], "exefs/main", "exefs/main.npdm", "exefs/romfs.bin", ctx.profile["dist"]["ini"]):
        if not (D / rel).is_file():
            missing.append(rel)
    if missing:
        raise RuntimeError("missing in the game folder: " + ", ".join(missing))
    doc = {"profile": ctx.profile["id"], "date": time.strftime("%Y-%m-%d %H:%M"), "dll_bytes": items,
           "exe": ctx.profile["dist"]["exe"], "exe_args": ctx.profile["dist"].get("exe_args_play", ""),
           "build_id_main": ctx.profile["expected_exefs"]["main"]["build_id"],
           "note": "Folder generated from the user's own dump and keys: for local use only, not to be distributed."}
    (D / "launcher-manifest.json").write_text(json.dumps(doc, indent=1, ensure_ascii=False), encoding="utf-8")
    print(f"  manifest: {D / 'launcher-manifest.json'}")


def default_portable_toolchain() -> Path:
    return Path(os.environ.get("PLA_RECOMP_ROOT", r"D:\PLARecomp")) / "toolchain-portable" / "recomp-toolchain"


def choose_toolchain(requested: str, dir_: Optional[Path]) -> tuple:
    """auto = the VS one if complete (VS Build Tools + clang-cl); otherwise the portable one if it exists."""
    tc_dir = dir_ or default_portable_toolchain()
    if requested == "auto":
        has_vs = bool(rq.vs_vcvars() and rq.clang_cl())
        requested = "vs" if has_vs or not (tc_dir / "bin/clang-cl.exe").is_file() else "portable"
    return requested, tc_dir


def resolve_isa(requested: str) -> str:
    """The -march level the DLLs are built for. auto = this CPU's x86-64 level (capped at v3). The resolved level is
    part of the compile phase's fingerprint, so the same install folder on a CPU without it gets rebuilt."""
    if requested != "auto":
        return requested
    try:
        sys.path.insert(0, str(REPO / "tools" / "toolchain"))
        import cpu_isa  # noqa: E402
        lv = cpu_isa.level()[0]
    except Exception:  # never fail an install over the detection: baseline x86-64 always works
        return "x86-64"
    return "x86-64-v3" if lv == "x86-64-v4" else lv


# PGO profile shipped with the release (pgo component -> build/pgo): <profile id>.profdata (sparse clang profile:
# per-function execution counters of the recompiled code, keyed by generated function names; no game code) and
# <profile id>.pgo.json (what it was trained on). Written by tools/toolchain/pgo_train.py, see LAUNCHER.md §8.12.
PGO_DIR = REPO / "build/pgo"


def file_sha256(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def pgo_choice(ctx: Ctx) -> tuple:
    """(profile path or None, message). The profile is used only when its manifest matches this install exactly:
    the exporter binary (SHA-256), the emitter options of the launcher profile and the game build. Anything else
    (missing files, other exporter or options, damaged file) compiles without PGO: never an install failure."""
    if getattr(ctx, "_pgo", None) is None:
        ctx._pgo = _pgo_choice(ctx)
    return ctx._pgo


def _pgo_choice(ctx: Ctx) -> tuple:
    if ctx.build_cmd or ctx.toolchain != "portable":
        return None, "no PGO: only with the portable toolchain"
    prof, man_p = PGO_DIR / f"{ctx.profile['id']}.profdata", PGO_DIR / f"{ctx.profile['id']}.pgo.json"
    if not prof.is_file() or not man_p.is_file():
        return None, f"no PGO: no profile in {PGO_DIR}"
    try:
        man = json.loads(man_p.read_text(encoding="utf-8"))
        em = ctx.profile["emitter"]
        want_em = {"backend": em["backend"], "shl_imm": bool(em.get("shl_imm")),
                   "env": {k: v for k, v in sorted(em["env"].items()) if v not in ("", "0")}}
        want_ids = {m: v["build_id"] for m, v in sorted(ctx.profile["expected_exefs"].items()) if "build_id" in v}
        problems = []
        if man.get("format") != 1 or man.get("profile") != ctx.profile["id"]:
            problems.append("manifest of another profile/format")
        if man.get("emitter") != want_em:
            problems.append("emitter options differ")
        if man.get("build_ids") != want_ids:
            problems.append("game build differs")
        exe = ctx.exporter_exe
        if not exe.is_file() or man.get("exporter_sha256") != file_sha256(exe):
            problems.append(f"exporter differs ({exe.name})")
        if not problems and man.get("profdata_sha256") != file_sha256(prof):
            problems.append("profile file damaged")
        if problems:
            return None, "no PGO: the shipped profile does not match (" + "; ".join(problems) + ")"
        note = "" if float(man.get("split_mb", ctx.split_mb)) == float(ctx.split_mb) else \
            f" (trained with {man.get('split_mb')} MB parts: shared helpers lose their counters)"
        return prof, f"PGO profile {prof.name} ({man['profdata_sha256'][:12]}, {man.get('coverage', '?')}){note}"
    except (OSError, ValueError, KeyError, TypeError) as e:
        return None, f"no PGO: unreadable profile manifest ({e!r})"


def log_line(ctx: Ctx, phase: str, msg: str) -> None:
    print(f"  {msg}", flush=True)
    logs = ctx.work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    with open(logs / f"{phase}.log", "a", encoding="utf-8") as f:
        f.write(f"\n{msg}\n")


def compile_step(ctx: Ctx, envs: Dict[str, str]) -> Step:
    if ctx.build_cmd:
        # Custom command: {export} {build} {jobs} {repo} {toolchain_dir} are substituted; runs with cmd.exe.
        jobs = ctx.jobs or ram_jobs()
        cmd = ctx.build_cmd.format(export=ctx.export, build=ctx.build, jobs=jobs, repo=REPO,
                                   toolchain_dir=ctx.toolchain_dir or "")
        return Step(f"custom command, {jobs} jobs", ["cmd.exe", "/d", "/s", "/c", cmd], envs)
    if ctx.toolchain == "portable":
        # jobs 0 = as many as logical cores and free RAM allow (build_modules.py's memory-aware scheduler)
        argv = PS + [str(REPO / "recomp/scripts/build-modules.ps1"), "-Export", str(ctx.export),
                     "-Target", "launcher", "-Jobs", str(ctx.jobs), "-BuildRoot", str(ctx.build),
                     "-Toolchain", "portable", "-ToolchainDir", str(ctx.toolchain_dir), "-SplitMB", str(ctx.split_mb)]
        isa = resolve_isa(ctx.isa)
        if isa != "x86-64":
            argv += ["-Isa", isa]
        prof, _ = pgo_choice(ctx)
        if prof:
            argv += ["-ProfileUse", str(prof)]
        how = (f"units split at {ctx.split_mb:g} MB" if ctx.split_mb else "whole units") + f", {isa}" + (", PGO" if prof else "")
        jobs = f"{ctx.jobs} jobs" if ctx.jobs else "jobs by cores and free RAM"
        return Step(f"portable toolchain (clang-cl + lld-link), {how}, {jobs}", argv, envs)
    jobs = ctx.jobs or ram_jobs()
    argv = PS + [str(REPO / "recomp/scripts/build-modules.ps1"), "-Export", str(ctx.export),
                 "-Target", "launcher", "-Jobs", str(jobs), "-BuildRoot", str(ctx.build)]
    return Step(f"clang-cl + Ninja (VS Build Tools), {jobs} jobs", argv, envs)


def ram_jobs() -> int:
    """The emitted CMakeLists' rule for whole units: one compile per 8 GB of RAM."""
    return max(1, rq.ram_total() // (8 << 30))


# ---------------------------------------------------------------------------
# Phases
# ---------------------------------------------------------------------------

def phases(ctx: Ctx) -> List[Phase]:
    P = ctx.profile
    tid, T, D, F = ctx.tid, ctx.work, ctx.dest, ctx.fork
    keys = ["--keys", str(ctx.keys)] if ctx.keys else []
    tkeys = ["--title-keys", str(ctx.title_keys)] if ctx.title_keys else []
    envs = ctx.env_scripts()
    files = ctx.input_files()
    base_src, upd_src = ctx.source("base"), ctx.source("update")
    base_nsp, upd_nsp = ctx.decompressed_nsp(base_src), ctx.decompressed_nsp(upd_src)
    patches = [x for p in P["baked_patches"] for x in ("-p", str(REPO / p["json"]))]
    gui = ctx.exporter == "gui"
    # Baked main: a LayeredExeFS mod of the isolated suyu (GUI exporter) or a plain file (--main of aot_export).
    load_main = (ctx.suyu_user / "load" / tid / "launcher-baked" / "exefs" / "main") if gui else T / "baked" / "main"
    pkg = ctx.export / "pkg"
    L: List[Phase] = []
    make_input = Step("create input/", fn=lambda: ctx.input_dir.mkdir(parents=True, exist_ok=True))

    # 1. prepare: small NSZ/XCZ (update) and links; the compressed base goes separately (heavy)
    steps, gb = [], 0.0
    if ctx.dump_dir() == ctx.input_dir:
        steps.append(make_input)
        for a in files:
            src = Path(a["path"])
            if a["format"] in ("nsz", "xcz"):
                if src == base_src:
                    continue
                steps += nsz_step(ctx, src)
                gb += a["size"] * 2 / (1 << 30)
            elif a["format"] in ("nsp", "xci", "nca", "tik"):
                steps.append(Step(f"link {src.name} into input/", fn=lambda s=src: link_or_copy(s, ctx.input_dir / s.name)))
    L.append(Phase("prepare", "Prepare the input (update and links)", "cheap", steps,
                   [upd_nsp] if upd_nsp and steps else [], gb, 0.5,
                   skip="" if steps else "the user's folder works as-is"))

    # 2. extract ExeFS + main.elf + target.json, verified against the profile
    L.append(Phase("extract", "Extract the 1.1.1 ExeFS and verify it", "cheap", [
        Step("ExeFS, main.elf and target.json", [PY, str(REPO / "tools/extract/extract.py"), "--dump", str(ctx.dump_dir()),
             "--out", str(ctx.data), "--title-id", tid, "--expect-version", P["version"]["display"]] + keys + tkeys),
        Step("compare SHA-256 and build ids with the profile", fn=lambda: verify_exefs(ctx)),
    ], [ctx.data / "exefs/main", ctx.data / "main.elf", ctx.data / "target.json"], 0.25, 0.5))

    # 3. static analysis roots (no Ghidra)
    L.append(Phase("roots", "Extra roots for the recompiler", "medium", [
        Step("code_targets (eh_frame, RTTI, relocs, jump tables)", [PY, str(REPO / "tools/analysis/code_targets.py"),
             str(ctx.data / "main.elf"), "--out-dir", str(T / "analysis")]),
        Step("main.roots", [PY, str(REPO / "recomp/scripts/make-roots.py"), "--json", str(T / "analysis/all_code_targets.json"),
             "--out", str(T / "roots"), "--text-size", P["roots"]["text_size"]]),
    ], [T / "roots" / f"{P['roots']['module']}.roots"], 0.1, 3, "Duration not measured in the launcher flow."))

    # 4. bake the patches (unlocked FPS) into main (the exporter's input)
    L.append(Phase("bake", "Bake the patches into main", "cheap", [
        Step("check original bytes", [PY, str(REPO / "tools/patch/apply_patches.py")] + patches + ["--in", str(ctx.data / "exefs/main"), "--check"]),
        Step("write baked main", [PY, str(REPO / "tools/patch/apply_patches.py")] + patches +
             ["--in", str(ctx.data / "exefs/main"), "--out", str(load_main), "--title-id", tid]),
    ], [load_main], 0.07, 0.2))

    # 5. decompressed base game: the GUI exporter opens the base NSP; the headless RomFS writer reads an NSZ's
    #    NCZ stream directly, so with --exporter cli only an XCZ base is decompressed.
    comp_base = bool(base_src) and base_src.suffix.lower() in (".nsz", ".xcz")
    need_base = comp_base and (gui or base_src.suffix.lower() == ".xcz")
    L.append(Phase("base", "Decompress the base game", "heavy",
                   [make_input] + nsz_step(ctx, base_src) if need_base else [],
                   [base_nsp] if need_base else [], 6.1, 6,
                   "Needed by the GUI exporter (rom_path) and for an XCZ base. Time not measured.",
                   skip="" if need_base else ("the base game is not compressed" if not comp_base else
                                              "the RomFS is read from the NSZ directly")))

    # 6. isolated exporter (GUI only): the fork's suyu copied into work/, with its own user folder
    L.append(Phase("suyu", "Prepare the exporter (isolated suyu + update in its NAND)", "medium", [
        # The exporter's identity is part of the fingerprint (and, chained, of export/compile/assemble).
        Step(f"exporter {file_identity(F / 'build/suyu/bin/suyu.exe')}", fn=lambda: None),
        Step("fork binaries", PS + [str(REPO / "recomp/scripts/setup-portable-suyu.ps1"), "-Dest", str(ctx.suyu), "-BinOnly"], envs),
        Step(f"junction {ctx.suyu / 'user'} -> {ctx.suyu_user}", fn=lambda: junction(ctx.suyu / "user", ctx.suyu_user)),
        Step("keys + update installed in the isolated NAND",
             [PY, str(REPO / "recomp/scripts/stage-suyu-userdata.py"), "--prod-keys", str(ctx.keys or Path.home() / ".switch/prod.keys"),
              "--title-keys", str(ctx.title_keys or Path.home() / ".switch/title.keys"),
              "--nsp", str(upd_nsp)] + (["--nsp", str(base_nsp)] if base_nsp else []) +
             ["--install-update", str(upd_nsp), "--mk8r", str(F)],
             {"APPDATA": str(T / "appdata")}),
    ], [ctx.suyu / "suyu.exe", ctx.suyu_user / "keys/prod.keys"] if gui else [], 0.4, 1.5,
        "stage-suyu-userdata.py and install-nsp.py write into %APPDATA%\\suyu: APPDATA is redirected for that process only.",
        skip="" if gui else "the headless exporter needs no emulator, keys or NAND"))

    # 7. export the C (the fork's emitter with the profile's flags) and the effective RomFS
    em = P["emitter"]
    if gui:
        exp_args = PS + [str(REPO / "recomp/scripts/export-pla.ps1"), "-Backend", em["backend"], "-Format", "source",
                         "-Tag", "launcher", "-Rom", str(base_nsp), "-OutDir", str(ctx.export), "-Roots", str(T / "roots"),
                         "-SuyuExe", str(ctx.suyu / "suyu.exe"), "-McpPort", str(ctx.mcp_port)]
        if em.get("shl_imm"):
            exp_args.append("-ShlImm")
        exp_steps = [Step("AOT export over MCP (isolated suyu)", exp_args, {**envs, **em["env"]})]
    else:
        aot = ctx.exporter_exe
        # aot_export writes a fresh export\pkg (an existing one is replaced) with the same files as the GUI export.
        exp_args = [str(aot), "--exefs", str(ctx.data / "exefs"), "--main", str(load_main), "--out", str(pkg),
                    "--roots", str(T / "roots"), "--backend", em["backend"], "--title", export_title(ctx),
                    "--source", str(base_src or "")]
        if em.get("shl_imm"):
            exp_args.append("--shl-imm")
        # RomFS: the update's BKTR patch over the base, from the dump (an NSZ base is streamed, not decompressed).
        rom_dumps = ["--dump", str(ctx.dump_dir())]
        if base_src and not need_base and base_src.parent != ctx.dump_dir():
            rom_dumps += ["--dump", str(base_src)]
        exp_steps = [
            Step(f"exporter {file_identity(aot)}", fn=lambda: None),
            Step("AOT export (headless aot_export)", exp_args, {**em["env"]}),
            Step("RomFS with the update applied (romfs.bin)",
                 [PY, str(REPO / "tools/extract/romfs_bktr.py")] + rom_dumps +
                 ["--out", str(pkg / "exefs/romfs.bin"), "--title-id", tid] + keys + tkeys),
        ]
    chk = []
    for p in P["baked_patches"]:
        chk.append(Step(f"baked patches present in the C ({p['expected']})",
                        [PY, str(REPO / "recomp/scripts/check-baked-patches.py"), "-p", str(REPO / p["json"]),
                         "--src", str(pkg / "aot_cache/exefs/main/src")]))
    c_gb = sum(m["c_mb"] for m in P["modules"].values()) / 1024
    L.append(Phase("export", "Export the recompiled C", "heavy",
                   exp_steps + [Step("emitter options recorded in the manifest", fn=lambda: verify_emitter(ctx))] + chk,
                   [pkg / "aot_cache/aot_manifest.json", pkg / "exefs/romfs.bin"], c_gb + P["romfs"]["gb"], 5,
                   "Opens a suyu window (--exporter gui). Do not run it with the game open." if gui else
                   "Headless: C in ~1 min, then the RomFS (~6 GB written; 2-3 min, bound by the disk)."))

    # 8. compile the DLLs (pluggable: --toolchain vs|portable or a custom command with --build-cmd)
    fast = ctx.toolchain == "portable" and ctx.split_mb > 0 and not ctx.build_cmd
    mins = sum(m.get("min_split", m["min"]) if fast else m["min"] for m in P["modules"].values())
    pgo_msg = pgo_choice(ctx)[1]
    L.append(Phase("compile", "Compile the modules (DLL)", "heavy",
                   [Step(pgo_msg, fn=lambda: log_line(ctx, "compile", pgo_msg)), compile_step(ctx, envs)],
                   [ctx.build / m / info["dll"] for m, info in P["modules"].items()], 7.0, mins,
                   "Resumable: only what is missing gets recompiled. Portable toolchain: units split into ~8 MB parts, "
                   "as many compiles as cores and free RAM allow (~13 min on 32 threads / 32 GB; whole units: ~80 min)."))

    # (The former phase 9 "romfs", romfs.bin -> folder, no longer exists: the nnshim reads exefs\romfs.bin
    #  directly with "romfs_bin": "auto"; see docs/port/ROMFS.md.)

    # 10. assemble the game folder
    mods_root = D / "mods"
    mods_tid = mods_root / tid
    # Prebuilt recomp mods (shipped in the setup's platform package under build/mods-recomp/<id>/): when all of
    # them are present, make-dist.ps1 runs with -Mods @() (no clang-cl/VS needed) and they are copied as-is.
    prebuilt = REPO / "build/mods-recomp"
    mods_rc = P["dist"]["mods_recomp"]
    use_prebuilt = bool(mods_rc) and all((prebuilt / m / "mod.json").is_file() for m in mods_rc)
    if use_prebuilt:
        def q(s) -> str:
            return "'" + str(s).replace("'", "''") + "'"
        cmd = (f"& {q(REPO / 'recomp/scripts/make-dist.ps1')} -Recomp {q(ctx.build)} -Export {q(pkg)} "
               f"-Dest {q(D)} -Mods @()")
        steps = [Step("dist base (DLL, exefs, user)", PS[:-1] + ["-Command", cmd], envs)]
        for m in mods_rc:
            steps.append(Step(f"mod {m} (prebuilt)", fn=lambda m=m: copy_prebuilt_mod(prebuilt / m, mods_tid / m)))
    else:
        steps = [Step("dist base (DLL, exefs, user, repo mods)", PS + [str(REPO / "recomp/scripts/make-dist.ps1"), "-Recomp", str(ctx.build),
                      "-Export", str(pkg), "-Dest", str(D), "-Mods", ",".join(mods_rc)], envs)]
    # Mods generated from the user's main.elf and compiled here (never shipped prebuilt: they contain lifted game
    # code). islands: tools/islands/build_islands.py with the portable toolchain.
    for m in P["dist"].get("mods_local", []):
        steps.append(Step(f"mod {m} (generated from main.elf and compiled here)", fn=lambda m=m: build_local_mod(ctx, m, mods_tid / m)))
    # The game's executable is plahost (tools/port/plahost, docs/port/HOST.md): no emulator in the game folder.
    exe_src = PLAHOST_EXE
    steps.append(Step(f"{P['dist']['exe']} (plahost {file_identity(exe_src)})",
                      fn=lambda: ((D / P["dist"]["exe"]).unlink(missing_ok=True), shutil.copy2(exe_src, D / P["dist"]["exe"]))))
    if P["dist"].get("icon_from_dump"):
        steps.append(Step("game icon (from the dump)", [PY, str(REPO / "recomp/scripts/make-pla-exe.py"), "--exe", str(D / P["dist"]["exe"]),
                          "--ico", str(D / "PLA.ico"), "--dump", str(ctx.dump_dir())]))
    steps += [Step(f"{P['dist']['ini']}", fn=lambda: write_ini(ctx)),
              Step("free fonts for the nnshim", [PY, str(REPO / "tools/port/nnshim/extract_suyu_fonts.py"),
                   "--suyu", str(F / "third_party/suyu"), "--out", str(D / "fonts")])]
    for m in P["dist"]["mods_platform"]:
        dst = mods_tid / m["id"]
        src = REPO / "build/port" / m["id"] / m["dll"]
        steps.append(Step(f"mod {m['id']}: {m['dll']}", fn=lambda s=src, d=dst: (d.mkdir(parents=True, exist_ok=True), shutil.copy2(s, d))))
        for x in m.get("extra", []):
            steps.append(Step(f"mod {m['id']}: {x} (optional)", fn=lambda x=x, i=m["id"], d=dst: copy_extra(x, i, d, F)))
        if m["mod_json"] in ("nnshim_template", "gen_mod_json"):  # "gen_mod_json": old profile name
            steps.append(Step(f"mod {m['id']}: mod.json (hook template + config) and controls.json",
                              fn=lambda d=dst: nnshim_mod_json(d, D)))
            steps.append(Step(f"mod {m['id']}: key icons (key_prompts.bin)",
                              [PY, str(REPO / "tools/port/nnshim/key_prompts/gen_key_prompts.py"), "--romfs",
                               str(D / "exefs/romfs.bin"), "--controls", str(dst / "controls.json"), "--out",
                               str(dst / "key_prompts.bin")]))
        elif m["mod_json"] in ("template", "gen_tables"):  # "gen_tables": old profile name
            extra = f"shader_cache_dir={(D / 'user/cache/nvnvk-shaders').as_posix()}" + (";" + m["extra"] if m.get("extra") else "")
            steps.append(Step(f"mod {m['id']}: mod.json (hook template + config)",
                              fn=lambda d=dst, mode=m.get("mode", "exclusive"), x=extra: nvnvk_mod_json(
                                  d, mode, (D / "user/log/nvnvk").as_posix(), (D / "user/cache/nvnvk-pipelines.bin").as_posix(), x)))
    # No PLAY.bat: PLA.exe needs no environment (mods\ next to it; renderer=nvnvk in PLA.ini and by default).
    # make-dist.ps1 removes the .bat launchers and suyu-cmd.exe left by older assemblies.
    dll_gb = sum(m["dll_mb"] for m in P["modules"].values()) / 1024
    L.append(Phase("assemble", "Assemble the game folder", "medium", steps, [D / P["dist"]["exe"], D / P["dist"]["ini"]], dll_gb + 0.3, 2,
                   "romfs.bin goes in as a hard link if work/ and dest/ are on the same volume (otherwise +6 GB). "
                   "The nnshim reads it directly (romfs_bin = auto): the RomFS folder is not extracted."))

    # 11. verify
    L.append(Phase("verify", "Verify the folder and write the manifest", "cheap",
                   [Step("check files and write launcher-manifest.json", fn=lambda: manifest(ctx))],
                   [D / "launcher-manifest.json"], 0, 0.2))
    return L


# ---------------------------------------------------------------------------
# State and execution
# ---------------------------------------------------------------------------

class State:
    def __init__(self, work: Path):
        self.path = work / "state.json"
        self.doc = json.loads(self.path.read_text(encoding="utf-8")) if self.path.is_file() else {"phases": {}}

    def done(self, f: Phase, fp: str) -> bool:
        e = self.doc["phases"].get(f.id)
        return bool(e and e.get("state") == "done" and e.get("fingerprint") == fp and all(p.exists() for p in f.outputs))

    def mark(self, f: Phase, state: str, fp: str, **kw) -> None:
        self.doc["phases"][f.id] = {"state": state, "fingerprint": fp, "date": time.strftime("%Y-%m-%d %H:%M:%S"), **kw}
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.doc, indent=1, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, self.path)


class Lock:
    def __init__(self, work: Path):
        self.path = work / "launcher.lock"

    def __enter__(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        if self.path.exists():
            pid = self.path.read_text().strip()
            alive = pid and f'"{pid}"' in subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/FO", "CSV", "/NH"],
                                                         capture_output=True, text=True).stdout
            if alive:
                raise SystemExit(f"Another launcher (PID {pid}) is using {self.path.parent}.")
        self.path.write_text(str(os.getpid()))
        return self

    def __exit__(self, *a):
        self.path.unlink(missing_ok=True)


def run_step(p: Step, log: Path) -> None:
    print(f"  - {p.desc}", flush=True)
    event("step", desc=p.desc, log=str(log))
    if p.fn:
        p.fn()
        return
    env = {**os.environ, **p.env}
    with open(log, "a", encoding="utf-8", errors="replace") as lf:
        lf.write(f"\n$ {p.text()}\n")
        lf.flush()
        r = subprocess.run(p.argv, env=env, stdout=lf, stderr=subprocess.STDOUT, cwd=str(REPO))
    if r.returncode != 0:
        raise RuntimeError(f"'{p.desc}' ended with code {r.returncode} (log: {log})")


def final_size_gb(ctx: Ctx) -> float:
    P = ctx.profile
    return sum(m["dll_mb"] for m in P["modules"].values()) / 1024 + P["romfs"]["gb"] + 0.3


def plan(ctx: Ctx, F: List[Phase], st: State) -> None:
    builder = "custom command" if ctx.build_cmd else ctx.toolchain + (f" ({ctx.toolchain_dir})" if ctx.toolchain == "portable" else "")
    print("\nPLAN (profile %s)\n  work:    %s\n  dest:    %s\n  fork:    %s\n  compile: %s" % (
        ctx.profile["id"], ctx.work, ctx.dest, ctx.fork, builder))
    total_gb = total_min = 0.0
    previous = ""
    ev = []
    for i, f in enumerate(F, 1):
        h = f.fingerprint(previous)
        previous = h
        mark = "DONE" if st.done(f, h) else ("skipped" if f.skip else "pending")
        ev.append({"id": f.id, "title": f.title, "cost": f.cost, "minutes": f.minutes, "gb": round(f.gb, 2),
                   "state": mark.lower(), "skip": f.skip, "steps": [p.desc for p in f.steps], "note": f.note})
        print(f"\n[{i:2}] {f.id:<9} {f.title}  ({f.cost}; ~{f.minutes:g} min; ~{f.gb:.1f} GB)  -> {mark}")
        if f.skip:
            print(f"      skipped: {f.skip}")
            continue
        total_gb += f.gb
        total_min += f.minutes
        for p in f.steps:
            print(f"      $ {p.text()}")
        for s in f.outputs:
            print(f"      => {s}")
        if f.note:
            print(f"      note: {f.note}")
    final = final_size_gb(ctx)
    print(f"\nESTIMATED TOTAL: ~{total_min:.0f} min and ~{total_gb:.0f} GB written (peak; --clean frees ~10 GB at the end).")
    print(f"  Game folder when done: ~{final:.1f} GB (DLL + romfs.bin; no RomFS folder).")
    event("plan", phases=ev, total_min=round(total_min, 1), peak_gb=round(total_gb, 1), final_gb=round(final, 1),
          work=str(ctx.work), dest=str(ctx.dest), jobs=ctx.jobs,
          toolchain="custom" if ctx.build_cmd else ctx.toolchain, toolchain_dir=str(ctx.toolchain_dir or ""))


def run(ctx: Ctx, F: List[Phase], st: State, allow_heavy: bool, only: List[str], force: List[str]) -> int:
    logs = ctx.work / "logs"
    logs.mkdir(parents=True, exist_ok=True)
    previous = ""
    for f in F:
        h = f.fingerprint(previous)
        previous = h
        if only and f.id not in only:
            continue
        if f.skip:
            continue
        if st.done(f, h) and f.id not in force:
            print(f"[{f.id}] done (resumed later)")
            event("phase", id=f.id, state="done", previous=True)
            continue
        if f.cost == "heavy" and not allow_heavy:
            event("heavy", id=f.id, title=f.title, minutes=f.minutes, gb=f.gb)
            print(f"\n[{f.id}] is HEAVY ({f.title}, ~{f.minutes:g} min, ~{f.gb:.1f} GB). Stopping here.\n"
                  "  Run again with --allow-heavy to continue (it resumes from this phase).")
            return 3
        print(f"\n[{f.id}] {f.title} ({f.cost})", flush=True)
        t0 = time.time()
        st.mark(f, "running", h)
        event("phase", id=f.id, state="running", log=str(logs / f"{f.id}.log"))
        try:
            for p in f.steps:
                run_step(p, logs / f"{f.id}.log")
            missing = [str(s) for s in f.outputs if not s.exists()]
            if missing:
                raise RuntimeError("not generated: " + ", ".join(missing))
        except Exception as e:  # noqa: BLE001
            st.mark(f, "failed", h, error=str(e))
            event("phase", id=f.id, state="failed", error=str(e), log=str(logs / f"{f.id}.log"))
            print(f"\nERROR in [{f.id}]: {e}\n  Fix it and run again: it resumes from this phase.")
            return 1
        st.mark(f, "done", h, seconds=round(time.time() - t0))
        event("phase", id=f.id, state="done", seconds=round(time.time() - t0, 1))
        print(f"  ok in {time.time() - t0:.0f} s", flush=True)
    exe = ctx.dest / ctx.profile["dist"]["exe"]
    print("\nDone." + (f" Play with {exe}" if exe.exists() else ""))
    return 0


def main(argv=None) -> int:
    idf.utf8_console()
    ap = argparse.ArgumentParser(description="From your own dump to the port's playable folder.")
    ap.add_argument("--input", type=Path, action="append", required=True, help="NSP/NSZ/XCI/XCZ or folder (can be repeated)")
    ap.add_argument("--dest", type=Path, required=True, help="game folder to create")
    ap.add_argument("--work", type=Path, help="work folder (default <dest>-work; short path, <= 40 characters)")
    ap.add_argument("--profile", default="pla-1.1.1")
    ap.add_argument("--fork", type=Path, default=Path(os.environ.get("PLA_RECOMP_ROOT", r"D:\PLARecomp")) / "mk8-recomp",
                    help="fork checkout with build\\suyu\\bin (the install-time exporter)")
    ap.add_argument("--keys", type=Path)
    ap.add_argument("--title-keys", type=Path)
    ap.add_argument("--jobs", type=int, default=0,
                    help="parallel compilations (default: portable = by cores and free RAM; vs = RAM/8 GB)")
    ap.add_argument("--split-mb", type=float, default=8.0,
                    help="portable toolchain: split the generated units into ~N MB parts (0 = whole units, old build)")
    ap.add_argument("--isa", choices=("auto", "x86-64", "x86-64-v2", "x86-64-v3"), default="x86-64",
                    help="portable toolchain: CPU level the DLLs are compiled for (auto = this CPU's; the DLLs then "
                         "need a CPU of that level, the compile phase reruns when the level changes)")
    ap.add_argument("--exporter", choices=("cli", "gui"), default="cli",
                    help="cli: headless aot_export.exe + romfs_bktr.py (default); gui: the fork's suyu.exe over MCP "
                         "(opens a window, needs the isolated NAND; fallback kept for one release)")
    ap.add_argument("--mcp-port", type=int, default=9782, help="MCP port of the GUI exporter (--exporter gui)")
    ap.add_argument("--dry-run", action="store_true", help="only requirements, identification and plan")
    ap.add_argument("--allow-heavy", action="store_true", help="also run the heavy phases (export, compile, base)")
    ap.add_argument("--only", default="", help="comma-separated phases")
    ap.add_argument("--force", default="", help="redo these phases even if they are recorded as done")
    ap.add_argument("--clean", action="store_true", help="when finished, delete the exported C and the objects (~10 GB)")
    ap.add_argument("--toolchain", choices=("auto", "vs", "portable"), default="auto",
                    help="compiler for the DLLs: vs (VS Build Tools + LLVM), portable (tools/toolchain) or auto")
    ap.add_argument("--toolchain-dir", type=Path,
                    help="portable package folder (default <PLA_RECOMP_ROOT>\\toolchain-portable\\recomp-toolchain)")
    ap.add_argument("--build-cmd", default="",
                    help="custom command for the compile phase (cmd.exe); accepts {export} {build} {jobs} {repo} {toolchain_dir}")
    ap.add_argument("--events", action="store_true", help="also emit '@@ {json}' lines (for the graphical installer)")
    a = ap.parse_args(argv)
    global EVENTS
    EVENTS = a.events

    profile = idf.load_profile(a.profile)
    work = (a.work or a.dest.with_name(a.dest.name + "-work")).resolve()
    tc, tc_dir = choose_toolchain(a.toolchain, a.toolchain_dir)
    ctx = Ctx(profile, [e.resolve() for e in a.input], work, a.dest.resolve(), a.fork.resolve(),
              a.keys, a.title_keys, a.jobs, a.mcp_port,
              tc, tc_dir.resolve() if tc == "portable" else None, a.build_cmd, a.split_mb, a.exporter, a.isa)

    print("== Dump identification")
    ctx.ident = idf.identify(ctx.inputs, profile, a.keys, a.title_keys)
    print(idf.report(ctx.ident))
    event("ident", state=ctx.ident["state"], game=ctx.ident.get("game"), warnings=ctx.ident.get("warnings", []),
          files=[{"path": x["path"], "format": x["format"], "size": x["size"]} for x in ctx.ident["files"]],
          base=ctx.ident.get("base"), update=ctx.ident.get("update"), differences=ctx.ident.get("differences", []),
          build_id=(ctx.ident.get("exefs") or {}).get("main", {}).get("build_id"))
    F = phases(ctx)
    space = sum(f.gb for f in F if not f.skip)
    print("\n== Requirements")
    reqs = rq.check(profile, work, ctx.dest, ctx.fork, a.keys,
                    any(x["format"] in ("nsz", "xcz", "ncz") for x in ctx.ident["files"]), space,
                    "custom" if ctx.build_cmd else ctx.toolchain, ctx.toolchain_dir, ctx.exporter_exe, PLAHOST_EXE)
    print(rq.report(reqs))
    for r in reqs:
        event("req", name=r.name, ok=r.ok, detail=r.detail, phases=r.phases, help=r.help)
    if ctx.exporter == "gui" and len(str(work / "export")) > 60:
        print(f"  WARNING long work path ({work}): the exporter recommends <= 70 characters in its output.")

    st = State(work)
    plan(ctx, F, st)
    if a.dry_run:
        rc = 0 if ctx.ident["state"] == "supported" else 2
        event("end", rc=rc, dry_run=True)
        return rc
    if ctx.ident["state"] != "supported":
        print(f"\nThe dump is not ready for this profile (state: {ctx.ident['state']}). Nothing is done.")
        event("end", rc=2, reason=f"dump: {ctx.ident['state']}")
        return 2
    hard = [r for r in reqs if r.ok is False and not r.phases]
    if hard:
        print("\nBasic requirements are missing: " + ", ".join(r.name for r in hard))
        event("end", rc=2, reason="missing requirements: " + ", ".join(r.name for r in hard))
        return 2
    with Lock(work):
        rc = run(ctx, F, st, a.allow_heavy, [s for s in a.only.split(",") if s], [s for s in a.force.split(",") if s])
        if rc == 0 and a.clean:
            for d in (ctx.export / "pkg/aot_cache", ctx.build):
                print(f"cleaning {d}")
                event("clean", path=str(d))
                shutil.rmtree(d, ignore_errors=True)
    event("end", rc=rc, dest=str(ctx.dest), exe=str(ctx.dest / profile["dist"]["exe"]),
          exe_args=profile["dist"].get("exe_args_play", ""))
    return rc


if __name__ == "__main__":
    sys.exit(main())
