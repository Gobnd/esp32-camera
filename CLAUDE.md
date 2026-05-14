# ESP32S3 Camera Project — Claude Context

## Hardware
- Board: Seeed XIAO ESP32S3 Sense
- Camera: OV2640 (built-in)
- PIR: Grove PIR motion sensor, signal wire on D0 (GPIO1)
- Camera IP: 10.10.10.155
- WiFi: SenSen2 / wongabongamcdonga

## What this project does
MJPEG camera streaming over HTTP and RTSP, with PIR-triggered motion capture.
- `/` — live stream + controls (resolution, quality, FPS cap)
- `/stream` — raw MJPEG stream
- `/motions` — gallery of last 3 PIR-triggered captures
- `/stats` — CPU, temp, RAM, FPS, RSSI, motion history
- RTSP: `rtsp://10.10.10.155:8554/mjpeg/1` (VLC compatible)

## Key firmware concepts
- PIR edge detection in `loop()` — rising edge captures JPEG, falling edge schedules baseline update
- In-frame detection: motion JPEG must be >10% larger AND >5KB larger than quiet-scene baseline
- Baseline: EMA (70% old + 30% new), updated 2s after PIR falls quiet
- Ring buffer: last 3 captures in `motionSlots[3]`, protected by `motionMutex`
- RTSP runs as FreeRTOS task on core 0, HTTP handlers spawned per-connection on core 1

## Home laptop setup (first time)

### 1. Install VS Code
Download from https://code.visualstudio.com

### 2. Install PlatformIO extension
Open VS Code → Extensions (Ctrl+Shift+X) → search "PlatformIO IDE" → Install
Wait for it to finish installing (takes a few minutes, installs Python tools automatically)

### 3. Clone the repo
Open a terminal and run:
```bash
git clone https://github.com/YOUR_USERNAME/YOUR_REPO_NAME.git
cd YOUR_REPO_NAME
```
Then open that folder in VS Code: File → Open Folder

### 4. Install Python deps
```bash
pip install requests tabulate python-docx
```

### 5. Build and upload
Connect the XIAO ESP32S3 via USB, then in VS Code:
- Click the PlatformIO tick icon (bottom toolbar) to build
- Click the arrow icon to upload
Or in terminal:
```bash
~/.platformio/penv/bin/pio run --target upload
~/.platformio/penv/bin/pio device monitor --baud 115200
```

## Test scripts (Python)
- `run_tests.py` — full automated test suite (power, streams, RTSP, stability, endpoints)
- `res_fps_test.py` — measures FPS at each resolution, saves sample JPEGs to frames/
- `wifi_test.py "location"` — RSSI and FPS at 1 and 2 viewers for a given location

### Python deps
```bash
pip install requests tabulate python-docx
```

## Test results (as of 2026-05-14)
- qvga 320×240: 25.4 fps — best for motion capture
- vga 640×480: 24.8 fps — recommended (best resolution still above 20fps threshold)
- svga 800×600: 13.1 fps — drops below 20fps (OV2640 hardware cliff above VGA)
- hd/sxga/uxga: 7–14 fps — too slow for reliable face capture in motion
- Stability: PASS (300s, 3 streams, no drops, ~9fps board FPS)
- RTSP: PASS
- Temp under load: 56–64°C (normal)

## Files
- `src/main.cpp` — firmware (working, do not break)
- `practice/pir_practice.cpp` — scratch file for learning, not compiled
- `run_tests.py`, `res_fps_test.py`, `wifi_test.py` — test scripts
- `/tmp/make_test_doc.py` — regenerates Cam_tests.docx (NOT in repo, regenerate if needed)
