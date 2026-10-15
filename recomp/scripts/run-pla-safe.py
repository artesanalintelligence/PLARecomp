#!/usr/bin/env python3
"""Starts PLA in the fork's suyu (JIT or Hybrid) WITHOUT closing other open suyu.exe instances.

run-hybrid.ps1 (mk8-recomp) kills any suyu.exe before starting; this launcher only
touches the process it creates: it waits for its MCP server, requests `launch_game_path`,
accepts the "Firmware Not Found" modal of ITS window, logs the FPS every 10 s, stops
emulation and closes its suyu. The MCP port (9742) must be free when it starts.

    python recomp/scripts/run-pla-safe.py --seconds 120
    python recomp/scripts/run-pla-safe.py --seconds 120 --recomp-dir D:/PLARecomp/mk8-recomp/build/recomp/pla
"""
from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SUYU = Path(r"D:\PLARecomp\mk8-recomp\build\suyu\bin\suyu.exe")
# Base game NSP of the user's own dump (repo dump/ folder), found by the title id in its file name.
ROM = next((p for p in sorted((HERE.parents[1] / "dump").glob("*.nsp")) if "[01001F5010DFA000]" in p.name), None)
PORT = 9742


def rpc(method, params=None, timeout=60.0):
    req = {"jsonrpc": "2.0", "id": 1, "method": method}
    if params is not None:
        req["params"] = params
    with socket.create_connection(("127.0.0.1", PORT), timeout=10.0) as s:
        s.settimeout(timeout)
        s.sendall(json.dumps(req).encode())
        buf = b""
        while True:
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
            try:
                return json.loads(buf)
            except json.JSONDecodeError:
                continue
    raise RuntimeError("incomplete response")


def call(name, args=None, timeout=60.0):
    out = rpc("tools/call", {"name": name, "arguments": args or {}}, timeout)
    for block in out.get("result", {}).get("content", []):
        if block.get("type") == "text":
            try:
                return json.loads(block["text"])
            except json.JSONDecodeError:
                return block["text"]
    return out


def port_open() -> bool:
    try:
        socket.create_connection(("127.0.0.1", PORT), timeout=1.0).close()
        return True
    except OSError:
        return False


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seconds", type=int, default=120)
    ap.add_argument("--recomp-dir", default="")
    ap.add_argument("--rom", default=str(ROM) if ROM else "")
    args = ap.parse_args()
    if not args.rom:
        print("no base game NSP found in the repo dump/ folder: pass --rom", file=sys.stderr)
        return 2
    if port_open():
        print("MCP port 9742 is already in use (another suyu): not starting", file=sys.stderr)
        return 2
    env = dict(os.environ)
    if args.recomp_dir:
        env["SUYU_RECOMP_DIR"] = args.recomp_dir
    log = Path(os.environ["APPDATA"]) / "suyu" / "log" / "suyu_log.txt"
    if log.exists():
        log.unlink()
    p = subprocess.Popen([str(SUYU), "-hacker"], env=env)
    print(f"suyu pid {p.pid}")
    try:
        t0 = time.time()
        while not port_open():
            if time.time() - t0 > 120 or p.poll() is not None:
                print("the MCP server did not start", file=sys.stderr)
                return 1
            time.sleep(1)
        res = {}
        th = threading.Thread(target=lambda: res.update(r=call("launch_game_path", {"path": args.rom}, 300)))
        th.start()
        click = HERE / "click-suyu-button.ps1"
        while th.is_alive():
            subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(click),
                            "-Button", "Yes", "-WindowText", "Firmware Not Found", "-ProcessId", str(p.pid)],
                           capture_output=True)
            th.join(2)
        print("launch:", json.dumps(res.get("r")))
        for secs in range(10, args.seconds + 1, 10):
            time.sleep(10)
            st = call("get_emulator_state", timeout=30)
            print(f"  {secs:>4}s running={st.get('game_running')} first_frame={st.get('first_frame_displayed')} "
                  f"fps={st.get('fps')}", flush=True)
        print("stop:", json.dumps(call("stop_emulation", timeout=120)))
        time.sleep(5)
    finally:
        if p.poll() is None:
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            f"$p = Get-Process -Id {p.pid} -ErrorAction SilentlyContinue; if ($p) {{ $null = $p.CloseMainWindow() }}"])
            try:
                p.wait(120)
            except subprocess.TimeoutExpired:
                p.kill()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
