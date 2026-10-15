#!/usr/bin/env python3
"""Sends input commands to the nn shim over UDP (config "hid.remote_port").

Commands run queued, one after another (see hid_core.cpp, ParseRemoteLocked):
  press <buttons> <ms>          buttons: A, B, X, Y, L, R, ZL, ZR, Plus, Minus, StickL, StickR,
                                Left, Up, Right, Down (combinable with +), or a 0x... mask
  stick <L|R> <x> <y> <ms>      x, y in -32767..32767 (up = positive)
  state <buttons> <lx> <ly> <rx> <ry> <ms>
  wait <ms>
  clear                         empties the queue

Examples:
  python tools/port/nnshim/nnshim_input.py "press A 120; wait 400; press A 120"
  python tools/port/nnshim/nnshim_input.py --port 7001 "stick L 0 32767 2000"
  python tools/port/nnshim/nnshim_input.py --tap A --times 5 --gap 600
"""
from __future__ import annotations

import argparse
import socket


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="?", default="")
    ap.add_argument("--port", type=int, default=7001)
    ap.add_argument("--tap", default="", help="shortcut: press these buttons --times times")
    ap.add_argument("--times", type=int, default=1)
    ap.add_argument("--hold", type=int, default=120)
    ap.add_argument("--gap", type=int, default=500)
    args = ap.parse_args(argv)
    cmd = args.commands
    if args.tap:
        cmd = "; ".join(f"press {args.tap} {args.hold}; wait {args.gap}" for _ in range(args.times))
    if not cmd:
        ap.error("no commands")
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.sendto(cmd.encode("utf-8"), ("127.0.0.1", args.port))
    print(f"sent to 127.0.0.1:{args.port}: {cmd}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
