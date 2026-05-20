// deep_sleep_motion.cpp — NOT compiled by PlatformIO (it's outside src/)
// To test: copy this file's content into src/main.cpp (back up the original first)
//
// GOAL: camera only wakes when PIR detects motion, then goes back to deep sleep.
// No live stream. No persistent web server. ~14µA while sleeping vs ~250mA awake.
//
// KEY INSIGHT: with deep sleep there is no loop(). setup() runs fresh on every wake.
// The program flow is: setup() → capture → sleep → [PIR fires] → setup() → capture → sleep → ...
//
// LEARNING ROADMAP:
//
//  [ ] STAGE 1 — Boot, capture one photo, print its size, go to sleep for 10 seconds, repeat
//                Goal: prove deep sleep works and camera survives repeated init/deinit
//
//  [ ] STAGE 2 — Check WHY the board woke up before doing anything
//                On first boot: skip capture, just configure wake source and sleep
//                On PIR wake:   capture the photo, print size, sleep again
//                Goal: the board only captures when it was actually the PIR that fired
//
//  [ ] STAGE 3 — Reconnect WiFi after PIR wake, serve the photo at /motion for 30 seconds
//                Any browser that opens /motion during those 30 seconds sees the capture
//                Goal: a working battery-friendly motion camera you can check remotely
//
//  [ ] STAGE 4 — Survive baseline across sleep cycles using RTC RAM
//                RTC_DATA_ATTR variables are stored in a tiny 8KB memory region that
//                stays powered during deep sleep — use this to keep the baseline alive
//                Goal: in-frame detection works correctly across reboots
//
//  [ ] STAGE 5 — Only wake WiFi and serve if the capture was in-frame
//                If the person wasn't visible to the camera, go straight back to sleep
//                without spending the 4-8 seconds on WiFi reconnect
//                Goal: maximum battery efficiency — no wasted transmission
//
// ─────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "driver/rtc_io.h"

struct Network { const char* ssid; const char* pass; };
static const Network NETWORKS[] = {
    { "SenSen2",        "wongabongamcdonga" },
    { "Home2.4g",       "18lookoutway"      },
    { "Gobind's iPhone","12345678"          },
};

#define PIR_PIN         1       // GPIO1 = D0 on XIAO — this is an RTC GPIO, required for ext0 wake

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

// ── RTC RAM: survives deep sleep, lost only on hard reset or power loss ───────
// Declare any variable you need to remember across sleep cycles with RTC_DATA_ATTR.
// Regular variables (including globals) are wiped when the chip sleeps.
RTC_DATA_ATTR int   bootCount       = 0;       // counts total wakes — useful for debugging
RTC_DATA_ATTR float baselineJpegLen = 20000.0f; // quiet-scene JPEG size, adapts over time

void goToSleep();  // forward declaration — defined after initCamera()

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 1 — Camera init (same pins as main.cpp, same config)
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
    cfg.jpeg_quality = 20;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    if (esp_camera_init(&cfg) != ESP_OK) {
        Serial.println("Camera init failed — sleeping 5s and retrying");
        goToSleep();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sleep helper — always configure the wake source before sleeping
// ext0: one specific GPIO, one specific level (HIGH = 1, LOW = 0)
// The PIR holds its output HIGH while motion is detected, so we wake on HIGH.
// ─────────────────────────────────────────────────────────────────────────────

// Re-init camera, sample one quiet-scene frame into the RTC baseline EMA, deinit.
// Call this just before goToSleep() so the baseline stays calibrated across wake cycles.
void updateBaseline() {
    camera_config_t cfg {};
    cfg.ledc_channel = LEDC_CHANNEL_0; cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM; cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM; cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM; cfg.pin_pclk = PCLK_GPIO_NUM;
    cfg.pin_vsync = VSYNC_GPIO_NUM; cfg.pin_href = HREF_GPIO_NUM;
    cfg.pin_sccb_sda = SIOD_GPIO_NUM; cfg.pin_sccb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn = PWDN_GPIO_NUM; cfg.pin_reset = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000; cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size = FRAMESIZE_VGA; cfg.jpeg_quality = 20;
    cfg.fb_count = 2; cfg.fb_location = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode = CAMERA_GRAB_LATEST;
    if (esp_camera_init(&cfg) != ESP_OK) return;
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
        baselineJpegLen = 0.7f * baselineJpegLen + 0.3f * (float)fb->len;
        Serial.printf("Baseline updated: %.0f bytes\n", baselineJpegLen);
        esp_camera_fb_return(fb);
    }
    esp_camera_deinit();
}

void goToSleep() {
    Serial.println("Going to sleep. Waiting for PIR...");
    Serial.flush();  // make sure Serial output finishes before power cuts
    rtc_gpio_pulldown_en((gpio_num_t)PIR_PIN);  // hold pin LOW during sleep — prevents spurious wakes from a floating line
    rtc_gpio_pullup_dis((gpio_num_t)PIR_PIN);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIR_PIN, 1);  // wake when GPIO1 goes HIGH
    esp_deep_sleep_start();  // this line never returns — chip powers down
}

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 3 — WiFi connect + serve the photo for a limited window
// After the window closes the board goes back to sleep.
// Any client that connects during the window gets the JPEG.
// ─────────────────────────────────────────────────────────────────────────────

void servePhotoAndSleep(uint8_t* buf, size_t len, bool inFrame) {
    Serial.print("Connecting to WiFi");
    for (auto& net : NETWORKS) {
        WiFi.begin(net.ssid, net.pass);
        for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) break;
        WiFi.disconnect();
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nWiFi failed — sleeping");
        goToSleep();
    }
    Serial.printf("\nOpen: http://%s/motion\n", WiFi.localIP().toString().c_str());

    WiFiServer server(80);
    server.begin();

    // Stay awake for 30 seconds to let someone check the photo
    unsigned long deadline = millis() + 30000;
    while (millis() < deadline) {
        WiFiClient client = server.accept();
        if (client) {
            // Drain the HTTP request (we don't need to parse it — only one endpoint)
            while (client.available()) client.read();
            client.printf(
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/jpeg\r\n"
                "Content-Length: %u\r\n"
                "X-In-Frame: %s\r\n"
                "\r\n",
                len, inFrame ? "yes" : "no"
            );
            client.write(buf, len);
            client.stop();
            Serial.printf("Served %u bytes (%s)\n", len, inFrame ? "IN FRAME" : "off-camera");
        }
        delay(10);
    }

    WiFi.disconnect(true);
    updateBaseline();  // re-sample quiet scene before sleeping so baseline stays calibrated
    goToSleep();
}

// ─────────────────────────────────────────────────────────────────────────────
// STAGE 2 — Main entry point
// setup() runs on every wake (first boot AND every deep sleep wake).
// esp_sleep_get_wakeup_cause() tells you which triggered this run.
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);  // small settle — PSRAM needs a moment after power-on

    bootCount++;
    Serial.printf("\n=== Wake #%d ===\n", bootCount);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        // ── PIR fired ────────────────────────────────────────────────────────
        Serial.println("Wake cause: PIR motion");

        initCamera();

        // Discard the buffered (stale) frame, grab a fresh one
        { camera_fb_t* s = esp_camera_fb_get(); if (s) esp_camera_fb_return(s); }
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            Serial.println("Camera capture failed");
            goToSleep();
        }

        // ── STAGE 4: in-frame detection using RTC-persisted baseline ─────────
        float ratio    = (float)fb->len / baselineJpegLen;
        float absDelta = (float)fb->len - baselineJpegLen;
        bool  inFrame  = (ratio > 1.10f) && (absDelta > 5120.0f);

        Serial.printf("JPEG: %u bytes | baseline: %.0f | ratio: %.2f | %s\n",
                      fb->len, baselineJpegLen, ratio, inFrame ? "IN FRAME" : "off-camera");

        // Copy frame to PSRAM — fb must be returned before WiFi init or it may block
        uint8_t* copy = (uint8_t*)ps_malloc(fb->len);
        size_t   copyLen = fb->len;
        if (copy) memcpy(copy, fb->buf, fb->len);
        esp_camera_fb_return(fb);
        esp_camera_deinit();  // free camera resources before WiFi starts (both are heavy)

        // ── STAGE 5: skip WiFi if not in frame ───────────────────────────────
        if (!inFrame) {
            Serial.println("Not in frame — skipping WiFi, going back to sleep");
            free(copy);
            updateBaseline();
            goToSleep();
        }

        if (copy) {
            servePhotoAndSleep(copy, copyLen, inFrame);
            // servePhotoAndSleep calls goToSleep() at the end — never returns
        }

    } else {
        // ── First boot (or manual reset) ──────────────────────────────────────
        // On first boot cause == ESP_SLEEP_WAKEUP_UNDEFINED.
        // Do NOT try to capture here — the PIR may or may not be HIGH.
        // Instead, take a quiet-scene baseline and go to sleep.
        Serial.println("First boot — capturing baseline, then sleeping");

        initCamera();
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            baselineJpegLen = (float)fb->len;  // stored in RTC RAM — survives all future sleeps
            Serial.printf("Baseline set: %.0f bytes\n", baselineJpegLen);
            esp_camera_fb_return(fb);
        }
        esp_camera_deinit();
    }

    goToSleep();
}

void loop() {
    // Never runs — setup() always ends in goToSleep() which resets the chip
}
