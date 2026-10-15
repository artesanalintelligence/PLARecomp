"""Per-function evidence collection for the triage.

Sources (all regenerable, see tools/ghidra/README.md):
  * tools/ghidra/out/main_functions_named.tsv  names (RTTI/vtables, PLT, symbols)
  * tools/ghidra/out/strings.tsv               strings and the functions that use them
  * tools/ghidra/out/rtti.json                 vtables with and without RTTI
  * tools/triage/out/lua_map.csv               identified Lua 5.3.4 functions
  * data/main.elf                              call graph (BL) and code patterns
"""
from __future__ import annotations

import bisect
import csv
import json
import re
from collections import defaultdict
from dataclasses import dataclass, field

import numpy as np

from common import GHIDRA_OUT, Image, words

OUT_DIR = __import__("pathlib").Path(__file__).resolve().parent / "out"


@dataclass
class Func:
    addr: int
    size: int
    name: str
    demangled: str
    sources: str
    votes: dict = field(default_factory=lambda: defaultdict(float))
    why: dict = field(default_factory=dict)      # label -> main source
    fixed: str | None = None                     # definitive label (strong evidence)
    fixed_conf: str = ""
    fixed_src: str = ""
    real_name: str | None = None
    vtable_group: str | None = None              # vtable (with or without RTTI) it belongs to

    def vote(self, label: str, w: float, src: str):
        self.votes[label] += w
        self.why.setdefault(label, src)


def load_funcs():
    fs = []
    with open(GHIDRA_OUT / "main_functions_named.tsv", encoding="utf-8") as fh:
        next(fh)
        for line in fh:
            p = line.rstrip("\n").split("\t")
            fs.append(Func(int(p[0], 16), int(p[1]), p[2], p[3] if len(p) > 3 else "",
                           p[4] if len(p) > 4 else ""))
    fs.sort(key=lambda f: f.addr)
    return fs


# ---------------------------------------------------------------------------
# Rules: class prefix / namespace -> label
# ---------------------------------------------------------------------------
CLASS_RULES = [
    (r"^\(anonymous namespace\)::grpc", "lib-open:grpc"),
    (r"^\(anonymous namespace\)::.*hk", "lib-closed:havok"),
    (r"^(grpc|grpc_core|grpc_impl|grpc_\w+|tsi|ExecCtx\w*|grpc)(::|$)", "lib-open:grpc"),
    (r"^grpc", "lib-open:grpc"),
    (r"^google::protobuf::", "lib-open:protobuf"),
    (r"^google::rpc::", "lib-closed:npln"),
    (r"^re2::", "lib-open:re2"),
    (r"^absl::", "lib-open:abseil"),
    (r"^(cricket|rtc|webrtc)::", "lib-open:webrtc"),
    (r"^(nn::pia|pead)::", "lib-closed:pia"),
    (r"^(nn::npln|nn::async)::", "lib-closed:npln"),
    (r"^(nn::ui2d|nn::font|nn::vfx|nn::g3d|nn::atk|nw)::", "lib-closed:nintendoware"),
    (r"^nn::gfx::", "lib-closed:nn-gfx"),
    (r"^nn::", "sdk-glue"),
    (r"^nlib", "lib-closed:nlib"),
    (r"^(hk|Hk)", "lib-closed:havok"),
    (r"^(NavMesh|TreeOverlaps|RemoveOverla|IConvexOverl|CreateUserEd|QueryAndUpda|PreStepTask|PostStepTask)", "lib-closed:havok"),
    (r"^AK::", "lib-closed:wwise"),
    (r"^std::__1::", "lib-open:libc++"),
    (r"^sol::", "lib-open:sol2"),
    (r"^boost::", "lib-open:boost"),
    (r"^flatbuffers::", "lib-open:flatbuffers"),
    (r"^rapidjson::", "lib-open:rapidjson"),
]
CLASS_RULES = [(re.compile(p), l) for p, l in CLASS_RULES]


def label_for_class(cls: str):
    for rx, lab in CLASS_RULES:
        if rx.search(cls):
            return lab
    return None


# Strings -> label (evaluated in order; the first one that matches wins)
STRING_RULES = [
    (r"localrombuild[\\/]+program", "game"),
    (r"sol::(detail|usertype|as_container|stack|function|basic_|protected|optional|types|constructor|destructor|unique_usertype)", "lib-open:sol2"),
    (r"com_google_grpc_base|grpc_core|grpc_impl|\bgrpc[._]|GRPC_|grpc-|/grpc/|\bgpr_|chttp2|\bHTTP2|census", "lib-open:grpc"),
    (r"third_party/protobuf|google/protobuf|google\.protobuf|protobuf", "lib-open:protobuf"),
    (r"net_nintendo_npln_proto|proto/\w+/v\d|nn::npln|\bnpln\b|NPLN|nn::async", "lib-closed:npln"),
    (r"external/openssl|OpenSSL|\bSSL_|ssl3_|tls1_|\bX509|ASN1|\bEVP_|\bRSA_|\bBN_|\bEC_|crypto/", "lib-open:openssl"),
    (r"re2/|\bRE2\b", "lib-open:re2"),
    (r"external/upb|upb/", "lib-open:upb"),
    (r"\babsl\b|absl::", "lib-open:abseil"),
    (r"webrtc|rtc_base", "lib-open:webrtc"),
    (r"libcurl|curl_|CURLOPT|curl\.haxx|CURLE_|Curl_", "lib-open:curl"),
    (r"hk2020|[Hh]avok|Stable_Package_Build_nx64|\bhk[A-Z]\w+|\bhknp|\bhkai|HK_[A-Z]", "lib-closed:havok"),
    (r"AK::|Wwise|\bAk[A-Z][a-z]\w+|\bAK_\w+|^Recorder: |^AK |SoundEngine|AkMonitor|SpatialAudio", "lib-closed:wwise"),
    (r"(?i:oodle)|newlz|lzblw|\bOO_|rrlz", "lib-closed:oodle"),
    (r"nn::pia|\bPia\b|pead::|PiaSession|PiaLan|PiaLocal|PiaNpln", "lib-closed:pia"),
    (r"nlib_|\bnlib\b", "lib-closed:nlib"),
    (r"boost::", "lib-open:boost"),
    (r"zlib|incorrect header check|invalid distance too far|inflate|deflate", "lib-open:zlib"),
    (r"\bnvn[A-Z]\w+", "lib-closed:nvn"),
    (r"nn::(ui2d|font|vfx|g3d|atk)|\bnw::", "lib-closed:nintendoware"),
    (r"nn::gfx", "lib-closed:nn-gfx"),
    (r"\bgfl::|\bgflnet::|\bgamesys::|\bfield::|\bcontents::|\bevent_script::|\bcommon::lua", "game"),
]
STRING_RULES = [(re.compile(p), l) for p, l in STRING_RULES]


def label_for_string(s: str):
    for rx, lab in STRING_RULES:
        if rx.search(s):
            return lab
    return None


def collect(fs, img: Image):
    addrs = [f.addr for f in fs]
    idx_of = {f.addr: i for i, f in enumerate(fs)}

    def owner(a):
        i = bisect.bisect_right(addrs, a) - 1
        if i >= 0 and fs[i].addr <= a < fs[i].addr + max(fs[i].size, 4):
            return i
        return None

    # 1) names
    for f in fs:
        n = f.name
        if "plt" in f.sources.split(","):
            f.fixed, f.fixed_conf, f.fixed_src = "sdk-glue", "high", "plt"
            f.real_name = f.demangled or n
            continue
        if n.startswith(("_INIT_", "_FINI_")):
            f.fixed, f.fixed_conf, f.fixed_src = "compiler-generated", "high", "init_array"
            continue
        if n in ("_DT_INIT", "_DT_FINI") or n.startswith(("nndetailRo", "__nnDetail")) \
                or n.endswith("SigloMiddlewareInfo"):
            f.fixed, f.fixed_conf, f.fixed_src = "sdk-glue", "high", "symbol"
            continue
        cls = None
        m = re.match(r"^(.*)::(thunk\d*_)?vf\d+$", n)
        if m:
            cls = m.group(1).replace(";", ",")
        elif f.demangled:
            cls = f.demangled
            f.real_name = f.demangled
        if cls:
            lab = label_for_class(cls)
            if lab:
                f.vote(lab, 3.0, "rtti/name")
            # vtables without RTTI are named vtable_<addr>
            f.vtable_group = cls

    # 2) strings
    with open(GHIDRA_OUT / "strings.tsv", encoding="utf-8", errors="replace") as fh:
        next(fh)
        for line in fh:
            p = line.rstrip("\n").split("\t", 4)
            if len(p) < 5 or not p[3]:
                continue
            lab = label_for_string(p[4])
            if not lab:
                continue
            for a in p[3].split(","):
                i = idx_of.get(int(a, 16))
                if i is None:
                    i = owner(int(a, 16))
                if i is not None:
                    fs[i].vote(lab, 2.0, "string")

    # 2b) characteristic imports of a library
    import_rules = [(re.compile(r"^nn::(audio|codec)::"), "lib-closed:wwise")]
    with open(GHIDRA_OUT / "imports.tsv", encoding="utf-8", errors="replace") as fh:
        next(fh)
        for line in fh:
            p = line.rstrip("\n").split("\t")
            if len(p) < 3:
                continue
            name = p[0].replace("<EXTERNAL>::", "")
            for rx, lab in import_rules:
                if not rx.search(name):
                    continue
                for a in p[2].split(","):
                    try:
                        i = owner(int(a, 16))
                    except ValueError:
                        continue
                    if i is not None and "plt" not in fs[i].sources:
                        fs[i].vote(lab, 1.0, "import:" + name.split("(")[0])

    # 3) Lua (byte-by-byte comparison against compiled Lua 5.3.4)
    lua_map = OUT_DIR / "lua_map.csv"
    lua_exact = set()
    lex = OUT_DIR / "lua_exact.json"
    if lex.exists():
        lua_exact = set(json.loads(lex.read_text()))
    if lua_map.exists():
        with open(lua_map, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                i = idx_of.get(int(r["address"], 16))
                if i is None:
                    continue
                f = fs[i]
                f.fixed = "lib-open:lua"
                f.real_name = r["name"]
                if r["name"] in lua_exact:
                    f.fixed_conf, f.fixed_src = "high", "signature:lua-5.3.4 (identical)"
                else:
                    f.fixed_conf, f.fixed_src = "medium", "alignment:lua-5.3.4 (modified)"

    # 3b) exact signatures of other open libraries (tools/triage/flirt.py)
    for p in sorted(OUT_DIR.glob("flirt_*.csv")):
        lib = p.stem.split("_", 1)[1]
        if lib == "lua":
            continue          # Lua already comes from the map (more complete)
        with open(p, encoding="utf-8") as fh:
            for r in csv.DictReader(fh):
                i = idx_of.get(int(r["address"], 16))
                if i is None or fs[i].fixed:
                    continue
                f = fs[i]
                f.fixed, f.fixed_conf = f"lib-open:{lib}", "high"
                f.fixed_src = f"signature:{lib} (identical, {r['obj']})"
                f.real_name = r["name"]

    # 4) vtables (with and without RTTI): they group functions of the same class
    rt = json.loads((GHIDRA_OUT / "rtti.json").read_text(encoding="utf-8", errors="replace"))
    groups = defaultdict(set)
    for name, c in rt["classes"].items():
        for vt in c.get("vtables", []):
            for e in vt.get("entries", []):
                if isinstance(e, str) and e.startswith("0x"):
                    groups[name].add(int(e, 16))
    for v in rt["vtables_without_rtti"]:
        for vt in v.get("vtables", []):
            for e in vt.get("entries", []):
                if isinstance(e, str) and e.startswith("0x"):
                    groups[v["name"]].add(int(e, 16))
    vt_of = defaultdict(list)
    for g, ents in groups.items():
        for a in ents:
            i = idx_of.get(a)
            if i is not None:
                vt_of[i].append(g)
    return idx_of, vt_of


def call_graph(fs, img: Image):
    """caller->callee edges from all the BL/B of the code segment."""
    lo, hi = img.text_range()
    seg = next(s for s in img.segs if s[0] == lo)
    arr = np.frombuffer(seg[3][: (hi - lo) // 4 * 4], dtype="<u4")
    pcs = lo + 4 * np.arange(arr.size, dtype=np.int64)
    isbl = (arr & 0xFC000000) == 0x94000000
    isb = (arr & 0xFC000000) == 0x14000000
    sel = isbl | isb
    imm = (arr[sel] & 0x03FFFFFF).astype(np.int64)
    imm = np.where(imm & 0x2000000, imm - 0x4000000, imm)
    src = pcs[sel]
    dst = src + imm * 4
    starts = np.array([f.addr for f in fs], dtype=np.int64)
    ends = starts + np.array([max(f.size, 4) for f in fs], dtype=np.int64)
    si = np.searchsorted(starts, src, side="right") - 1
    di = np.searchsorted(starts, dst, side="left")
    ok = (si >= 0) & (di < len(fs))
    di_c = np.clip(di, 0, len(fs) - 1)
    ok &= starts[di_c] == dst          # the target is the start of a function
    si_c = np.clip(si, 0, len(fs) - 1)
    ok &= src < ends[si_c]
    ok &= si_c != di_c
    callers = defaultdict(set)
    callees = defaultdict(set)
    for a, b in zip(si_c[ok].tolist(), di_c[ok].tolist()):
        callees[a].add(b)
        callers[b].add(a)
    return callers, callees


def is_thunk(b: bytes) -> bool:
    """Clang non-virtual thunk: add/sub x0,x0,#imm ; b target (8 bytes),
    or the virtual-adjustment one: ldr x16,[x0]; ldur x16,[x16,#-N]; add x0,x0,x16; b."""
    ws = words(b)
    if len(ws) == 2:
        w0, w1 = ws
        # only SUB x0, x0, #imm: a non-virtual thunk subtracts the offset of the
        # base; ADD x0 + B is a real function that forwards to a member.
        sub = (w0 & 0xFF0003FF) == 0xD1000000
        return sub and (w1 & 0xFC000000) == 0x14000000
    if 3 <= len(ws) <= 5 and (ws[-1] & 0xFC000000) == 0x14000000:
        return all((w & 0x1F) in (0, 16) or (w & 0xFC000000) == 0x14000000 for w in ws) and \
            any((w & 0xFFC00000) == 0xF8400000 or (w & 0xFFE00C00) == 0xF8400000 for w in ws)
    return False
