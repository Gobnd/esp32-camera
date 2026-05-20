#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "CStreamer.h"
#include "CRtspSession.h"

struct Network { const char* ssid; const char* pass; };
static const Network NETWORKS[] = {
    { "SenSen2",        "wongabongamcdonga" },
    { "Home2.4g",       "18lookoutway"      },
    { "Gobind's iPhone","12345678"          },
};

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

// Grove PIR motion sensor — connect signal wire to D0 on the XIAO
#define PIR_PIN         1

WiFiServer tcpServer(80);

unsigned long ledOffMs = 0;   // millis() when LED should turn back off (0 = already off)

volatile uint32_t totalFramesSent = 0;
volatile int   frameDelayMs = 0;
volatile int   jpegQuality  = 20;   // 0–63, lower = better quality & bigger file

struct MotionCapture {
    uint8_t*  buf       = nullptr;
    size_t    len       = 0;
    uint32_t  ms        = 0;        // millis() at capture
    uint32_t  latencyMs = 0;        // PIR rising edge → JPEG ready
    bool      inFrame   = false;    // JPEG size exceeded 1.25× quiet baseline
};
static const int   MOTION_SLOTS      = 3;
static const float MOTION_RATIO_MIN  = 1.10f;   // JPEG must be >10% bigger than baseline
static const float MOTION_ABS_MIN    = 5120.0f; // AND >5 KB bigger in absolute terms
static const char* SLOT_LABEL[]      = { "Most Recent", "2nd", "3rd" };

SemaphoreHandle_t motionMutex    = nullptr;
MotionCapture     motionSlots[MOTION_SLOTS];
int               motionHead     = 0;   // index where NEXT write goes
int               motionCount    = 0;   // slots filled so far (0–3)
bool              lastPirState   = false;
unsigned long     pirFellMs      = 0;   // millis() when PIR last went LOW
volatile float    baselineJpegLen = 20000.0f;  // EMA of quiet-scene JPEG size

// n=0 → most recent, n=1 → second, n=2 → oldest
inline int motionSlotIdx(int n) {
    return (motionHead - 1 - n + MOTION_SLOTS * 10) % MOTION_SLOTS;
}

// Extract a single value from a URL query string ("key=value&...")
String queryParam(const String& query, const char* key) {
    int si = query.indexOf(key);
    if (si < 0) return "";
    String val = query.substring(si + strlen(key));
    int amp = val.indexOf('&');
    return amp >= 0 ? val.substring(0, amp) : val;
}

struct ResOption { const char* id; const char* label; framesize_t size; uint16_t w; uint16_t h; };
static const ResOption RES_OPTIONS[] = {
    { "qvga", "240p  320&times;240",   FRAMESIZE_QVGA,  320,  240 },
    { "vga",  "480p  640&times;480",   FRAMESIZE_VGA,   640,  480 },
    { "svga", "SVGA  800&times;600",   FRAMESIZE_SVGA,  800,  600 },
    { "hd",   "720p  1280&times;720",  FRAMESIZE_HD,   1280,  720 },
    { "sxga", "SXGA  1280&times;1024", FRAMESIZE_SXGA, 1280, 1024 },
    { "uxga", "1.9MP 1600&times;1200", FRAMESIZE_UXGA, 1600, 1200 },
};
static const char* currentResId = "vga";
static uint16_t    currentW     = 640;
static uint16_t    currentH     = 480;

void setResolution(const String& id) {
    for (auto& opt : RES_OPTIONS) {
        if (id == opt.id) {
            sensor_t* s = esp_camera_sensor_get();
            s->set_framesize(s, opt.size);
            currentResId = opt.id;
            currentW     = opt.w;
            currentH     = opt.h;
            Serial.printf("Resolution -> %s\n", opt.label);
            return;
        }
    }
}

void setQuality(int q) {
    q = q < 4 ? 4 : q > 63 ? 63 : q;
    sensor_t* s = esp_camera_sensor_get();
    s->set_quality(s, q);
    jpegQuality = q;
    Serial.printf("JPEG quality -> %d\n", q);
}

void initCamera() {
    camera_config_t cfg = {};
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
    cfg.jpeg_quality = jpegQuality;
    cfg.fb_count     = 2;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;
    cfg.grab_mode    = CAMERA_GRAB_LATEST;

    if (esp_camera_init(&cfg) != ESP_OK) {
        Serial.println("Camera init failed");
        while (true) delay(1000);
    }
    Serial.println("Camera ready");
}

String readPath(WiFiClient& client) {
    String path = "/";
    String line = "";
    bool gotPath = false;
    unsigned long t = millis();

    while (client.connected() && millis() - t < 3000) {
        while (client.available()) {
            char c = client.read();
            if (c == '\r') continue;
            if (c == '\n') {
                if (!gotPath && line.startsWith("GET ")) {
                    int sp = line.indexOf(' ', 4);
                    path = (sp > 4) ? line.substring(4, sp) : line.substring(4);
                    gotPath = true;
                }
                if (line.length() == 0) return path;
                line = "";
            } else {
                line += c;
            }
        }
        delay(1);
    }
    return path;
}

String buildStatsHtml() {
    unsigned long upSec = millis() / 1000;
    int mhz = getCpuFrequencyMhz();
    int estMa = (mhz >= 240) ? 310 : (mhz >= 160) ? 240 : 200;
    float temp = temperatureRead();
    const char* tempNote = (temp < 60) ? "normal" : (temp < 75) ? "warm" : "hot";
    uint32_t now = millis();

    // Build motion rows outside the string concat so we can use loops
    String motionRows = "<tr><td>Motion Captures</td><td>" + String(motionCount) + " / " + String(MOTION_SLOTS) + "</td></tr>";
    for (int i = 0; i < motionCount; i++) {
        MotionCapture& cap = motionSlots[motionSlotIdx(i)];
        motionRows += "<tr><td>&nbsp;&nbsp;" + String(SLOT_LABEL[i]) + "</td><td>";
        motionRows += String((now - cap.ms) / 1000) + "s ago | ";
        motionRows += (cap.inFrame ? "<b>in-frame</b>" : "off-camera");
        motionRows += " | " + String(cap.latencyMs) + "ms latency";
        motionRows += "</td></tr>";
    }
    if (motionCount >= 2) {
        uint32_t span = motionSlots[motionSlotIdx(0)].ms - motionSlots[motionSlotIdx(motionCount - 1)].ms;
        float perMin = span > 0 ? (float)(motionCount - 1) * 60000.0f / (float)span : 0;
        motionRows += "<tr><td>Frequency</td><td>" + String(motionCount) + " events in " +
                      String(span / 1000) + "s (" + String(perMin, 1) + "/min)</td></tr>";
    }
    motionRows += "<tr><td>JPEG Baseline</td><td>" + String((int)baselineJpegLen / 1024) + " KB (quiet scene avg)</td></tr>";

    String html = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='3'>"
        "<style>body{font-family:sans-serif;padding:20px}table{border-collapse:collapse}"
        "td{padding:8px 16px;border-bottom:1px solid #ddd}</style></head><body>"
        "<h2>XIAO ESP32S3 Stats</h2><table>"
        "<tr><td>Uptime</td><td>"       + String(upSec/3600) + "h " + String((upSec%3600)/60) + "m " + String(upSec%60) + "s</td></tr>"
        "<tr><td>CPU</td><td>"          + String(mhz) + " MHz</td></tr>"
        "<tr><td>Free RAM</td><td>"     + String(ESP.getFreeHeap()/1024) + " / " + String(ESP.getHeapSize()/1024) + " KB</td></tr>"
        "<tr><td>Free PSRAM</td><td>"   + String(ESP.getFreePsram()/1024) + " / " + String(ESP.getPsramSize()/1024) + " KB</td></tr>"
        "<tr><td>Chip Temp</td><td>"    + String(temp, 1) + " &deg;C &mdash; <em>" + String(tempNote) + "</em>"
        "<br><small style='color:#888'>Max safe: 85 &deg;C. Camera+WiFi streaming at 50&ndash;60 &deg;C is normal.</small></td></tr>"
        "<tr><td>WiFi Signal</td><td>"  + String(WiFi.RSSI()) + " dBm</td></tr>"
        "<tr><td>IP</td><td>"           + WiFi.localIP().toString() + "</td></tr>"
        "<tr><td>RTSP URL</td><td>rtsp://" + WiFi.localIP().toString() + ":8554/mjpeg/1</td></tr>"
        "<tr><td>Resolution</td><td>"   + String(currentResId) + "</td></tr>"
        "<tr><td>JPEG Quality</td><td>" + String(jpegQuality) + " (0=best/large &rarr; 63=worst/small)</td></tr>"
        "<tr><td>Stream FPS</td><td>"   + String(millis() > 0 ? totalFramesSent * 1000.0f / (float)millis() : 0.0f, 1) + " fps (HTTP, all clients)</td></tr>"
        "<tr><td>Frame Delay</td><td>"  + String(frameDelayMs) + " ms</td></tr>"
        + motionRows +
        "<tr><td>Est. Current</td><td>~" + String(estMa) + " mA (&plusmn;30%, 3.3&thinsp;V rail)"
        "<br><small style='color:#888'>Accurate measurement needs an INA219 sensor</small></td></tr>"
        "</table><br><a href='/'>Back</a></body></html>";
    return html;
}

String buildRootHtml() {
    String ip = WiFi.localIP().toString();
    String html =
        "<html><head><meta charset='utf-8'>"
        "<style>"
        "body{font-family:sans-serif;display:flex;gap:32px;padding:20px;margin:0}"
        "nav{display:flex;flex-direction:column;gap:8px;min-width:180px;flex-shrink:0}"
        "a.btn{padding:10px;background:#222;color:#fff;text-decoration:none;"
        "border-radius:4px;text-align:center;font-size:14px}"
        "a.btn:hover{background:#444}"
        "hr{border:none;border-top:1px solid #ccc;margin:4px 0}"
        "form{margin:0}"
        ".rb{width:100%;padding:7px;margin:2px 0;cursor:pointer;border:1px solid #bbb;"
        "border-radius:4px;font-size:13px;background:#f5f5f5;text-align:left}"
        ".rb.active{background:#080;color:#fff;border-color:#080}"
        ".rb:hover:not(.active){background:#ddd}"
        "h3{margin:0 0 8px}"
        // Fixed 640x480 box — stream fills it via object-fit regardless of resolution
        "#lv{width:640px;height:480px;object-fit:contain;background:#000;"
        "border:1px solid #ccc;display:block}"
        ".rtsp{font-size:11px;color:#555;word-break:break-all;margin-top:4px}"
        "</style></head><body>"
        "<nav>"
        "<h3>Camera</h3>"
        "<a class='btn' href='/stream'>&#9654; Raw Stream (VLC)</a>"
        "<a class='btn' href='/photo'>&#128247; Still Photo</a>"
        "<a class='btn' href='/motions'>&#128248; Motion Gallery (last 3)</a>"
        "<a class='btn' href='/stats'>&#128200; Stats</a>"
        "<a class='btn' href='/restart' style='background:#800'>&#128260; Restart</a>"
        "<p class='rtsp'>RTSP: rtsp://" + ip + ":8554/mjpeg/1</p>"
        "<hr>"
        "<b>Resolution</b><br>";

    for (auto& opt : RES_OPTIONS) {
        bool active = (strcmp(opt.id, currentResId) == 0);
        html += "<form method='get' action='/res'>"
                "<input type='hidden' name='id' value='" + String(opt.id) + "'>"
                "<button class='rb" + (active ? " active" : "") + "'>" + String(opt.label) + "</button>"
                "</form>";
    }

    // Quality presets: lower q = better quality & bigger file & lower fps
    const int   quals[]      = {  10,    20,     35     };
    const char* qualLabels[] = { "Best", "Good", "Fast" };
    html += "<hr><b>Quality / FPS</b><br>";
    for (int i = 0; i < 3; i++) {
        bool active = (jpegQuality == quals[i]);
        html += "<form method='get' action='/quality'>"
                "<input type='hidden' name='q' value='" + String(quals[i]) + "'>"
                "<button class='rb" + (active ? " active" : "") + "'>" + String(qualLabels[i]) + "</button>"
                "</form>";
    }

    // Frame delay throttle
    const int   delays[]      = {  0,         33,        66,        200      };
    const char* delayLabels[] = { "Max FPS", "~30 fps", "~15 fps", "~5 fps" };
    html += "<hr><b>Frame Rate Cap</b><br>";
    for (int i = 0; i < 4; i++) {
        bool active = (frameDelayMs == delays[i]);
        html += "<form method='get' action='/fps'>"
                "<input type='hidden' name='delay' value='" + String(delays[i]) + "'>"
                "<button class='rb" + (active ? " active" : "") + "'>" + String(delayLabels[i]) + "</button>"
                "</form>";
    }

    html += "</nav>"
            "<div style='flex:1;min-width:0'>"
            "<img id='lv' src='/stream'><br>"
            "<small style='color:#888'>Live &mdash; quality and resolution apply within 1&ndash;2 frames. "
            "Open <em>Raw Stream</em> or use the RTSP URL in VLC for lower latency.</small>"
            "</div>"
            "</body></html>";
    return html;
}

// ── RTSP support ────────────────────────────────────────────────────────────

// Wraps esp_camera_fb_get() into the Micro-RTSP CStreamer interface.
// streamFrame() (protected in CStreamer) handles all RTP packetisation.
class EspCamStreamer : public CStreamer {
public:
    EspCamStreamer(uint16_t w, uint16_t h) : CStreamer(w, h) {}

    void streamImage(uint32_t msec) override {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) return;
        streamFrame(fb->buf, fb->len, msec);
        esp_camera_fb_return(fb);
    }
};

// One RTSP client at a time.  handleRequests() auto-deletes stopped sessions.
void rtspTask(void*) {
    WiFiServer rtspServer(8554);
    rtspServer.begin();
    Serial.printf("RTSP ready: rtsp://%s:8554/mjpeg/1\n", WiFi.localIP().toString().c_str());

    WiFiClient client;   // lives here so &client stays valid for the session lifetime

    while (true) {
        client = rtspServer.accept();
        if (client) {
            // Recreate the streamer per-connection so it picks up the current resolution.
            EspCamStreamer streamer(currentW, currentH);
            streamer.setURI(WiFi.localIP().toString() + ":8554");
            streamer.addSession(&client);

            uint32_t lastFrame = millis();
            while (client.connected() && streamer.anySessions()) {
                streamer.handleRequests(0); // processes RTSP commands, removes stopped sessions
                uint32_t now = millis();
                if (now - lastFrame >= 66) {    // ~15 fps
                    streamer.streamImage(now);
                    lastFrame = now;
                }
                delay(1);
            }
        }
        delay(10);
    }
}

// ── HTTP connection handler ──────────────────────────────────────────────────

void handleConnection(void* param) {
    WiFiClient* ptr = (WiFiClient*)param;
    WiFiClient client = *ptr;
    delete ptr;

    String fullPath = readPath(client);
    String reqPath = fullPath;
    String query = "";
    int q = fullPath.indexOf('?');
    if (q >= 0) {
        reqPath = fullPath.substring(0, q);
        query   = fullPath.substring(q + 1);
    }

    if (reqPath == "/stream") {
        client.setNoDelay(true);    // disable Nagle — send each frame immediately
        client.println("HTTP/1.1 200 OK");
        client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
        client.println("Connection: keep-alive");
        client.println();

        unsigned long fpsWindowStart = millis();
        int fpsFrameCount = 0;

        while (client.connected()) {
            camera_fb_t* fb = esp_camera_fb_get();
            if (!fb) { delay(50); continue; }

            client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
            client.write(fb->buf, fb->len);
            client.print("\r\n");
            esp_camera_fb_return(fb);

            totalFramesSent++;

            if (frameDelayMs > 0) delay(frameDelayMs);
        }

    } else if (reqPath == "/photo") {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
            client.write(fb->buf, fb->len);
            esp_camera_fb_return(fb);
        } else {
            client.print("HTTP/1.1 500 Internal Server Error\r\n\r\nCapture failed");
        }

    } else if (reqPath == "/motion") {
        // ?n=0 = most recent, ?n=1 = second, ?n=2 = oldest
        int n = constrain(queryParam(query, "n=").toInt(), 0, MOTION_SLOTS - 1);

        if (motionMutex && xSemaphoreTake(motionMutex, pdMS_TO_TICKS(2000))) {
            if (n < motionCount) {
                int idx = motionSlotIdx(n);
                MotionCapture& cap = motionSlots[idx];
                if (cap.buf && cap.len > 0) {
                    client.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", cap.len);
                    client.write(cap.buf, cap.len);
                } else {
                    client.print("HTTP/1.1 503 Service Unavailable\r\n\r\nSlot empty");
                }
            } else {
                client.print("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n\r\n"
                             "No motion photo yet — wave in front of the PIR sensor on D0");
            }
            xSemaphoreGive(motionMutex);
        } else {
            client.print("HTTP/1.1 503 Service Unavailable\r\n\r\nMutex timeout");
        }

    } else if (reqPath == "/motions") {
        // Gallery page: last 3 motion captures with timestamps and frequency summary.
        String pg = "<html><head><meta charset='utf-8'>"
                    "<meta http-equiv='refresh' content='5'>"
                    "<style>body{font-family:sans-serif;padding:20px}"
                    ".row{display:flex;gap:24px;flex-wrap:wrap;margin-bottom:24px}"
                    ".card{border:1px solid #ccc;border-radius:6px;padding:12px;min-width:220px}"
                    "img{width:300px;height:auto;display:block;border-radius:4px}"
                    ".badge{display:inline-block;padding:2px 8px;border-radius:3px;font-size:12px;font-weight:bold}"
                    ".in{background:#c8f0c8;color:#1a6b1a}.out{background:#f0e0c8;color:#7a4a00}"
                    "</style></head><body>"
                    "<h2>Motion Gallery</h2>";

        if (motionCount == 0) {
            pg += "<p>No captures yet — wave in front of the PIR sensor on D0.</p>";
        } else {
            // Frequency summary
            if (motionCount >= 2) {
                uint32_t span = motionSlots[motionSlotIdx(0)].ms - motionSlots[motionSlotIdx(motionCount - 1)].ms;
                float perMin = span > 0 ? (float)(motionCount - 1) * 60000.0f / (float)span : 0;
                pg += "<p><b>" + String(motionCount) + " events in " + String(span / 1000) +
                      "s &mdash; " + String(perMin, 1) + " triggers/min</b></p>";
            }

            pg += "<div class='row'>";
            uint32_t now = millis();
            for (int i = 0; i < motionCount; i++) {
                MotionCapture& cap = motionSlots[motionSlotIdx(i)];
                String badge = cap.inFrame
                    ? "<span class='badge in'>IN FRAME</span>"
                    : "<span class='badge out'>off-camera</span>";
                pg += "<div class='card'>"
                      "<p><b>" + String(SLOT_LABEL[i]) + "</b> &mdash; " + String((now - cap.ms) / 1000) + "s ago</p>"
                      "<img src='/motion?n=" + String(i) + "' alt='motion'><br>"
                      + badge +
                      " &nbsp; latency: " + String(cap.latencyMs) + "ms"
                      " &nbsp; size: " + String(cap.len / 1024) + "KB"
                      "</div>";
            }
            pg += "</div>";
        }

        pg += "<p><small>Auto-refreshes every 5s &mdash; "
              "<a href='/motions'>refresh now</a> | <a href='/'>home</a></small></p>"
              "</body></html>";

        client.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n\r\n", pg.length());
        client.print(pg);

    } else if (reqPath == "/res") {
        String id = queryParam(query, "id=");
        if (id.length()) setResolution(id);
        client.print("HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n");

    } else if (reqPath == "/quality") {
        String val = queryParam(query, "q=");
        if (val.length()) setQuality(val.toInt());
        client.print("HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n");

    } else if (reqPath == "/fps") {
        String val = queryParam(query, "delay=");
        if (val.length()) {
            frameDelayMs = constrain(val.toInt(), 0, 500);
            Serial.printf("Frame delay -> %d ms\n", frameDelayMs);
        }
        client.print("HTTP/1.1 302 Found\r\nLocation: /\r\n\r\n");

    } else if (reqPath == "/restart") {
        client.print("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                     "<html><body><p>Restarting&hellip; returning in 5s</p>"
                     "<script>setTimeout(()=>location='/',5000)</script></body></html>");
        client.stop();
        delay(100);     // let the TCP flush before reset
        ESP.restart();  // resets the chip — no further code runs

    } else if (reqPath == "/stats") {
        String html = buildStatsHtml();
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n\r\n", html.length());
        client.print(html);

    } else {
        String html = buildRootHtml();
        client.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n\r\n", html.length());
        client.print(html);
    }

    client.stop();
    vTaskDelete(NULL);
}

// ── Arduino entry points ─────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(3000);

    motionMutex = xSemaphoreCreateMutex();
    initCamera();
    pinMode(LED_BUILTIN, OUTPUT);
    pinMode(PIR_PIN, INPUT_PULLDOWN);   // PULLDOWN prevents spurious triggers when sensor not connected

    Serial.print("Connecting to WiFi");
    for (auto& net : NETWORKS) {
        WiFi.begin(net.ssid, net.pass);
        for (int i = 0; i < 16 && WiFi.status() != WL_CONNECTED; i++) {
            delay(500); Serial.print(".");
        }
        if (WiFi.status() == WL_CONNECTED) break;
        WiFi.disconnect();
    }
    Serial.printf("\nOpen: http://%s  (on %s)\n", WiFi.localIP().toString().c_str(), WiFi.SSID().c_str());

    // Capture an initial quiet-scene baseline so the first motion event has something to compare
    {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) { baselineJpegLen = (float)fb->len; esp_camera_fb_return(fb); }
    }

    tcpServer.begin();
    xTaskCreatePinnedToCore(rtspTask, "rtsp", 8192, NULL, 1, NULL, 0);
}

void loop() {
    // PIR + camera-based motion confirmation
    bool pirNow = digitalRead(PIR_PIN);

    static unsigned long lastCapMs = 0;
    if (pirNow && !lastPirState && (millis() - lastCapMs > 5000)) {
        lastCapMs = millis();
        // Rising edge: flash LED immediately so you can see the PIR fired
        digitalWrite(LED_BUILTIN, HIGH);
        ledOffMs = millis() + 200;

        // Discard the buffered frame so the next get() captures a fresh one
        { camera_fb_t* stale = esp_camera_fb_get(); if (stale) esp_camera_fb_return(stale); }
        uint32_t t0 = millis();
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            uint32_t latency = millis() - t0;
            float ratio    = (float)fb->len / baselineJpegLen;
            float absDelta = (float)fb->len - baselineJpegLen;
            bool inFrame   = (ratio > MOTION_RATIO_MIN) && (absDelta > MOTION_ABS_MIN);

            if (xSemaphoreTake(motionMutex, pdMS_TO_TICKS(500))) {
                MotionCapture& slot = motionSlots[motionHead];
                free(slot.buf);
                slot.buf       = (uint8_t*)ps_malloc(fb->len);
                slot.len       = fb->len;
                slot.ms        = millis();
                slot.latencyMs = latency;
                slot.inFrame   = inFrame;
                if (slot.buf) memcpy(slot.buf, fb->buf, fb->len);
                motionHead  = (motionHead + 1) % MOTION_SLOTS;
                motionCount = motionCount < MOTION_SLOTS ? motionCount + 1 : MOTION_SLOTS;
                xSemaphoreGive(motionMutex);
            }

            Serial.printf("Motion! slot %d/%d | %ums | JPEG %uB (%.2fx, +%.0fB) | %s\n",
                          motionCount, MOTION_SLOTS, latency, fb->len, ratio, absDelta,
                          inFrame ? "IN FRAME" : "off-camera");
            esp_camera_fb_return(fb);
        }

    } else if (!pirNow && lastPirState) {
        // Falling edge: PIR went quiet — schedule a baseline update in 2 seconds
        // (give the scene time to settle back to empty before sampling)
        pirFellMs = millis();
    }

    // 2 seconds after PIR goes quiet, snapshot the empty scene as the new baseline
    if (pirFellMs && !pirNow && (millis() - pirFellMs >= 2000)) {
        pirFellMs = 0;
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
            // Slow EMA — 70% old value + 30% new sample, adapts to lighting changes
            baselineJpegLen = 0.7f * baselineJpegLen + 0.3f * (float)fb->len;
            Serial.printf("Baseline updated: %.0f bytes\n", baselineJpegLen);
            esp_camera_fb_return(fb);
        }
    }

    lastPirState = pirNow;

    WiFiClient newClient = tcpServer.accept();
    if (newClient) {
        WiFiClient* ptr = new WiFiClient(newClient);
        xTaskCreatePinnedToCore(handleConnection, "http", 8192, ptr, 1, NULL, 0);
    }

    if (ledOffMs && millis() >= ledOffMs) {
        digitalWrite(LED_BUILTIN, LOW);
        ledOffMs = 0;
    }
}
