#!/usr/bin/env python3
"""
Run once per location: python3 wifi_test.py "next to router"
Prints RSSI and FPS at 1 viewer and 2 viewers.
"""
import sys, time, threading, requests, re

_ip_arg = next((a for a in sys.argv[1:] if a.startswith("http")), None)
CAM     = _ip_arg if _ip_arg else "http://10.10.10.155"
MEASURE = 10  # seconds to count frames

def get_rssi():
    page = requests.get(f"{CAM}/stats", timeout=5).text
    m = re.search(r"WiFi Signal</td><td>(-?\d+)", page)
    return int(m.group(1)) if m else None

def measure_fps(result, stop):
    """Count --frame boundaries in the MJPEG stream for MEASURE seconds."""
    frames = 0
    buf = b""
    try:
        with requests.get(f"{CAM}/stream", stream=True, timeout=MEASURE + 5) as r:
            t0 = time.time()
            for chunk in r.iter_content(chunk_size=4096):
                if stop.is_set():
                    break
                buf += chunk
                count = buf.count(b"--frame")
                if count > 1:
                    frames += count - 1
                    buf = buf[buf.rfind(b"--frame"):]
            result["elapsed"] = time.time() - t0
    except Exception:
        result["elapsed"] = MEASURE
    result["frames"] = frames

location = " ".join(a for a in sys.argv[1:] if not a.startswith("http")) or "unknown"
print(f"\nLocation: {location}")
print("-" * 40)

# 1 viewer
stop1 = threading.Event()
res1  = {"frames": 0, "elapsed": MEASURE}
t1 = threading.Thread(target=measure_fps, args=(res1, stop1), daemon=True)
t1.start()
time.sleep(MEASURE)
stop1.set(); t1.join(timeout=3)
rssi = get_rssi()
fps1 = res1["frames"] / max(res1["elapsed"], 1)
print(f"1 viewer  — RSSI: {rssi} dBm  |  FPS: {fps1:.1f}")

# 2 viewers — keep t1's stream alive, add t2
stop1b = threading.Event()
stop2  = threading.Event()
res1b  = {"frames": 0, "elapsed": MEASURE}
res2   = {"frames": 0, "elapsed": MEASURE}
t1b = threading.Thread(target=measure_fps, args=(res1b, stop1b), daemon=True)
t2  = threading.Thread(target=measure_fps, args=(res2,  stop2),  daemon=True)
t1b.start(); t2.start()
time.sleep(MEASURE)
stop1b.set(); stop2.set()
t1b.join(timeout=3); t2.join(timeout=3)
rssi2 = get_rssi()
combined = (res1b["frames"] + res2["frames"]) / max(res1b["elapsed"], res2["elapsed"], 1)
fps2 = combined / 2
print(f"2 viewers — RSSI: {rssi2} dBm  |  FPS: {fps2:.1f} each  ({combined:.1f} combined)")
print()
