// main_recognition.cpp — PIR-triggered face recognition firmware
//
// Framework: ESP-IDF (not Arduino) — see [env:recognition] in platformio.ini
// Upload: ~/.platformio/penv/bin/platformio run -e recognition --target upload
//
// Behaviour: deep sleep until PIR fires, then wake, run face recognition for
// 30s via esp-who pipeline, sleep again. Face embeddings stored in SPIFFS.
//
// Endpoints (when awake):
//   esp32cam.local/stream            — MJPEG stream
//   esp32cam.local/enroll?name=X     — enroll next detected face as X
//   esp32cam.local/faces             — list enrolled names
//   esp32cam.local/delete?name=X     — remove face X
//   esp32cam.local/log               — recent recognition events
//
// TODO: implement WiFi connect, HTTP server, recognition pipeline, SPIFFS mount
// See plan: ~/.claude/plans/snug-cuddling-hedgehog.md

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"

// Camera pins — XIAO ESP32S3 Sense OV2640
#define PIR_PIN         GPIO_NUM_1
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

static const char* TAG = "recognition";

static void go_to_sleep(void) {
    ESP_LOGI(TAG, "Going to sleep — waiting for PIR...");
    rtc_gpio_pulldown_en(PIR_PIN);
    rtc_gpio_pullup_dis(PIR_PIN);
    esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);   // keepalive for power bank
    esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);
    esp_deep_sleep_start();
}

extern "C" void app_main(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Wake: PIR motion");
        // TODO: init camera (QVGA RGB565)
        // TODO: mount SPIFFS for face database
        // TODO: connect WiFi + start HTTP server
        // TODO: run esp-who recognition pipeline for 30s
        // TODO: deinit and sleep
    } else {
        ESP_LOGI(TAG, "Wake: timer keepalive or first boot — sleeping");
    }

    go_to_sleep();
}
