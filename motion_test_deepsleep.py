#!/usr/bin/env python3
"""
Deep Sleep Motion Capture Test
Board sleeps until PIR fires, then wakes, connects WiFi, and serves one photo
at http://esp32cam.local/motion for 30 seconds before going back to sleep.

Usage:
    python3 motion_test_deepsleep.py
"""
import os, re, time, requests
from tabulate import tabulate

CAM = "http://esp32cam.local"
OUT = "motion_captures_deepsleep"
os.makedirs(OUT, exist_ok=True)

SUBJECTS = [
    "fast walker",
    "normal walker",
    "slow walker",
    "wheelchair (office chair)",
]

SERVE_WINDOW = 30  # seconds the board stays awake after PIR fires
POLL_TIMEOUT = 60  # seconds to wait for board to appear on network after PIR

def wait_for_board():
    """Poll esp32cam.local until it responds or timeout."""
    deadline = time.time() + POLL_TIMEOUT
    while time.time() < deadline:
        try:
            r = requests.get(f"{CAM}/motion", timeout=3)
            if r.status_code == 200:
                return r
        except Exception:
            pass
        time.sleep(0.5)
        print(f"  waiting for board... {int(deadline - time.time())}s left  ", end="\r")
    return None

def wait_for_sleep():
    """Block until esp32cam.local stops responding (board went back to sleep)."""
    print("  Waiting for board to go back to sleep...", end="", flush=True)
    while True:
        try:
            requests.get(f"{CAM}/motion", timeout=2)
            print(".", end="", flush=True)  # still awake
        except Exception:
            print(" asleep.\n")
            return
        time.sleep(1)

rows = []
print(f"\nDeep Sleep Motion Capture Test")
print(f"Board hostname : esp32cam.local")
print(f"Images         : ./{OUT}/")
print(f"Subjects       : {len(SUBJECTS)}")
print(f"\nNOTE: board sleeps between captures. After each subject walks past")
print(f"the PIR, the board has {SERVE_WINDOW}s to connect WiFi and serve the photo.\n")

for subject in SUBJECTS:
    label = re.sub(r"[^a-z0-9]+", "_", subject.lower()).strip("_")
    fname = f"{OUT}/{label}.jpg"

    input(f"\n--- Subject: [{subject}] ---\nGet them ready, then press Enter... ")
    print(f"\n  >>> SEND THEM NOW <<<\n")
    print(f"  Watching for PIR trigger ({POLL_TIMEOUT}s window)...")

    t0 = time.time()
    response = wait_for_board()
    elapsed = time.time() - t0

    if response is None:
        print(f"\n  Board did not appear within {POLL_TIMEOUT}s — skipping [{subject}]")
        print(f"  (Either PIR didn't trigger, or board went back to sleep before connecting)\n")
        rows.append([subject, "—", "—", "—", "no response"])
        continue

    inframe  = response.headers.get("X-In-Frame", "?")
    size_b   = len(response.content)
    size_kb  = size_b // 1024

    with open(fname, "wb") as f:
        f.write(response.content)

    print(f"\n  Board responded in {elapsed:.1f}s")
    print(f"  In-frame : {inframe}")
    print(f"  JPEG size: {size_kb}KB")
    print(f"  Saved    : {fname}")

    rows.append([subject, f"{elapsed:.1f}s", f"{size_kb}KB", inframe, fname])

    wait_for_sleep()

print("=" * 70)
print(tabulate(
    rows,
    headers=["Subject", "Wake time", "JPEG size", "In-frame?", "File"],
    tablefmt="simple",
))
print(f"\nOpen ./{OUT}/ to check face visibility in each image.")
