#!/usr/bin/env python3
"""
PIR Motion Capture Test — streaming firmware
Records MJPEG stream frames + downloads PIR captures for each subject.

Usage: python3 motion_test.py [http://IP]
"""
import os, re, sys, time, threading, requests
from tabulate import tabulate

CAM     = next((a for a in sys.argv[1:] if a.startswith("http")), "http://10.10.10.155")
OUT     = "motion_captures"
RECORD  = 15  # seconds to record stream per subject

SUBJECTS = [
    "fast walker",
    "normal walker",
    "slow walker",
    "wheelchair (office chair)",
]

os.makedirs(OUT, exist_ok=True)


def record_stream(label, stop_evt, result):
    """Pull MJPEG stream and save each frame as a JPEG."""
    folder = os.path.join(OUT, label)
    os.makedirs(folder, exist_ok=True)
    buf = b""
    frame_n = 0
    try:
        with requests.get(f"{CAM}/stream", stream=True, timeout=RECORD + 5) as r:
            for chunk in r.iter_content(chunk_size=4096):
                if stop_evt.is_set():
                    break
                buf += chunk
                while True:
                    start = buf.find(b"\r\n\r\n")
                    if start == -1:
                        break
                    start += 4
                    end = buf.find(b"--frame", start)
                    if end == -1:
                        break
                    frame_data = buf[start:end]
                    if frame_data.startswith(b"\xff\xd8"):  # valid JPEG
                        path = os.path.join(folder, f"frame_{frame_n:04d}.jpg")
                        with open(path, "wb") as f:
                            f.write(frame_data)
                        frame_n += 1
                    buf = buf[end:]
    except Exception:
        pass
    result["frames"] = frame_n
    result["folder"] = folder


def get_capture_count():
    try:
        page = requests.get(f"{CAM}/stats", timeout=5).text
        m = re.search(r"Motion Captures</td><td>(\d+)", page)
        return int(m.group(1)) if m else 0
    except Exception:
        return 0


def download_pir_captures(label, n_new):
    """Download PIR-triggered captures from /motion?n=0,1,2 into subject folder."""
    folder = os.path.join(OUT, label)
    os.makedirs(folder, exist_ok=True)
    saved = []
    for i in range(min(n_new, 3)):
        try:
            r = requests.get(f"{CAM}/motion?n={i}", timeout=10)
            if r.status_code == 200 and "image" in r.headers.get("Content-Type", ""):
                path = os.path.join(folder, f"pir_{i}.jpg")
                with open(path, "wb") as f:
                    f.write(r.content)
                saved.append(path)
        except Exception:
            pass
    return saved


rows = []
print(f"\nPIR Motion Capture Test — Streaming firmware")
print(f"Camera  : {CAM}")
print(f"Output  : ./{OUT}/<subject>/")
print(f"Per subject: {RECORD}s stream recording + PIR captures\n")

for subject in SUBJECTS:
    label = re.sub(r"[^a-z0-9]+", "_", subject.lower()).strip("_")

    baseline = get_capture_count()
    input(f"--- [{subject}] ---\nGet them ready, then press Enter... ")
    print(f"\n  >>> SEND THEM NOW <<<\n")

    # Record stream in background
    stop_evt = threading.Event()
    result   = {"frames": 0, "folder": ""}
    t = threading.Thread(target=record_stream, args=(label, stop_evt, result), daemon=True)
    t.start()

    # Countdown
    for remaining in range(RECORD, 0, -1):
        print(f"  Recording... {remaining}s  ", end="\r", flush=True)
        time.sleep(1)
    stop_evt.set()
    t.join(timeout=5)

    # Download PIR captures
    final_count = get_capture_count()
    n_new       = final_count - baseline
    pir_files   = download_pir_captures(label, n_new) if n_new > 0 else []

    print(f"\n  Stream frames saved : {result['frames']}  →  ./{result['folder']}/")
    print(f"  PIR captures        : {n_new} ({len(pir_files)} downloaded)")
    print()

    rows.append([subject, result["frames"], n_new, f"./{result['folder']}/"])

print("=" * 65)
print(tabulate(
    rows,
    headers=["Subject", "Stream frames", "PIR captures", "Folder"],
    tablefmt="simple",
))
print("\nOpen each folder to review frames and check face visibility.")
