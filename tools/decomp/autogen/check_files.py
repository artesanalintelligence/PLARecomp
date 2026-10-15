"""Consistency of src/auto/: every definition marker must have an autogen O/F status for that file.

  python tools/decomp/autogen/check_files.py [--fix]   # --fix deletes orphan files (with no O/F)
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import ROOT  # noqa: E402
import status as st  # noqa: E402

rows = st.read()
bad_files = []
for p in sorted((ROOT / "src" / "auto").glob("*.cpp")):
    rel = p.relative_to(ROOT).as_posix()
    addrs = [int(x, 16) for x in re.findall(r"^// 0x(71[0-9a-f]{8})$", p.read_text(encoding="utf-8"), re.M)]
    ok = [a for a in addrs if rows.get(a, {}).get("file") == rel and rows[a].get("quality") in ("O", "F")]
    if len(ok) != len(addrs):
        print(f"{rel}: {len(addrs)} functions, {len(ok)} with O/F status in this file")
        bad_files.append((p, len(ok)))
if "--fix" in sys.argv:
    for p, n in bad_files:
        if n == 0:
            p.unlink()
            print("deleted", p.name)
