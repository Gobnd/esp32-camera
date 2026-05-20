// main_hybrid.cpp — PIR-triggered streaming camera
//
// Sleeps at ~14µA until PIR fires, then wakes, streams live MJPEG
// at esp32cam.local/stream for 30 seconds, then goes back to sleep.
//
// VGA 640x480, quality 20 — ~24fps, good face capture.
// Power bank keepalive: timer wakeup every 30s draws enough current
// to prevent auto-shutoff.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_camera.h"
#include "driver/rtc_io.h"

struct Network { const char* ssid; const char* pass; };
static const Network NETWORKS[] = {
    { "SenSen2",         "wongabongamcdonga" },
    { "Home2.4g",        "18lookoutway"      },
    { "Gobind's iPhone", "12345678"          },
};

#define PIR_PIN        1
#define STREAM_SECS    30

#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  10
#define SIOD_GPIO_NUM  40
#define SIOC_GPIO_NUM  39
#define Y9_GPIO_NUM    48
#define Y8_GPIO_NUM    11
#define Y7_GPIO_NUM    12
#define Y6_GPIO_NUM    14
#define Y5_GPIO_NUM    16
#define Y4_GPIO_NUM    18
#define Y3_GPIO_NUM    17
#define Y2_GPIO_NUM    15
#define VSYNC_GPIO_NUM 38
#define HREF_GPIO_NUM  47
#define PCLK_GPIO_NUM  13

RTC_DATA_ATTR int bootCount = 0;

void goToSleep() {
    Serial.println("Going to sleep. Waiting for PIR...");
    Serial.flush();
    esp_camera_deinit();
    rtc_gpio_pulldown_en((gpio_num_t)PIR_PIN);
    rtc_gpio_pullup_dis((gpio_num_t)PIR_PIN);
    esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
    esp_sleep_enable_ext0_wakeup((gpio_num_t)PIR_PIN, 1);
    esp_deep_sleep_start();
}

bool initCamera() {
    camera_config_t cfg {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM; cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM; cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM; cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM; cfg.pin_d7 = Y9_GPIO_NUM;
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
    return esp_camera_init(&cfg) == ESP_OK;
}

void streamAndSleep() {
    // Connect WiFi
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
        Serial.println("\nWiFi failed");
        goToSleep();
    }

    MDNS.begin("esp32cam");
    Serial.printf("\nStreaming: http://esp32cam.local/stream  (%s)\n",
                  WiFi.localIP().toString().c_str());

    WiFiServer server(80);
    server.begin();

    unsigned long deadline = millis() + (STREAM_SECS * 1000UL);

    while (millis() < deadline) {
        WiFiClient client = server.accept();
        if (!client) { delay(5); continue; }

        // Read request (ignore path — only endpoint is /stream)
        while (client.available()) client.read();

        // Send MJPEG headers
        client.print(
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Cache-Control: no-cache\r\n\r\n"
        );

        // Stream until window closes or client disconnects
        while (client.connected() && millis() < deadline) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) { delay(10); continue; }
            client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
            client.write(fb->buf, fb->len);
            client.print("\r\n");
            esp_camera_fb_return(fb);
        }
        client.stop();
        Serial.println("Client disconnected");
    }

    WiFi.disconnect(true);
    goToSleep();
}

void setup() {
    Serial.begin(115200);
    delay(300);
    bootCount++;
    Serial.printf("\n=== Wake #%d ===\n", bootCount);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        Serial.println("Wake cause: PIR motion");
        if (!initCamera()) {
            Serial.println("Camera init failed");
            goToSleep();
        }
        // Discard stale buffered frame, grab fresh one to confirm camera is ready
        { camera_fb_t* s = esp_camera_fb_get(); if (s) esp_camera_fb_return(s); }
        streamAndSleep();
    } else {
        // Timer keepalive or first boot — go straight back to sleep
        if (cause == ESP_SLEEP_WAKEUP_UNDEFINED)
            Serial.println("First boot — sleeping");
        else
            Serial.println("Timer keepalive — sleeping");
        goToSleep();
    }
}

void loop() {}
