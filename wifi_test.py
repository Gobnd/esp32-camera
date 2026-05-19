#!/usr/bin/env python3
"""
Run once per location: python3 wifi_test.py "next to router"
Prints RSSI and FPS at 1 viewer and 2 viewers.
"""
import sys, time, threading, requests

_ip_arg = next((a for a in sys.argv[1:] if a.startswith("http")), None)
CAM     = _ip_arg if _ip_arg else "http://10.10.10.155"
SETTLE  = 8  # seconds to wait after opening streams

def parse_stats():
    r = requests.get(f"{CAM}/stats", timeout=5)
    rssi = fps = None
    for line in r.text.splitlines():
        l = line.lower()
        if "rssi" in l:
            import re
            m = re.search(r"(-?\d+)", line)
            if m: rssi = int(m.group(1))
        if "stream" in l and "fps" in l:
            m = re.search(r"([\d.]+)", line)
            if m: fps = float(m.group(1))
    return rssi, fps

def stream_thread(stop):
    try:
        with requests.get(f"{CAM}/stream", stream=True, timeout=120) as r:
            for chunk in r.iter_content(chunk_size=4096):
                if stop.is_set():
                    break
    except Exception:
        pass

location = " ".join(sys.argv[1:]) if len(sys.argv) > 1 else "unknown"
print(f"\nLocation: {location}")
print("-" * 40)

# 1 viewer
stop1 = threading.Event()
t1 = threading.Thread(target=stream_thread, args=(stop1,), daemon=True)
t1.start()
time.sleep(SETTLE)
rssi, fps = parse_stats()
print(f"1 viewer  — RSSI: {rssi} dBm  |  FPS: {fps}")

# 2 viewers
stop2 = threading.Event()
t2 = threading.Thread(target=stream_thread, args=(stop2,), daemon=True)
t2.start()
time.sleep(SETTLE)
rssi2, fps2 = parse_stats()
print(f"2 viewers — RSSI: {rssi2} dBm  |  FPS: {fps2}")

stop1.set(); stop2.set()
print()
