#!/usr/bin/env python3
"""
Resolution vs FPS test — measures native max FPS at each supported resolution.
Saves one sample JPEG per resolution to ./frames/.
Usage: python3 res_fps_test.py
"""
import os, re, time, threading, requests
from tabulate import tabulate

CAM = "http://10.10.10.155"
MEASURE_SECS = 12
SETTLE_SECS  = 2

RESOLUTIONS = [
    ("qvga", "320×240",   "100 cm"),
    ("vga",  "640×480",   "150 cm"),
    ("svga", "800×600",   "—"),
    ("hd",   "1280×720",  "300 cm"),
    ("sxga", "1280×1024", "320 cm"),
    ("uxga", "1600×1200", "380 cm"),
]

os.makedirs("frames", exist_ok=True)

def parse_stats():
    try:
        import html as html_lib
        page = requests.get(f"{CAM}/stats", timeout=5).text

        def td(label):
            m = re.search(rf"<td>{re.escape(label)}</td><td>(.*?)</td>", page, re.DOTALL)
            return html_lib.unescape(re.sub(r"<[^>]+>", "", m.group(1))).strip() if m else None

        out = {}
        res = td("Resolution")
        if res: out["resolution"] = res
        temp_raw = td("Chip Temp")
        if temp_raw:
            m = re.search(r"([\d.]+)", temp_raw)
            if m: out["temp"] = float(m.group(1))
        fps_raw = td("Stream FPS")
        if fps_raw:
            m = re.search(r"([\d.]+)", fps_raw)
            if m: out["fps"] = float(m.group(1))
        return out
    except Exception:
        return {}

# pre-check: warn if camera already has an active stream
try:
    _pre = parse_stats()
    _fps = _pre.get("fps", 0)
    if _fps and _fps > 1.0:
        print(f"WARNING: camera /stats shows {_fps} fps — another client is already streaming.")
        print("         Close all browser tabs / VLC pointing at the camera before continuing.")
        input("         Press Enter to proceed anyway, or Ctrl-C to abort: ")
except Exception:
    pass
print()

def measure_fps(stop_evt, result):
    """Collect raw MJPEG bytes, count --frame markers, save first JPEG."""
    frames = 0
    jpeg_saved = False
    buf = b""
    try:
        with requests.get(f"{CAM}/stream", stream=True, timeout=MEASURE_SECS + 5) as r:
            t0 = time.time()
            for chunk in r.iter_content(chunk_size=8192):
                if stop_evt.is_set():
                    break
                buf += chunk
                count = buf.count(b"--frame")
                if count > 1:
                    frames += count - 1
                    # save first complete JPEG
                    if not jpeg_saved and result.get("save_path"):
                        try:
                            start = buf.find(b"\r\n\r\n") + 4
                            end   = buf.find(b"--frame", start)
                            if start > 4 and end > start:
                                with open(result["save_path"], "wb") as f:
                                    f.write(buf[start:end])
                                jpeg_saved = True
                        except Exception:
                            pass
                    buf = buf[buf.rfind(b"--frame"):]
            result["elapsed"] = time.time() - t0
    except Exception:
        result["elapsed"] = MEASURE_SECS
    result["frames"] = frames

rows = []

for res_id, dims, face_range in RESOLUTIONS:
    print(f"  Testing {res_id} ({dims}) ...", end="", flush=True)

    # set resolution
    try:
        requests.get(f"{CAM}/res?id={res_id}", timeout=5)
    except Exception:
        print(" SKIP (camera unreachable)")
        continue
    time.sleep(SETTLE_SECS)

    # confirm
    stats = parse_stats()
    if stats.get("resolution", res_id) != res_id:
        print(f" WARNING: /stats shows {stats.get('resolution')} not {res_id}")

    # measure
    stop  = threading.Event()
    result = {"save_path": f"frames/{res_id}.jpg", "frames": 0, "elapsed": MEASURE_SECS}
    t = threading.Thread(target=measure_fps, args=(stop, result), daemon=True)
    t.start()
    time.sleep(MEASURE_SECS)
    stop.set()
    t.join(timeout=5)

    fps  = result["frames"] / max(result["elapsed"], 1)
    temp = parse_stats().get("temp", "—")
    flag = "YES" if fps >= 20 else ("BORDERLINE" if fps >= 15 else "NO")

    print(f" {fps:.1f} fps  {temp}°C  {flag}")
    rows.append([res_id, dims, f"{fps:.1f}", f"{temp}", face_range, flag])

# restore default
try:
    requests.get(f"{CAM}/res?id=vga", timeout=5)
except Exception:
    pass

print()
print(tabulate(
    rows,
    headers=["Resolution", "Dimensions", "FPS", "Temp (°C)", "Face range (T3)", "≥20 fps?"],
    tablefmt="simple"
))
print()

# recommendation
passing = [(r[0], float(r[2])) for r in rows if r[5] == "YES"]
if passing:
    best = max(passing, key=lambda x: x[1])
    # prefer hd or sxga over raw fps if both pass
    priority = ["uxga","sxga","hd","svga","vga","qvga"]
    best_res = next((r for r in priority if any(p[0]==r for p in passing)), passing[-1][0])
    print(f"Recommendation: highest resolution with ≥20 fps → {best_res}")
else:
    print("No resolution achieved ≥20 fps — check camera and connection.")
print("Sample frames saved to ./frames/")
