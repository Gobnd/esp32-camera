// Practice file — NOT compiled by PlatformIO (it's outside src/)
// When you want to test: copy this into src/main.cpp (rename old one first)

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

// STEP 1: write initCamera() here
void initCamera(){
    camera_config_t cfg {};

    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer = LEDC_TIMER_0;
    cfg.pin_d0 = Y2_GPIO_NUM;   // data bit 0
    cfg.pin_d1 = Y3_GPIO_NUM;
    cfg.pin_d2 = Y4_GPIO_NUM;
    cfg.pin_d3 = Y5_GPIO_NUM;
    cfg.pin_d4 = Y6_GPIO_NUM;
    cfg.pin_d5 = Y7_GPIO_NUM;
    cfg.pin_d6 = Y8_GPIO_NUM;
    cfg.pin_xclk = XCLK_GPIO_NUM;
    cfg.pin_pclk = PCLK_GPIO_NUM;
    

// STEP 2: write setup() here


// STEP 3: write loop() with edge detection here

