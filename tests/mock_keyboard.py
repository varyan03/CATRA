#!/usr/bin/env python3
"""
Simulated evdev keyboard for CATRA testing.

Default (no args) keeps the original behaviour: 10 keystrokes, 0.5 s apart.
Integration mode:  --wpm 80 --duration 40   (WPM uses the standard 5 keystrokes/word)
"""
import argparse
import os
import stat
import struct
import time

FIFO_PATH = "/tmp/fake_keyboard"
# Linux 64-bit struct input_event: timeval (tv_sec: long, tv_usec: long), type: __u16, code: __u16, value: __s32
FORMAT = "@llHHi"
EV_KEY = 1
KEY_SPACE = 57
PRESSED = 1


def ensure_fifo(path: str) -> None:
    if os.path.exists(path):
        if not stat.S_ISFIFO(os.stat(path).st_mode):
            os.remove(path)
            os.mkfifo(path)
    else:
        os.mkfifo(path)


def main() -> None:
    parser = argparse.ArgumentParser(description="CATRA mock keyboard (FIFO evdev emitter)")
    parser.add_argument("--fifo", default=FIFO_PATH)
    parser.add_argument("--count", type=int, default=10, help="keystrokes to send (ignored with --wpm)")
    parser.add_argument("--interval", type=float, default=0.5, help="seconds between keystrokes (ignored with --wpm)")
    parser.add_argument("--wpm", type=float, help="target typing speed; enables integration mode")
    parser.add_argument("--duration", type=float, default=30.0, help="seconds to type for in --wpm mode")
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    if args.wpm is not None:
        if args.wpm <= 0:
            parser.error("--wpm must be > 0")
        interval = 60.0 / (args.wpm * 5.0)
        count = max(1, int(args.duration / interval))
    else:
        interval = args.interval
        count = args.count

    ensure_fifo(args.fifo)
    if not args.quiet:
        print(f"[*] FIFO ready at {args.fifo}. Waiting for reader...")

    # Blocks until the daemon/test_sensor opens the read descriptor
    with open(args.fifo, "wb") as fifo:
        if not args.quiet:
            print(f"[+] Reader attached. Emitting {count} keystrokes every {interval:.3f}s...")
        next_due = time.monotonic()
        for i in range(count):
            now = time.time()
            sec = int(now)
            usec = int((now - sec) * 1_000_000)

            fifo.write(struct.pack(FORMAT, sec, usec, EV_KEY, KEY_SPACE, PRESSED))
            fifo.flush()
            if not args.quiet:
                print(f"[>] Injected keystroke {i + 1}/{count}")

            # Drift-free pacing so the effective WPM matches the request
            next_due += interval
            delay = next_due - time.monotonic()
            if delay > 0:
                time.sleep(delay)

    if not args.quiet:
        print(f"[*] Completed {count} events. Closed FIFO pipe to trigger POLLIN + POLLHUP drain.")


if __name__ == "__main__":
    main()
