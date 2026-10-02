#!/usr/bin/env python3
"""Talk to the U-Boot USB CDC console exposed by lk2nd.

U-Boot is loaded by lk2nd as the payload, and its `usb serial` gadget appears
as a CDC-ACM device. This is the natural rig for testing the TrustZone SMC
interface: U-Boot runs at EL1, apps level, by which point EL3 is already
loaded, so SMCs can be issued with controlled registers without flashing
anything and without a kernel module.

    python3 tools/uboot_console.py "version" "bdinfo"
    python3 tools/uboot_console.py --interactive

Requires membership of the dialout group for /dev/ttyACM0.
"""

from __future__ import annotations

import argparse
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("error: pyserial not installed (pip install pyserial)")

PROMPT = b"=>"
DEFAULT_PORT = "/dev/ttyACM0"


class Console:
    def __init__(self, port: str, baud: int = 115200, settle: float = 1.0) -> None:
        self.s = serial.Serial(port, baud, timeout=0.3)
        self.settle = settle

    def drain(self, seconds: float | None = None) -> bytes:
        end = time.time() + (self.settle if seconds is None else seconds)
        out = b""
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                out += chunk
                end = time.time() + 0.4  # extend while data still flowing
        return out

    def send(self, line: str, wait: float | None = None) -> str:
        """Send one command and return everything printed until the prompt."""
        self.drain(0.2)  # clear any stale output
        self.s.write(line.encode() + b"\n")
        time.sleep(0.2)
        out = self.drain(wait)
        text = out.decode("latin1", "replace")
        # strip the echoed command and trailing prompt for readability
        lines = [l for l in text.splitlines() if l.strip() and not l.startswith(PROMPT.decode())]
        return "\n".join(lines).strip()

    def close(self) -> None:
        self.s.close()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("commands", nargs="*", help="commands to send, in order")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--settle", type=float, default=1.0)
    ap.add_argument("--interactive", action="store_true")
    args = ap.parse_args()

    try:
        con = Console(args.port, args.baud, args.settle)
    except Exception as e:
        sys.exit(f"error: cannot open {args.port}: {e}")

    try:
        con.send("")  # nudge for a prompt
        for cmd in args.commands:
            print(f"$ {cmd}")
            out = con.send(cmd)
            if out:
                print(out)
            print()
        if args.interactive:
            print("interactive: Ctrl-D to exit")
            for line in sys.stdin:
                out = con.send(line.rstrip("\n"))
                if out:
                    print(out)
    finally:
        con.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
