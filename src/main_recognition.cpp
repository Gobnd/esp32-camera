// main_recognition.cpp — PIR-triggered face recognition firmware
//
// Framework: ESP-IDF (not Arduino) — see [env:recognition] in platformio.ini
// Upload: pio run -e recognition --target upload
//
// Behaviour: deep sleep until PIR fires, then:
//   - init camera (QVGA JPEG)
//   - mount SD  → saves 1fps frames labelled with name to /sdcard/captures/<ts>/
//   - mount SPIFFS → face embeddings + person counter persist across sleep cycles
//   - connect WiFi (SD saving works without it)
//   - run detection + recognition for 30s:
//       known face  → labelled with enrolled name
//       unknown face → auto-enrolled as person_001, person_002, etc.
//   - deep sleep
//
// Endpoints (when WiFi available):
//   esp32cam.local/enroll?name=X       — next detected face enrolled as X (overrides auto)
//   esp32cam.local/rename?from=X&to=Y  — rename an existing enrolled person
//   esp32cam.local/faces               — list all enrolled names
//   esp32cam.local/log                 — recent recognition events
//
// NOTE: esp-who headers (human_face_detect_*.hpp, face_recognition_*.hpp) must
// match the version pulled by PlatformIO. Adjust if build fails.
// Two TODOs remain: recognizer.load() and recognizer.save() need esp-who version API.

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "driver/rtc_io.h"
#include "driver/spi_common.h"
#include "driver/sdspi_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_spiffs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "human_face_detect_msr01.hpp"
#include "human_face_detect_mnp01.hpp"
#include "face_recognition_tool.hpp"
#include "face_recognition_112_v1_s8.hpp"

static const char *TAG = "recognition";

// ─── Camera pins (XIAO ESP32S3 Sense OV2640) ─────────────────────────────
#define PIR_PIN        GPIO_NUM_1
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

// ─── SD card pins ────────────────────────────────────────────────────────
#define SD_CS   GPIO_NUM_21
#define SD_CLK  GPIO_NUM_8
#define SD_MISO GPIO_NUM_9
#define SD_MOSI GPIO_NUM_10

// ─── Constants ───────────────────────────────────────────────────────────
#define ACTIVE_SECS      30
#define SAVE_INTERVAL_US (1000000LL)
#define SD_MOUNT         "/sdcard"
#define SPIFFS_BASE      "/spiffs"
#define FACE_DB_PATH     SPIFFS_BASE "/face_db.bin"
#define PERSON_CTR_PATH  SPIFFS_BASE "/person_ctr.txt"

// ─── WiFi networks ───────────────────────────────────────────────────────
typedef struct { const char *ssid; const char *pass; } network_t;
static const network_t NETWORKS[] = {
    { "SenSen2",         "wongabongamcdonga" },
    { "Home2.4g",        "18lookoutway"      },
    { "Gobind's iPhone", "12345678"          },
};

// ─── Global state ────────────────────────────────────────────────────────
static RTC_DATA_ATTR int boot_count = 0;
static sdmmc_card_t *s_card = NULL;
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

// Pending manual enroll name — set via /enroll?name=X, consumed on next face detect
static char s_enroll_name[64] = {};

// ─── Person counter (persisted in SPIFFS) ────────────────────────────────
static int load_person_counter(void) {
    FILE *f = fopen(PERSON_CTR_PATH, "r");
    if (!f) return 1;
    int n = 1;
    fscanf(f, "%d", &n);
    fclose(f);
    return n;
}

static void save_person_counter(int n) {
    FILE *f = fopen(PERSON_CTR_PATH, "w");
    if (f) { fprintf(f, "%d", n); fclose(f); }
}

// ─── Deep sleep ──────────────────────────────────────────────────────────
static void go_to_sleep(void) {
    ESP_LOGI(TAG, "Going to sleep — waiting for PIR...");
    esp_camera_deinit();
    rtc_gpio_pulldown_en(PIR_PIN);
    rtc_gpio_pullup_dis(PIR_PIN);
    esp_sleep_enable_timer_wakeup(30ULL * 1000000ULL);
    esp_sleep_enable_ext0_wakeup(PIR_PIN, 1);
    esp_deep_sleep_start();
}

// ─── Camera ──────────────────────────────────────────────────────────────
static bool init_camera(void) {
    camera_config_t cfg = {};
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
    cfg.frame_size   = FRAMESIZE_QVGA;
    cfg.jpeg_quality = 12;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;
    return esp_camera_init(&cfg) == ESP_OK;
}

// ─── SD card ─────────────────────────────────────────────────────────────
static bool mount_sd(void) {
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = SD_MOSI,
        .miso_io_num   = SD_MISO,
        .sclk_io_num   = SD_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t ret = spi_bus_initialize((spi_host_device_t)host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return false;
    }
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS;
    slot_cfg.host_id = (spi_host_device_t)host.slot;
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        return false;
    }
    ESP_LOGI(TAG, "SD ready");
    return true;
}

static void make_event_folder(char *folder, size_t len) {
    time_t now; struct tm t;
    time(&now); localtime_r(&now, &t);
    if (t.tm_year > 100)
        strftime(folder, len, SD_MOUNT "/captures/%Y-%m-%d_%H-%M-%S", &t);
    else
        snprintf(folder, len, SD_MOUNT "/captures/wake_%04d", boot_count);
    mkdir(SD_MOUNT "/captures", 0775);
    mkdir(folder, 0775);
    ESP_LOGI(TAG, "Folder: %s", folder);
}

static void save_frame(camera_fb_t *fb, const char *folder, int n, const char *label) {
    char path[96];
    snprintf(path, sizeof(path), "%s/frame_%03d_%s.jpg", folder, n, label);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(fb->buf, 1, fb->len, f); fclose(f); }
}

// ─── SPIFFS ──────────────────────────────────────────────────────────────
static bool mount_spiffs(void) {
    esp_vfs_spiffs_conf_t cfg = {
        .base_path             = SPIFFS_BASE,
        .partition_label       = NULL,
        .max_files             = 5,
        .format_if_mount_failed = true,
    };
    return esp_vfs_spiffs_register(&cfg) == ESP_OK;
}

// ─── WiFi ────────────────────────────────────────────────────────────────
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
}

static bool wifi_connect(void) {
    s_wifi_eg = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    for (size_t i = 0; i < sizeof(NETWORKS) / sizeof(NETWORKS[0]); i++) {
        wifi_config_t wcfg = {};
        strncpy((char *)wcfg.sta.ssid,     NETWORKS[i].ssid, sizeof(wcfg.sta.ssid) - 1);
        strncpy((char *)wcfg.sta.password, NETWORKS[i].pass, sizeof(wcfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &wcfg);
        esp_wifi_start();
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi: %s", NETWORKS[i].ssid);
            return true;
        }
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    ESP_LOGI(TAG, "WiFi failed — SD only");
    return false;
}

// ─── HTTP server ─────────────────────────────────────────────────────────
// GET /enroll?name=Gobind  → queue name for next detected face
static esp_err_t handle_enroll(httpd_req_t *req) {
    char name[64] = {};
    if (httpd_req_get_url_query_str(req, name, sizeof(name)) == ESP_OK) {
        char val[64] = {};
        if (httpd_query_key_value(name, "name", val, sizeof(val)) == ESP_OK) {
            strncpy(s_enroll_name, val, sizeof(s_enroll_name) - 1);
            ESP_LOGI(TAG, "Enroll queued: %s", s_enroll_name);
            httpd_resp_sendstr(req, "OK — stand in front of camera");
            return ESP_OK;
        }
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Usage: /enroll?name=X");
    return ESP_OK;
}

// GET /rename?from=person_001&to=Gobind  → rename an enrolled person
static esp_err_t handle_rename(httpd_req_t *req) {
    char query[128] = {};
    char from[64] = {}, to[64] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "from", from, sizeof(from)) == ESP_OK &&
        httpd_query_key_value(query, "to",   to,   sizeof(to))   == ESP_OK) {
        // TODO: call recognizer.rename(from, to) — API depends on esp-who version
        ESP_LOGI(TAG, "Rename: %s → %s", from, to);
        httpd_resp_sendstr(req, "OK");
        return ESP_OK;
    }
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Usage: /rename?from=X&to=Y");
    return ESP_OK;
}

// GET /faces  → list all enrolled names, one per line
static esp_err_t handle_faces(httpd_req_t *req) {
    // TODO: iterate recognizer.get_enrolled_names() — API depends on esp-who version
    httpd_resp_sendstr(req, "(TODO: list enrolled faces)");
    return ESP_OK;
}

static httpd_handle_t start_http_server(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) return NULL;
    httpd_uri_t uris[] = {
        { "/enroll", HTTP_GET, handle_enroll, NULL },
        { "/rename", HTTP_GET, handle_rename, NULL },
        { "/faces",  HTTP_GET, handle_faces,  NULL },
    };
    for (auto &u : uris) httpd_register_uri_handler(server, &u);
    ESP_LOGI(TAG, "HTTP ready: esp32cam.local/enroll?name=X");
    return server;
}

// ─── Active window ───────────────────────────────────────────────────────
static void run_active_window(void) {
    bool sd_ok     = mount_sd();
    bool spiffs_ok = mount_spiffs();

    bool wifi_ok = false;
    if (nvs_flash_init() == ESP_OK)
        wifi_ok = wifi_connect();
    if (wifi_ok) {
        esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        vTaskDelay(pdMS_TO_TICKS(3000));
        start_http_server();
    }

    char folder[64] = {};
    int  frame_n   = 0;
    if (sd_ok) make_event_folder(folder, sizeof(folder));

    // Load face models and persisted database
    HumanFaceDetectMSR01   detector1(0.3F, 0.3F, 10, 0.3F);
    HumanFaceDetectMNP01   detector2(0.4F, 0.3F, 10);
    FaceRecognition112V1S8 recognizer;
    int person_ctr = 1;
    if (spiffs_ok) {
        person_ctr = load_person_counter();
        FILE *f = fopen(FACE_DB_PATH, "rb");
        if (f) {
            // TODO: recognizer.load(f);
            fclose(f);
            ESP_LOGI(TAG, "Face DB loaded (person counter: %d)", person_ctr);
        }
    }

    int64_t deadline  = esp_timer_get_time() + (ACTIVE_SECS * 1000000LL);
    int64_t last_save = 0;

    while (esp_timer_get_time() < deadline) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

        const char *label = "no_face";

        // Detect faces in frame
        // NOTE: camera is in JPEG mode; detection expects RGB565.
        // TODO: decode JPEG → RGB565 before inference (esp_jpeg_decode_one_picture)
        // For now passes raw buffer — adjust when testing.
        std::list<dl::detect::result_t> candidates =
            detector1.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3});

        if (!candidates.empty()) {
            std::list<dl::detect::result_t> results =
                detector2.infer((uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3}, candidates);

            if (!results.empty()) {
                // Manual enroll takes priority over auto-enroll
                if (s_enroll_name[0] != '\0') {
                    // TODO: recognizer.enroll(fb->buf, {h,w,3}, keypoint, s_enroll_name);
                    ESP_LOGI(TAG, "Enrolled (manual): %s", s_enroll_name);
                    label = s_enroll_name;
                    s_enroll_name[0] = '\0';
                } else {
                    face_info_t info = recognizer.recognize(
                        (uint16_t *)fb->buf, {(int)fb->height, (int)fb->width, 3},
                        results.front().keypoint);

                    if (info.id >= 0) {
                        // Known person
                        label = info.name.c_str();
                        ESP_LOGI(TAG, "Recognized: %s", label);
                    } else {
                        // Unknown — auto-enroll as person_XXX
                        static char auto_name[32];
                        snprintf(auto_name, sizeof(auto_name), "person_%03d", person_ctr++);
                        // TODO: recognizer.enroll(fb->buf, {h,w,3}, keypoint, auto_name);
                        ESP_LOGI(TAG, "New person auto-enrolled: %s", auto_name);
                        label = auto_name;
                        if (spiffs_ok) save_person_counter(person_ctr);
                    }
                }
            }
        }

        // Save 1fps JPEG to SD, filename includes recognition label
        int64_t now = esp_timer_get_time();
        if (sd_ok && (now - last_save) >= SAVE_INTERVAL_US) {
            save_frame(fb, folder, frame_n++, label);
            last_save = now;
        }

        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Event complete — %d frames saved to %s", frame_n, folder);

    // Persist updated face database and person counter
    if (spiffs_ok) {
        FILE *f = fopen(FACE_DB_PATH, "wb");
        if (f) {
            // TODO: recognizer.save(f);
            fclose(f);
        }
        save_person_counter(person_ctr);
    }

    if (wifi_ok) esp_wifi_stop();
}

// ─── Entry point ─────────────────────────────────────────────────────────
extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    boot_count++;
    ESP_LOGI(TAG, "=== Wake #%d ===", boot_count);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "Wake cause: PIR motion");
        if (!init_camera()) {
            ESP_LOGE(TAG, "Camera init failed");
            go_to_sleep();
            return;
        }
        { camera_fb_t *s = esp_camera_fb_get(); if (s) esp_camera_fb_return(s); }
        run_active_window();
    } else if (cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
        ESP_LOGI(TAG, "First boot — sleeping");
    } else {
        ESP_LOGI(TAG, "Timer keepalive — sleeping");
    }

    go_to_sleep();
}
