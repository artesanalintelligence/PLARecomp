"""Index of address markers in src/ and include/.

Convention (see docs/decomp/CONTRIBUTING.md):

    // 0x7100123456                 <- marker: the next declaration/definition is that address
    u32 Foo::getCount() const { ... }

    // 0x7100123460 D0              <- variant: D0 (deleting destructor), C1/C2/D1/D2 optional
    // vtable 0x7104abcdef          <- above `class Foo`: address of the _ZTV symbol (start of the vtable)
    // 0x7104aaaa00                 <- above an `extern` variable (global data)
    // thunk 0x7100123470 -72 D1    <- non-virtual thunk (_ZThn72_) of the D1 of the next declaration
    // vthunk 0x7100123480 0 -24 D1 <- virtual thunk (_ZTv0_n24_) of the D1 of the next declaration
    // static-init 0x7100123490     <- (standalone, in the .cpp) static initializer of the TU (_GLOBAL__sub_I_<file>)

The index associates each marker with its qualified name (namespace + class + name),
the number of parameters, the return type and the file/line. verify.py uses it to
(1) find in the .o the symbol of the function being verified and (2) link the
external symbols to the original addresses.
"""
from __future__ import annotations

import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from common import CACHE, INCLUDE, ROOT, SRC  # noqa: E402

MARKER_RE = re.compile(r"^\s*//\s*(vtable\s+)?(?:@\s*)?(?:0x)?(71[0-9a-fA-F]{8})\b\s*(C1|C2|D0|D1|D2)?\b(.*)$")
# Special markers: the keyword goes BEFORE the address so that old versions of the
# tools (which only recognize MARKER_RE) ignore them instead of taking them for normal markers.
_NUM = r"[-+]?(?:0x[0-9a-fA-F]+|\d+)"
SPECIAL_RE = re.compile(r"^\s*//\s*(thunk|vthunk|static-init|sinit)\s+(?:0x)?(71[0-9a-fA-F]{8})\b\s*(.*)$")
THUNK_ARGS_RE = re.compile(r"^(?:this\s*=\s*)?(" + _NUM + r")\s*(?:(C1|C2|D0|D1|D2)\b)?")
VTHUNK_ARGS_RE = re.compile(r"^(" + _NUM + r")\s+(" + _NUM + r")\s*(?:(C1|C2|D0|D1|D2)\b)?")
INDEX_VERSION = 2   # bump it if the parsing changes: invalidates the srcindex cache
EXTS = {".cpp", ".cc", ".cxx", ".h", ".hpp", ".hh", ".inc"}
CV_RE = re.compile(r"\b(static|virtual|inline|constexpr|explicit|extern|friend|override|final|"
                   r"__attribute__\s*\(\(.*?\)\)|\[\[.*?\]\]|NON_MATCHING|PLA_\w+)\b")


@dataclass
class Marker:
    addr: int
    kind: str          # func | var | vtable
    qualname: str      # ns::Foo::bar
    nparams: int       # -1 if not applicable
    params: str
    ret: str           # text of the return type (func) or of the type (var)
    variant: str       # C1 C2 D0 D1 D2 or ''
    is_c: bool         # extern "C"
    is_def: bool       # definition with a body
    file: str
    line: int          # (1-based) line of the declaration
    decl: str          # normalized text of the declaration
    special: str = ""  # '' | 'thunk:h<adjustment>' | 'thunk:v<nv>:<vcall>' | 'sinit'


def _blank(text: str) -> str:
    """Replaces comments and literals with spaces, preserving positions."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            # The "C" literal of extern "C" is kept.
            if text[i:j + 1] != '"C"':
                for k in range(i + 1, min(j, n)):
                    out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


_NS_RE = re.compile(r"(?:^|\s)namespace\s*([\w:]*)\s*$")
_CLS_RE = re.compile(r"(?:^|[\s>])(class|struct|union)\s+(?:\[\[.*?\]\]\s*|__attribute__\(\(.*?\)\)\s*|alignas\s*\(.*?\)\s*)*"
                     r"(\w+)\s*(?:final\s*)?(?::[^{]*)?$", re.S)
_EXTC_RE = re.compile(r'extern\s+"C"\s*$')


def _scopes(blank: str, positions: list[int]) -> dict[int, list[str] | None]:
    """Scope stack (names) at each requested position. None if inside a function body."""
    want = sorted(set(positions))
    res: dict[int, list[str] | None] = {}
    stack: list[tuple[str, str]] = []  # (kind, name)
    wi = 0
    last_stmt = 0
    for i, c in enumerate(blank):
        while wi < len(want) and want[wi] <= i:
            body = any(t == "other" for t, _ in stack)
            res[want[wi]] = None if body else [n for t, n in stack if t in ("ns", "cls") and n]
            wi += 1
        if c in ";":
            last_stmt = i + 1
        elif c == "{":
            hdr = " ".join(blank[last_stmt:i].split())
            # remove access labels (public: etc.) from the header
            hdr = re.sub(r"^(?:public|private|protected)\s*:\s*", "", hdr)
            m = _NS_RE.search(hdr)
            if m:
                for part in (m.group(1) or "").split("::") or [""]:
                    stack.append(("ns", part))
                if not m.group(1):
                    pass
                stack[-1] = stack[-1]
                # namespace a::b { -> two levels, but a single brace: mark how many
                stack.append(("nsend", str(max(1, len((m.group(1) or "").split("::"))))))
            elif _EXTC_RE.search(hdr):
                stack.append(("externc", ""))
            elif "(" not in hdr and _CLS_RE.search(hdr) and "=" not in hdr:
                stack.append(("cls", _CLS_RE.search(hdr).group(2)))
            elif re.search(r"\benum\b", hdr) or (hdr.endswith("=") or hdr == "" and stack and stack[-1][0] == "cls"):
                stack.append(("enum", ""))
            else:
                stack.append(("other", ""))
            last_stmt = i + 1
        elif c == "}":
            if stack:
                t, n = stack.pop()
                if t == "nsend":
                    for _ in range(int(n)):
                        if stack:
                            stack.pop()
            last_stmt = i + 1
        elif c == ":" and blank[i - 1:i + 1] != "::" and blank[i:i + 2] != "::":
            # public: / private: / protected:
            if re.search(r"(public|private|protected)\s*$", blank[last_stmt:i]):
                last_stmt = i + 1
    while wi < len(want):
        res[want[wi]] = [n for t, n in stack if t in ("ns", "cls") and n]
        wi += 1
    return res


def _split_params(s: str) -> int:
    s = s.strip()
    if not s or s == "void":
        return 0
    depth = 0
    n = 1
    for c in s:
        if c in "<([":
            depth += 1
        elif c in ">)]":
            depth -= 1
        elif c == "," and depth == 0:
            n += 1
    return n


_EXPR_ARG_RE = re.compile(r"""^(?:["']|[-+]?\.?\d|true\b|false\b|nullptr\b|NULL\b|&|\{|sizeof\b|alignof\b|"""
                          r"""static_cast\b|reinterpret_cast\b|const_cast\b)""")


def _ctor_args(params: str) -> bool:
    """Is the parenthesized list arguments (expressions) rather than parameters? It is only decided so
    when some argument is clearly an expression (literal, &x, true, nullptr...): a bare
    identifier (`void f(Foo);`) remains an unnamed parameter."""
    s = params.strip()
    if not s:
        return False
    depth, cur, args = 0, "", []
    for c in s:
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        if c == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += c
    args.append(cur)
    return any(_EXPR_ARG_RE.match(a.strip()) for a in args)


def _parse_decl(decl: str):
    """-> (kind, name, params, ret, is_def) or None."""
    d = " ".join(decl.split())
    # Find the '(' of the parameter list (not inside <...>, and not the one of operator()).
    depth = 0
    i = 0
    paren = -1
    mop = re.search(r"\boperator\s*(\(\)|\[\]|new\b|delete\b|[^\s(\w]+|\w[\w\s*&]*?(?=\())", d)
    if mop:
        paren = d.find("(", mop.end())
        i = len(d)
    while i < len(d):
        c = d[i]
        if c == "<":
            depth += 1
        elif c == ">":
            depth = max(0, depth - 1)
        elif c == "(" and depth == 0:
            pre = d[:i].rstrip()
            if pre.endswith("operator"):
                i = d.find(")", i) + 1
                continue
            if pre.endswith("__attribute__") or pre.endswith("alignas") or pre.endswith("__declspec"):
                # skip the attributes block
                lvl = 0
                while i < len(d):
                    if d[i] == "(":
                        lvl += 1
                    elif d[i] == ")":
                        lvl -= 1
                        if lvl == 0:
                            break
                    i += 1
                i += 1
                continue
            paren = i
            break
        elif c in ";={" and depth == 0:
            break
        i += 1
    if paren >= 0:
        pre = d[:paren].rstrip()
        m = re.search(r"((?:[\w]+(?:<[^()]*?>)?::)*(?:operator\s*(?:\(\)|\[\]|[^\s(\w]+|new|delete|\w[\w\s*&]*?)|~?\w+))\s*$",
                      pre)
        if not m:
            return None
        name = m.group(1).replace(" ", "") if "operator" not in m.group(1) else \
            re.sub(r"operator\s*([^\w\s])", r"operator\1", m.group(1))
        ret = pre[:m.start()].strip()
        # parameters: up to the closing ')'
        lvl, j = 0, paren
        while j < len(d):
            if d[j] == "(":
                lvl += 1
            elif d[j] == ")":
                lvl -= 1
                if lvl == 0:
                    break
            j += 1
        params = d[paren + 1:j]
        rest = d[j + 1:]
        is_def = "{" in rest.split(";")[0] if ";" in rest else "{" in rest
        if ret and "operator" not in name and _ctor_args(params):
            # `T g("x");` / `T g(1, 2);`: variable with direct initialization, not a function
            return "var", name, "", ret, False
        if not is_def and "::" in name and re.match(r"\s*(?:noexcept\s*)?=\s*default\b", rest):
            # `X::~X() = default;` outside the class: it is a definition
            is_def = True
        return "func", name, params, ret, is_def
    # variable
    m = re.search(r"(~?[\w:]+)\s*(?:\[[^\]]*\]\s*)*(?:=.*)?;?\s*$", d.split("{")[0].split(";")[0] + ";")
    if not m:
        return None
    return "var", m.group(1), "", d[:m.start()].strip(), False


def scan_file(path: Path) -> list[Marker]:
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    if not re.search(r"71[0-9a-fA-F]{8}", text):
        return []
    lines = text.split("\n")
    line_off = [0]
    for ln in lines:
        line_off.append(line_off[-1] + len(ln) + 1)
    blank = _blank(text)
    pending = []
    out = []
    rel = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else str(path)
    for li, ln in enumerate(lines):
        m = MARKER_RE.match(ln)
        if m:
            m = (bool(m.group(1)), int(m.group(2), 16), m.group(3) or "", "")
        else:
            sm = SPECIAL_RE.match(ln)
            if not sm:
                continue
            kw, addr, rest = sm.group(1), int(sm.group(2), 16), sm.group(3).strip()
            if kw in ("static-init", "sinit"):
                # standalone: not tied to any declaration; it is this TU's _GLOBAL__sub_I_<file>
                out.append(Marker(addr, "func", f"_GLOBAL__sub_I_{path.name}", 0, "", "void", "", True,
                                  path.suffix in (".cpp", ".cc", ".cxx"), rel, li + 1, ln.strip(), "sinit"))
                continue
            if kw == "thunk":
                ta = THUNK_ARGS_RE.match(rest)
                if not ta:
                    continue
                m = (False, addr, ta.group(2) or "", f"thunk:h{int(ta.group(1), 0)}")
            else:
                ta = VTHUNK_ARGS_RE.match(rest)
                if not ta:
                    continue
                m = (False, addr, ta.group(3) or "", f"thunk:v{int(ta.group(1), 0)}:{int(ta.group(2), 0)}")
        # the declaration starts at the next non-empty line that is not a comment/marker
        j = li + 1
        while j < len(lines) and (not lines[j].strip() or lines[j].strip().startswith("//")
                                  or lines[j].strip().startswith("template")):
            if lines[j].strip().startswith("template"):
                break
            j += 1
        if j >= len(lines):
            continue
        start = line_off[j]
        # end: first ';' or '{' outside parentheses
        k, lvl = start, 0
        while k < len(blank):
            c = blank[k]
            if c == "(":
                lvl += 1
            elif c == ")":
                lvl -= 1
            elif c in ";{" and lvl <= 0:
                k += 1
                break
            k += 1
        decl = blank[start:k]
        pending.append((m, j, start, decl))
    scopes = _scopes(blank, [p[2] for p in pending])
    for m, j, start, decl in pending:
        is_vt, addr, variant, special = m
        sc = scopes.get(start)
        if sc is None:
            continue  # inside a function body: ignore
        d = " ".join(decl.split())
        is_c = 'extern "C"' in d or _in_extern_c(blank, start)
        d_noc = d.replace('extern "C"', " ").strip()
        if is_vt:
            mm = re.search(r"\b(?:class|struct)\s+(?:\[\[.*?\]\]\s*)?(\w+)", d_noc)
            if not mm:
                continue
            out.append(Marker(addr, "vtable", "::".join(sc + [mm.group(1)]), -1, "", "", "", False, False,
                              rel, j + 1, d))
            continue
        parsed = _parse_decl(CV_RE.sub(" ", d_noc))
        if not parsed:
            continue
        kind, name, params, ret, is_def = parsed
        if special and kind != "func":
            continue
        q = name if is_c else "::".join(sc + [name])
        out.append(Marker(addr, kind, q, _split_params(params) if kind == "func" else -1, params.strip(),
                          ret, variant, is_c, is_def, rel, j + 1, d, special))
    return out


def _in_extern_c(blank: str, pos: int) -> bool:
    # approximation: is there an open extern "C" { before pos?
    depth_open = [m.end() for m in re.finditer(r'extern\s+"C"\s*\{', blank[:pos])]
    for o in depth_open:
        lvl = 1
        for c in blank[o:pos]:
            if c == "{":
                lvl += 1
            elif c == "}":
                lvl -= 1
                if lvl == 0:
                    break
        if lvl > 0:
            return True
    return False


def iter_source_files():
    for base in (SRC, INCLUDE, ROOT / "lib"):
        if base.exists():
            for p in base.rglob("*"):
                if p.suffix in EXTS and p.is_file() and "_smoke" not in p.parts:
                    yield p


def build_index(use_cache: bool = True) -> list[Marker]:
    CACHE.mkdir(parents=True, exist_ok=True)
    # srcindex_v<N>.json: old versions of the tool (running processes) keep using
    # srcindex.json without clashing with the new format (`special` field).
    cpath = CACHE / f"srcindex_v{INDEX_VERSION}.json"
    cache = {}
    if use_cache and cpath.exists():
        try:
            cache = json.loads(cpath.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            cache = {}
    new_cache = {}
    out = []
    for p in iter_source_files():
        key = str(p)
        st = p.stat()
        sig = f"{st.st_size}:{st.st_mtime_ns}"
        ent = cache.get(key)
        if ent and ent["sig"] == sig:
            ms = [Marker(**m) for m in ent["markers"]]
        else:
            ms = scan_file(p)
        new_cache[key] = {"sig": sig, "markers": [asdict(m) for m in ms]}
        out.extend(ms)
    if new_cache != cache:
        import os
        tmp = cpath.with_name(f"{cpath.stem}.{os.getpid()}_{id(out)}.tmp")
        try:
            tmp.write_text(json.dumps(new_cache), encoding="utf-8")
            tmp.replace(cpath)
        except OSError:
            pass   # cache: if another process has it open, it will be retried next time
        finally:
            try:
                tmp.unlink()
            except OSError:
                pass
    return out


# ------------------------------------------------------------ demangling and matching
def split_demangled(dem: str):
    """'ns::Foo::bar(int, float) const' -> ('ns::Foo::bar', 2). For variables: (name, -1)."""
    s = dem.strip()
    if s.startswith("vtable for "):
        return ("vtable", s[len("vtable for "):], -1)
    # remove trailing qualifiers
    if not s.endswith(")"):
        t = re.sub(r"\s*(const|volatile|&&|&|noexcept)\s*$", "", s)
        while t != s:
            s = t
            t = re.sub(r"\s*(const|volatile|&&|&|noexcept)\s*$", "", s)
    if s.endswith(")"):
        lvl = 0
        for i in range(len(s) - 1, -1, -1):
            if s[i] == ")":
                lvl += 1
            elif s[i] == "(":
                lvl -= 1
                if lvl == 0:
                    name = s[:i]
                    # return type of templates: "int foo<int>(...)" -> remove the prefix
                    if " " in name and "operator" not in name:
                        name = name.rsplit(" ", 1)[-1]
                    return ("func", name, _split_params(s[i + 1:-1]))
    return ("var", s, -1)


_SUBST_RE = re.compile(r"S(?:[0-9A-Z]*_|[tabsiod])")
_THUNK_RE = re.compile(r"_ZT(?:h(n?\d+)_|v(n?\d+)_(n?\d+)_)")


def thunk_spec(sym: str) -> tuple[str, str]:
    """'_ZThn72_N3Foo3barEv' -> ('thunk:h-72', '_ZN3Foo3barEv'); '_ZTv0_n24_...' -> ('thunk:v0:-24', ...).
    For symbols that are not thunks: ('', sym)."""
    m = _THUNK_RE.match(sym)
    if not m:
        return "", sym

    def num(t):
        return -int(t[1:]) if t.startswith("n") else int(t)
    if m.group(1) is not None:
        spec = f"thunk:h{num(m.group(1))}"
    else:
        spec = f"thunk:v{num(m.group(2))}:{num(m.group(3))}"
    return spec, "_Z" + sym[m.end():]


def _skip_source_name(s: str, i: int) -> int:
    j = i
    while j < len(s) and s[j].isdigit():
        j += 1
    return j + int(s[i:j])


def _skip_template_args(s: str, i: int) -> int:
    """s[i] == 'I': returns the index after the 'E' that closes the template arguments."""
    depth = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c.isdigit():
            i = _skip_source_name(s, i)
            continue
        if c == "L":
            if s.startswith("L_Z", i):       # external name: L _Z <encoding> E
                depth += 1
                i += 3
                continue
            j = s.find("E", i)                # literal: L <type> <value> E
            if j < 0:
                return n
            i = j + 1
            continue
        if c in "INXJ":
            depth += 1
            i += 1
            continue
        if c == "E":
            depth -= 1
            i += 1
            if depth <= 0:
                return i
            continue
        if c == "S":
            m = _SUBST_RE.match(s, i)
            i = m.end() if m else i + 1
            continue
        if c == "T":
            j = s.find("_", i)
            i = n if j < 0 else j + 1
            continue
        if c == "D" and i + 1 < n and s[i + 1] in "tT":
            depth += 1
            i += 2
            continue
        i += 2 if c == "D" else 1
    return i


def mangled_variant(sym: str) -> str:
    """Constructor/destructor variant (C1 C2 C3 D0 D1 D2) of an Itanium symbol, or ''.

    Only counts if the <ctor-dtor-name> is the last component of the <nested-name> (right before its
    closing 'E'). An identifier containing 'D0' (`Foo_D0`) does not count, because source
    names are preceded by their length and are skipped whole. Thunks are analyzed on their target."""
    if not sym.startswith("_Z"):
        return ""
    _, base = thunk_spec(sym)
    s = base[2:]
    if not s.startswith("N"):
        return ""    # constructors/destructors are always in a nested name
    i, n = 1, len(s)
    while i < n and s[i] in "rVKRO":
        i += 1
    last = ""
    try:
        while i < n:
            c = s[i]
            if c.isdigit():
                i = _skip_source_name(s, i)
                last = ""
            elif c == "I":
                i = _skip_template_args(s, i)
            elif c == "S":
                m = _SUBST_RE.match(s, i)
                if not m:
                    return ""
                i = m.end()
                last = ""
            elif c == "C" and i + 1 < n and s[i + 1] in "123":
                last = s[i:i + 2]
                i += 2
            elif c == "C" and s.startswith("CI", i) and i + 2 < n and s[i + 2] in "12":
                return "C" + s[i + 2]       # inherited constructor: CI1 <base type>
            elif c == "D" and i + 1 < n and s[i + 1] in "012":
                last = s[i:i + 2]
                i += 2
            elif c == "B":                  # ABI tag: B <source-name>
                i = _skip_source_name(s, i + 1)
            elif c == "E":
                return last
            elif c == "L":                  # names with internal linkage
                i += 1
            elif c == "T":
                j = s.find("_", i)
                if j < 0:
                    return ""
                i = j + 1
                last = ""
            else:                           # operator name (two letters) or something else
                i += 2
                last = ""
    except (ValueError, IndexError):
        return ""
    return ""


_THUNK_DEM_PREFIXES = ("non-virtual thunk to ", "virtual thunk to ", "covariant return thunk to ")


class MarkerIndex:
    def __init__(self, markers: list[Marker] | None = None):
        self.markers = markers if markers is not None else build_index()
        self.by_addr: dict[int, list[Marker]] = {}
        self.by_name: dict[tuple[str, str], list[Marker]] = {}
        for m in self.markers:
            self.by_addr.setdefault(m.addr, []).append(m)
            self.by_name.setdefault((m.kind, m.qualname), []).append(m)

    def lookup_symbol(self, sym: str, demangled: str | None, file: str | None = None) -> Marker | None:
        """Finds the marker that corresponds to a symbol (mangled + demangled).

        `file` (relative path of the TU) disambiguates static initializers (`_GLOBAL__sub_I_<file>`),
        which are named the same in TUs with the same file name."""
        if sym.startswith("_GLOBAL__sub_I_"):
            cands = [c for c in self.by_name.get(("func", sym), []) if c.special == "sinit"]
            if file:
                cands = [c for c in cands if c.file == file] or (cands if len(cands) == 1 else [])
            return cands[0] if len({c.addr for c in cands}) == 1 else None
        cand = self.by_name.get(("func", sym)) or self.by_name.get(("var", sym))
        if cand and any(c.is_c and not c.special for c in cand):
            return next(c for c in cand if c.is_c and not c.special)
        special, base = thunk_spec(sym)
        if not demangled:
            if sym.startswith("_Z"):
                return None
            # C++ global in the global namespace (`extern T* gFoo;` without extern "C"):
            # its symbol is the name as is, without mangling
            demangled = sym
        elif special:
            for p in _THUNK_DEM_PREFIXES:
                if demangled.startswith(p):
                    demangled = demangled[len(p):]
                    break
        # The index does not store anonymous namespaces (`namespace { ... }`): remove them here too
        demangled = demangled.replace("(anonymous namespace)::", "")
        kind, name, npar = split_demangled(demangled)
        allc = self.by_name.get((kind, name), [])
        cands = [c for c in allc if c.special == special]
        if special and not cands:
            # Compatibility: without a `// thunk` marker for this thunk, it behaves as before (it is tied to the
            # target function's marker). This way TUs whose local vtables reference
            # unmarked secondary-base thunks keep linking.
            cands = [c for c in allc if not c.special]
        if kind == "func":
            cands = [c for c in cands if not c.is_c]
            var = mangled_variant(base)
            if var == "D0":
                cands = [c for c in cands if c.variant == "D0"]
            elif var:
                cands = [c for c in cands if c.variant in ("", var)]
            else:
                cands = [c for c in cands if c.variant == ""]
            if len({c.addr for c in cands}) > 1:
                cands = [c for c in cands if c.nparams == npar]
        if len({c.addr for c in cands}) == 1:
            return cands[0]
        return None


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(description="Lists the address markers of src/ and include/")
    ap.add_argument("--file", help="only this file")
    a = ap.parse_args()
    ms = scan_file(Path(a.file)) if a.file else build_index()
    for m in ms:
        sp = f" [{m.special}]" if m.special else ""
        print(f"0x{m.addr:010x} {m.kind:6} {m.variant:2} {m.qualname}({m.nparams}) ret='{m.ret}' "
              f"{'def' if m.is_def else 'decl'} {m.file}:{m.line}{sp}")
