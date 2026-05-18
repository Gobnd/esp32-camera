// Practice file — NOT compiled by PlatformIO (it's outside src/)
// To test a stage: copy content into src/main.cpp (back up the original first)
//
// LEARNING ROADMAP — check off each stage as you complete it:
//
//  [ ] STAGE 1 — Camera init + take one photo, print its byte size to Serial
//  [ ] STAGE 2 — Connect to WiFi, serve that photo at http://<ip>/photo
//  [ ] STAGE 3 — Read the PIR pin in loop(), print "motion!" / "quiet" on state change
//  [ ] STAGE 4 — On PIR rising edge, capture a JPEG and serve it at /motion
//  [ ] STAGE 5 — Store the last 3 captures in a ring buffer, serve them at /motion?n=0,1,2
//  [ ] STAGE 6 — Track a quiet-scene baseline; flag whether motion was "in frame" or "off-camera"
//  [ ] STAGE 7 — Serve a live MJPEG stream at /stream (the browser-viewable video feed)
//  [ ] STAGE 8 — Add the full HTTP router: /, /stream, /photo, /motion, /motions, /stats
//               (this is what main.cpp does — all stages combined)
//
// Each stage builds on the last. You can stop at any stage and have something real and working.

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"

const char* SSID     = "SenSen2";
const char* PASSWORD = "wongabongamcdonga";

#define PIR_PIN 1

// Hardware pin assignments — fixed for XIAO ESP32S3 Sense, don't change these
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

bool lastPirState = false;

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 1 — Camera init + take one photo
// Goal: call initCamera(), then in setup() grab a frame and Serial.printf its size.
// You know it works when the Serial monitor prints a number like "Frame: 12345 bytes"
// ─────────────────────────────────────────────────────────────────────────────

void initCamera() {
    camera_config_t cfg {};

    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM;
    cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAMESIZE_VGA;
    cfg.jpeg_quality = 20;   // 0=best/biggest, 63=worst/smallest
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    if (esp_camera_init(&cfg) != ESP_OK) {
        Serial.println("Camera init failed");
        while (true) delay(1000);
    }
    Serial.println("Camera ready");
}

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 2 — WiFi + serve /photo over HTTP
// Goal: connect to WiFi, start a WiFiServer on port 80.
// In loop(), accept a client, read the first line of the HTTP request,
// and if it's "GET /photo" send back the JPEG bytes with the right headers.
// You know it works when opening http://<ip>/photo in a browser shows a photo.
// ─────────────────────────────────────────────────────────────────────────────

// TODO: add WiFiServer server(80); here
// TODO: write setup() — Serial, initCamera(), WiFi.begin(), server.begin(), print IP
// TODO: write loop() — server.accept(), read request line, if /photo send JPEG

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 3 — Read the PIR pin, detect edges
// Goal: in loop(), read digitalRead(PIR_PIN).
// Print "Motion started!" when it goes LOW→HIGH (rising edge).
// Print "Motion stopped." when it goes HIGH→LOW (falling edge).
// lastPirState tracks what the pin was last time so you can spot the change.
// You know it works when waving your hand at the sensor prints the messages.
// ─────────────────────────────────────────────────────────────────────────────

// TODO: add edge detection inside loop() using lastPirState

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 4 — Capture a JPEG on PIR trigger, serve it at /motion
// Goal: combine stage 2 and 3.
// On rising edge: grab a frame, copy it into a global buffer (ps_malloc).
// When a browser requests /motion, send that buffer as a JPEG.
// You know it works when waving at the PIR and then opening /motion shows what it saw.
// ─────────────────────────────────────────────────────────────────────────────

// TODO: global uint8_t* motionBuf = nullptr; size_t motionLen = 0;
// TODO: on rising edge: free old motionBuf, ps_malloc new one, memcpy frame into it
// TODO: in HTTP handler: if /motion, send motionBuf

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 5 — Ring buffer: keep last 3 captures
// Goal: instead of one buffer, use motionSlots[3].
// motionHead points to where the NEXT write goes (wraps around 0→1→2→0).
// /motion?n=0 = most recent, ?n=1 = second, ?n=2 = oldest.
// This is exactly how main.cpp stores captures.
// ─────────────────────────────────────────────────────────────────────────────

// TODO: struct MotionCapture { uint8_t* buf; size_t len; uint32_t ms; };
// TODO: MotionCapture motionSlots[3]; int motionHead = 0; int motionCount = 0;
// TODO: ring-buffer write on rising edge; read index = (motionHead - 1 - n + 9) % 3

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 6 — Baseline + in-frame detection
// Goal: track what the camera sees when nothing is moving (the "quiet scene").
// On PIR falling edge, wait 2 seconds, then sample the empty scene and update:
//   baseline = 0.7 * baseline + 0.3 * newSample   (EMA — slow-adapts to lighting)
// On PIR rising edge, compare the new frame to baseline:
//   if (frame > 1.10 * baseline  AND  frame > baseline + 5120) → "IN FRAME"
//   otherwise → "off-camera" (PIR fired but the person wasn't in the camera's view)
// ─────────────────────────────────────────────────────────────────────────────

// TODO: float baselineJpegLen = 20000.0f;
// TODO: unsigned long pirFellMs = 0; — set on falling edge, used to schedule baseline update
// TODO: inFrame logic in the rising-edge block
// TODO: baseline EMA update in the "2s after PIR fell quiet" block

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 7 — Live MJPEG stream at /stream
// Goal: when a browser connects to /stream, keep sending frames forever.
// MJPEG is just JPEG frames separated by a boundary string — no special codec needed.
// Send this header first:
//   Content-Type: multipart/x-mixed-replace; boundary=frame
// Then in a loop:
//   --frame\r\nContent-Type: image/jpeg\r\nContent-Length: N\r\n\r\n<bytes>\r\n
// Stop when client.connected() is false.
// ─────────────────────────────────────────────────────────────────────────────

// TODO: handle /stream in your HTTP router with the multipart loop

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 8 — Full router (= main.cpp)
// Routes: /  /stream  /photo  /motion  /motions  /res  /quality  /fps  /stats  /restart
// main.cpp also adds: RTSP on port 8554, FreeRTOS tasks per connection, LED flash on motion.
// By this stage your practice file IS main.cpp — you're done.
// ─────────────────────────────────────────────────────────────────────────────
