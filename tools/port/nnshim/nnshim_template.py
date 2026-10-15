#!/usr/bin/env python3
"""Builds the nnshim mod.json template used by the installer.

The hook list comes from gen_mod_json.py (--fps-fixes --ui: the same hooks as the play configuration) and needs
generated/*.inc, which never ships. The template is generated once at release time and shipped in the platform
package; launcher.py fills it at install time by replacing the {GAME} placeholder with the game folder.

  python tools/port/nnshim/nnshim_template.py --out build/port/nnshim/mod.template.json
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
GAME = "{GAME}"
TITLE = "01001F5010DFA000"

# Play configuration on top of gen_mod_json's defaults (what the tested play setup uses).
EXTRA_CONFIG = {
    "account": {"hle_backing": False},
    "time": {"hle_backing": False},
    "err": {"dialog": True, "dump_dir": f"{GAME}/user/log"},
    "swkbd": {"mode": "dialog"},
    "hid": {"controls_file": f"{GAME}/mods/{TITLE}/nnshim/controls.json"},
    "ui": {"dump_dir": f"{GAME}/user/ui-dump"},
}


def build(out: Path) -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        gen = Path(tmp) / "mod.json"
        subprocess.run([sys.executable, str(HERE / "gen_mod_json.py"), "--out", str(gen), "--romfs-bin", "auto",
                        "--save-root", f"{GAME}/user/save", "--input", "auto", "--fps-fixes", "--ui",
                        "--set", f"font.dir={GAME}/fonts"], check=True, capture_output=True)
        doc = json.loads(gen.read_text(encoding="utf-8"))
    doc["config"].update(EXTRA_CONFIG)
    doc["config"].pop("romfs_dir", None)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=1), encoding="utf-8")
    return doc


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, required=True)
    a = ap.parse_args()
    doc = build(a.out)
    print(f"{a.out}: {len(doc['hooks'])} hooks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
