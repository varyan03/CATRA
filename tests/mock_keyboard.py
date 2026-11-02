#!/usr/bin/env python3
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
    ensure_fifo(FIFO_PATH)
    print(f"[*] FIFO ready at {FIFO_PATH}. Waiting for reader...")

    # Blocks until test_sensor opens the read descriptor
    with open(FIFO_PATH, "wb") as fifo:
        print("[+] Reader attached. Emitting simulated keystrokes...")
        for i in range(10):
            now = time.time()
            sec = int(now)
            usec = int((now - sec) * 1_000_000)

            packet = struct.pack(FORMAT, sec, usec, EV_KEY, KEY_SPACE, PRESSED)
            fifo.write(packet)
            fifo.flush()
            print(f"[>] Injected keystroke {i + 1}/10")
            time.sleep(0.5)

    print("[*] Completed 10 events. Closed FIFO pipe to trigger POLLIN + POLLHUP drain.")

if __name__ == "__main__":
    main()
