#!/usr/bin/env python3
"""
XIAO ESP32S3 Camera Automated Test Suite
Covers: T4 power/stats, T5 multi-stream, T6 RTSP probe,
        T6b 5-min stability, TEP endpoint verification,
        T1 motion monitor (optional — pass --motion)
Run:  python3 run_tests.py           # skip motion window
      python3 run_tests.py --motion  # include 30s PIR window
"""

import re
import sys
import html as html_lib
import json
import time
import socket
import threading
import subprocess
from datetime import datetime

import requests

_ip_arg    = next((a for a in sys.argv[1:] if not a.startswith("--")), None)
CAMERA_IP  = _ip_arg.replace("http://", "").split(":")[0] if _ip_arg else "10.10.10.155"
BASE_URL   = f"http://{CAMERA_IP}"
RTSP_URL   = f"rtsp://{CAMERA_IP}:8554/mjpeg/1"
RESULTS    = []
SEP        = "─" * 62
RUN_MOTION = "--motion" in sys.argv
RUN_WIFI   = "--wifi"   in sys.argv


# ── helpers ────────────────────────────────────────────────────────────────────

def strip_html(s):
    return html_lib.unescape(re.sub(r"<[^>]+>", "", s)).strip()


def parse_stats():
    """GET /stats and return key metrics as a dict."""
    try:
        page = requests.get(f"{BASE_URL}/stats", timeout=5).text

        def td(label):
            m = re.search(rf"<td>{re.escape(label)}</td><td>(.*?)</td>", page, re.DOTALL)
            return strip_html(m.group(1)) if m else "n/a"

        cap_raw = td("Motion Captures")
        cap_n   = cap_raw.split("/")[0].strip() if "/" in cap_raw else cap_raw

        temp_raw    = td("Chip Temp")
        temp        = temp_raw.split("°")[0].strip() + "°C" if "°" in temp_raw else temp_raw

        current_raw = td("Est. Current")
        current     = current_raw.split("(")[0].strip() if "(" in current_raw else current_raw

        quality_raw = td("JPEG Quality")
        quality     = quality_raw.split()[0] if quality_raw != "n/a" else "n/a"

        delay_raw = td("Frame Delay")
        delay_ms  = delay_raw.split()[0] if delay_raw != "n/a" else "n/a"

        return {
            "fps":         td("Stream FPS"),
            "temp":        temp,
            "rssi":        td("WiFi Signal"),
            "ram":         td("Free RAM"),
            "current":     current,
            "cpu":         td("CPU"),
            "captures":    cap_n,
            "resolution":  td("Resolution"),
            "quality":     quality,
            "frame_delay": delay_ms,
        }
    except Exception as e:
        return {"error": str(e)}


MAX_STREAM_BUF = 256 * 1024

def stream_worker(stop_evt, frame_counter, idx):
    """Open /stream and count frames until stop_evt is set."""
    try:
        r = requests.get(f"{BASE_URL}/stream", stream=True, timeout=60)
        buf = b""
        for chunk in r.iter_content(chunk_size=8192):
            if stop_evt.is_set():
                break
            buf += chunk
            count = buf.count(b"--frame")
            if count:
                frame_counter[idx] += count
                buf = buf[buf.rfind(b"--frame"):]
            if len(buf) > MAX_STREAM_BUF:
                buf = b""
        r.close()
    except Exception:
        pass


def rtsp_probe():
    """Send a raw RTSP OPTIONS request and return (success, details_dict)."""
    try:
        s = socket.create_connection((CAMERA_IP, 8554), timeout=5)
        req = (
            f"OPTIONS {RTSP_URL} RTSP/1.0\r\n"
            f"CSeq: 1\r\n"
            f"User-Agent: PythonTestSuite\r\n\r\n"
        )
        s.sendall(req.encode())
        resp = s.recv(2048).decode(errors="replace")
        s.close()

        ok = "RTSP/1.0 200" in resp
        m = re.search(r"Public:\s*(.+)", resp)
        methods = m.group(1).strip() if m else ""
        return ok, {"response_first_line": resp.splitlines()[0], "methods": methods}
    except Exception as e:
        return False, {"error": str(e)}


# ── Test 1 — motion monitor (requires PIR) ─────────────────────────────────────

def run_test1_motion(duration=30):
    print(f"\n{SEP}")
    print("TEST 1 — Motion Capture Monitor")
    print(f"{SEP}")
    print(f"Watching /stats for {duration}s — trigger PIR during this window.")
    print("Press Ctrl-C to end early.\n")

    last_count = -1
    events     = []
    start      = time.time()

    try:
        while time.time() - start < duration:
            remaining = int(duration - (time.time() - start))
            stats = parse_stats()
            cap   = int(stats.get("captures", 0) or 0)

            if last_count == -1:
                last_count = cap

            if cap != last_count:
                ts = round(time.time() - start, 1)
                try:
                    mhtml     = requests.get(f"{BASE_URL}/motions", timeout=3).text
                    latencies = re.findall(r"latency:\s*(\d+)ms", mhtml)
                    sizes     = re.findall(r"size:\s*(\d+)KB", mhtml)
                    badges    = re.findall(r"(IN FRAME|off-camera)", mhtml)
                except Exception:
                    latencies, sizes, badges = [], [], []

                event = {
                    "t":              ts,
                    "total_captures": cap,
                    "latencies_ms":   latencies,
                    "sizes_kb":       sizes,
                    "in_frame":       badges,
                }
                events.append(event)
                last_count = cap
                print(f"  [{ts:5.1f}s] New capture #{cap}  "
                      f"latency={latencies[0] if latencies else '?'}ms  "
                      f"in-frame={badges[0] if badges else '?'}  "
                      f"size={sizes[0] if sizes else '?'}KB")
            else:
                print(f"  [{remaining:3d}s left]  captures so far: {cap}", end="\r")

            time.sleep(1)
    except KeyboardInterrupt:
        print("\n  Stopped early.")

    RESULTS.append(("T1", {"duration_s": duration, "events": events}))
    print(f"\n  Total new captures detected: {len(events)}")


# ── Test 2 — WiFi range ────────────────────────────────────────────────────────

def run_test2_wifi():
    """Interactive WiFi range test — place camera at each location, press Enter."""
    print(f"\n{SEP}")
    print("TEST 2 — WiFi Range (interactive)")
    print(f"{SEP}")
    print("  For each location: move the camera, then type the location name and press Enter.")
    print("  Press Enter with no name to finish.\n")

    MEASURE = 10
    rows = []
    print(f"  {'Location':<32}  {'RSSI':>8}  {'1 viewer':>10}  {'each (2v)':>10}")
    print(f"  {'-'*32}  {'-'*8}  {'-'*10}  {'-'*10}")

    while True:
        loc = input("\n  Location name (blank to finish): ").strip()
        if not loc:
            break

        # 1 viewer
        stop1 = threading.Event()
        fc1   = {0: 0}
        t1 = threading.Thread(target=stream_worker, args=(stop1, fc1, 0), daemon=True)
        t1.start()
        time.sleep(MEASURE)
        stop1.set(); t1.join(timeout=3)
        rssi = parse_stats().get("rssi", "?")
        fps1 = fc1[0] / MEASURE

        # 2 viewers
        stop2a = threading.Event(); stop2b = threading.Event()
        fc2 = {0: 0, 1: 0}
        ta = threading.Thread(target=stream_worker, args=(stop2a, fc2, 0), daemon=True)
        tb = threading.Thread(target=stream_worker, args=(stop2b, fc2, 1), daemon=True)
        ta.start(); tb.start()
        time.sleep(MEASURE)
        stop2a.set(); stop2b.set()
        ta.join(timeout=3); tb.join(timeout=3)
        fps2_each = (fc2[0] + fc2[1]) / MEASURE / 2

        row = {"location": loc, "rssi": rssi, "fps_1v": round(fps1, 1), "fps_2v_each": round(fps2_each, 1)}
        rows.append(row)
        RESULTS.append(("T2", row))
        print(f"  {loc:<32}  {rssi:>8}  {fps1:>9.1f}  {fps2_each:>9.1f}")

    print(f"\n  WiFi range test complete — {len(rows)} location(s) measured.")


# ── Test 4 — power / stats ─────────────────────────────────────────────────────

def record_power(state):
    stats = parse_stats()
    row   = {"state": state, **stats}
    RESULTS.append(("T4", row))
    print(f"  {state:<28}  {stats.get('cpu','?'):>8}  "
          f"{stats.get('current','?'):>10}  "
          f"{stats.get('temp','?'):>8}  "
          f"{stats.get('fps','?'):>18}")
    return row


def run_test4_power():
    print(f"\n{SEP}")
    print("TEST 4 — Power / Stats")
    print(f"{SEP}")
    print(f"  {'State':<28}  {'CPU':>8}  {'Est mA':>10}  {'Temp':>8}  {'FPS':>18}")
    print(f"  {'-'*28}  {'-'*8}  {'-'*10}  {'-'*8}  {'-'*18}")
    time.sleep(1)
    record_power("Idle (no streams)")


# ── Test 5 — multiple simultaneous streams ─────────────────────────────────────

def run_test5_streams():
    print(f"\n{SEP}")
    print("TEST 5 — Multiple Simultaneous Streams")
    print(f"{SEP}")
    print(f"  {'Clients':>8}  {'FPS (board)':>18}  {'Frames rx':>10}  {'Temp':>8}  {'All alive?':>10}")
    print(f"  {'-'*8}  {'-'*18}  {'-'*10}  {'-'*8}  {'-'*10}")

    all_threads    = []
    all_stop_evts  = []
    frame_counters = {}

    for n in [1, 2, 3, 4, 5]:
        stop_evt = threading.Event()
        fc_idx   = len(all_threads)
        frame_counters[fc_idx] = 0
        t = threading.Thread(
            target=stream_worker,
            args=(stop_evt, frame_counters, fc_idx),
            daemon=True,
        )
        t.start()
        all_threads.append(t)
        all_stop_evts.append(stop_evt)

        time.sleep(5)

        stats     = parse_stats()
        total_rx  = sum(frame_counters.values())
        alive     = sum(1 for th in all_threads if th.is_alive())
        all_alive = "yes" if alive == n else f"no ({alive}/{n})"

        row = {
            "clients":   n,
            "fps_board": stats.get("fps", "?"),
            "frames_rx": total_rx,
            "temp":      stats.get("temp", "?"),
            "all_alive": all_alive,
            "error":     stats.get("error", ""),
        }
        RESULTS.append(("T5", row))
        print(f"  {n:>8}  {stats.get('fps','?'):>18}  {total_rx:>10}  {stats.get('temp','?'):>8}  {all_alive:>10}")
        RESULTS.append(("T4", {"state": f"{n}x streams", **stats}))

    for evt in all_stop_evts:
        evt.set()
    for th in all_threads:
        th.join(timeout=5)


# ── Test 6 — RTSP probe ────────────────────────────────────────────────────────

def run_test6_rtsp():
    print(f"\n{SEP}")
    print("TEST 6 — RTSP Stream")
    print(f"{SEP}")
    print(f"  Probing {RTSP_URL} ...")

    ok, details = rtsp_probe()
    RESULTS.append(("T6", {"reachable": ok, **details}))

    if ok:
        print(f"  RTSP responded OK")
        print(f"  First line : {details.get('response_first_line', '?')}")
        print(f"  Methods    : {details.get('methods', '?')}")
    else:
        print(f"  RTSP probe failed: {details.get('error', 'unknown')}")

    try:
        proc = subprocess.run(
            ["ffprobe", "-v", "quiet", "-print_format", "json",
             "-show_streams", "-rtsp_transport", "udp", RTSP_URL],
            capture_output=True, text=True, timeout=12
        )
        data    = json.loads(proc.stdout)
        streams = data.get("streams", [])
        if streams:
            s  = streams[0]
            ff = {"codec": s.get("codec_name"), "width": s.get("width"),
                  "height": s.get("height"), "fps": s.get("r_frame_rate")}
            RESULTS.append(("T6_ffprobe", ff))
            print(f"  ffprobe    : {ff['codec']} {ff['width']}x{ff['height']} @ {ff['fps']} fps")
    except FileNotFoundError:
        print("  ffprobe not installed — skipping deep probe")
    except Exception as e:
        print(f"  ffprobe error: {e}")


# ── Test 6b — 5-minute stability ───────────────────────────────────────────────

def run_test6_stability(duration=300):
    """Hold 3 HTTP streams for duration seconds; poll every 30s for health."""
    print(f"\n{SEP}")
    print(f"TEST 6b — Stability ({duration}s, 3 simultaneous streams)")
    print(f"{SEP}")
    print(f"  {'Elapsed':>8}  {'Alive':>6}  {'FPS':>18}  {'Temp':>8}  {'Free RAM':>12}  {'RSSI':>8}")
    print(f"  {'-'*8}  {'-'*6}  {'-'*18}  {'-'*8}  {'-'*12}  {'-'*8}")

    stop_evts      = [threading.Event() for _ in range(3)]
    frame_counters = {i: 0 for i in range(3)}
    threads = [
        threading.Thread(target=stream_worker, args=(stop_evts[i], frame_counters, i), daemon=True)
        for i in range(3)
    ]
    for t in threads:
        t.start()
    time.sleep(3)

    polls  = []
    start  = time.time()
    passed = True

    while time.time() - start < duration:
        time.sleep(30)
        elapsed = int(time.time() - start)
        stats   = parse_stats()
        alive   = sum(1 for t in threads if t.is_alive())

        poll = {
            "elapsed_s": elapsed,
            "alive":     alive,
            "fps":       stats.get("fps", "?"),
            "temp":      stats.get("temp", "?"),
            "ram":       stats.get("ram", "?"),
            "rssi":      stats.get("rssi", "?"),
        }
        polls.append(poll)
        print(f"  {elapsed:>8}s  {alive:>6}  {stats.get('fps','?'):>18}  "
              f"{stats.get('temp','?'):>8}  {stats.get('ram','?'):>12}  "
              f"{stats.get('rssi','?'):>8}")

        if alive < 3:
            print(f"  WARNING: only {alive}/3 streams alive at {elapsed}s")
            passed = False

    for e in stop_evts:
        e.set()
    for t in threads:
        t.join(timeout=5)

    RESULTS.append(("T6b", {"duration_s": duration, "passed": passed, "polls": polls}))
    print(f"\n  Stability: {'PASS' if passed else 'FAIL'}")


# ── Test EP — endpoint verification ───────────────────────────────────────────

def run_test_endpoints():
    """Verify /res, /quality, /fps each actually change what /stats reports."""
    print(f"\n{SEP}")
    print("TEST EP — Endpoint Verification")
    print(f"{SEP}")

    checks = [
        # (description,           set_url,                       stat_key,      expected, restore_url)
        ("Resolution → qvga",  f"{BASE_URL}/res?id=qvga",     "resolution",  "qvga",  f"{BASE_URL}/res?id=vga"),
        ("Quality    → 10",    f"{BASE_URL}/quality?q=10",    "quality",     "10",    f"{BASE_URL}/quality?q=20"),
        ("Frame delay → 33ms", f"{BASE_URL}/fps?delay=33",    "frame_delay", "33",    f"{BASE_URL}/fps?delay=0"),
    ]

    all_pass = True
    for desc, url, key, expected, restore_url in checks:
        try:
            requests.get(url, timeout=5, allow_redirects=False)
            time.sleep(1)
            actual = parse_stats().get(key, "n/a")
            ok     = (actual == expected)
            status = "PASS" if ok else f"FAIL (got {actual!r}, want {expected!r})"
            all_pass = all_pass and ok
            RESULTS.append(("TEP", {"check": desc, "pass": ok, "got": actual, "expected": expected}))
        except Exception as e:
            status   = f"ERROR: {e}"
            all_pass = False
            RESULTS.append(("TEP", {"check": desc, "pass": False, "error": str(e)}))
        finally:
            try:
                requests.get(restore_url, timeout=5, allow_redirects=False)
                time.sleep(0.5)
            except Exception:
                pass
        print(f"  {desc:<28}  {status}")

    print(f"\n  Endpoint tests: {'all PASS' if all_pass else 'some FAILED'}")


# ── summary ────────────────────────────────────────────────────────────────────

def print_summary():
    print(f"\n{'═'*62}")
    print("FINAL SUMMARY")
    print(f"{'═'*62}")
    print(f"Camera : {BASE_URL}")
    print(f"Time   : {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")

    t2_rows = [r for tag, r in RESULTS if tag == "T2"]
    if t2_rows:
        print("TEST 2: WiFi Range")
        print(f"  {'Location':<32}  {'RSSI':>8}  {'1 viewer':>10}  {'each (2v)':>10}")
        print(f"  {'-'*32}  {'-'*8}  {'-'*10}  {'-'*10}")
        for r in t2_rows:
            print(f"  {r['location']:<32}  {r['rssi']:>8}  {r['fps_1v']:>10}  {r['fps_2v_each']:>10}")
        print()

    if RUN_MOTION:
        print("TEST 1: Motion Captures")
        t1_rows = [r for tag, r in RESULTS if tag == "T1"]
        if t1_rows:
            events = t1_rows[0].get("events", [])
            for e in events:
                print(f"  t={e['t']}s  captures={e['total_captures']}  "
                      f"latencies={e['latencies_ms']}  in-frame={e['in_frame']}")
            if not events:
                print("  No PIR triggers detected during window")

    print("\nTEST 4: Power / Stats")
    print(f"  {'State':<28}  {'CPU':>8}  {'Est mA':>10}  {'Temp':>8}  {'FPS':>18}")
    print(f"  {'-'*28}  {'-'*8}  {'-'*10}  {'-'*8}  {'-'*18}")
    for tag, r in RESULTS:
        if tag == "T4":
            print(f"  {r.get('state',''):<28}  {r.get('cpu',''):>8}  "
                  f"{r.get('current',''):>10}  {r.get('temp',''):>8}  {r.get('fps',''):>18}")

    print("\nTEST 5: Multiple Streams")
    print(f"  {'Clients':>8}  {'FPS (board)':>18}  {'Frames rx':>10}  {'Temp':>8}  {'All alive?':>10}")
    print(f"  {'-'*8}  {'-'*18}  {'-'*10}  {'-'*8}  {'-'*10}")
    for tag, r in RESULTS:
        if tag == "T5":
            print(f"  {r.get('clients',''):>8}  {r.get('fps_board',''):>18}  "
                  f"{r.get('frames_rx',''):>10}  {r.get('temp',''):>8}  {r.get('all_alive',''):>10}")

    print("\nTEST 6: RTSP")
    for tag, r in RESULTS:
        if tag == "T6":
            print(f"  Reachable : {'PASS' if r.get('reachable') else 'FAIL'}")
            if "error" in r:
                print(f"  Error     : {r['error']}")
            else:
                print(f"  Response  : {r.get('response_first_line','?')}")
                print(f"  Methods   : {r.get('methods','?')}")
        if tag == "T6_ffprobe":
            print(f"  ffprobe   : {r.get('codec')} {r.get('width')}x{r.get('height')} @ {r.get('fps')} fps")

    for tag, r in RESULTS:
        if tag == "T6b":
            print(f"\nTEST 6b — Stability ({r['duration_s']}s): {'PASS' if r['passed'] else 'FAIL'}")

    ep_rows = [(tag, r) for tag, r in RESULTS if tag == "TEP"]
    if ep_rows:
        print("\nTEST EP — Endpoint Verification")
        for _, r in ep_rows:
            status = "PASS" if r.get("pass") else f"FAIL — got {r.get('got')!r}"
            print(f"  {r.get('check',''):<28}  {status}")

    out_path = "test_results.json"
    with open(out_path, "w") as f:
        json.dump(
            {"run_at": datetime.now().isoformat(), "camera": BASE_URL, "results": RESULTS},
            f, indent=2, default=str
        )
    print(f"\nRaw results saved to {out_path}")


# ── main ───────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    print("ESP32S3 Camera: Automated Test Suite")
    print(f"Target : {BASE_URL}")
    print(f"Motion : {'ENABLED (--motion)' if RUN_MOTION else 'skipped (pass --motion to enable)'}")
    print(f"WiFi   : {'ENABLED (--wifi)'   if RUN_WIFI   else 'skipped (pass --wifi to enable)'}\n")

    try:
        requests.get(BASE_URL, timeout=5)
        print("Board reachable OK")
    except Exception as e:
        print(f"ERROR: Board not reachable ({e})\nCheck IP and WiFi.")
        sys.exit(1)

    # Wait until the camera is actually delivering frames before testing.
    # Right after a flash/reboot the sensor needs ~20-30s to fully initialise.
    print("Waiting for camera to stabilise", end="", flush=True)
    for _ in range(30):
        try:
            r = requests.get(f"{BASE_URL}/stream", stream=True, timeout=5)
            buf = b""
            for chunk in r.iter_content(chunk_size=8192):
                buf += chunk
                if b"--frame" in buf:
                    r.close()
                    break
            else:
                r.close()
        except Exception:
            pass
        stats = parse_stats()
        fps_str = stats.get("fps", "0")
        try:
            fps = float(fps_str.split()[0])
        except Exception:
            fps = 0.0
        if fps >= 5.0:
            print(f" ready ({fps:.1f} fps)\n")
            break
        print(".", end="", flush=True)
        time.sleep(2)
    else:
        print(" timeout — proceeding anyway\n")

    if RUN_WIFI:
        run_test2_wifi()

    run_test4_power()
    run_test6_rtsp()
    run_test_endpoints()
    run_test5_streams()
    run_test6_stability(duration=300)

    if RUN_MOTION:
        print(f"\n{SEP}")
        input("Press Enter to start 30s motion monitor window (trigger PIR now)...")
        run_test1_motion(duration=30)

    print_summary()
