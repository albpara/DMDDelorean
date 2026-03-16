/*
 * DeLorean DMD — 128x32 HUB75 LED Matrix GIF Player
 *
 * Reads GIF file paths from /lista.txt on SD card, plays them sequentially
 * on a 128x32 HUB75 panel driven by ESP32-HUB75-MatrixPanel-DMA.
 *
 * Double-buffer strategy:
 *   - Two PSRAM buffers (A/B) hold entire GIF files in memory.
 *   - While buffer A plays (zero SD access), a FreeRTOS task on Core 0
 *     pre-loads the next GIF into buffer B.
 *   - On transition the buffers swap instantly — no freeze, no flicker.
 *   - Falls back to direct SD file-based playback when PSRAM is absent.
 *
 * Anti-ghosting settings ported from RetroPixelLED reference project.
 */

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <AnimatedGIF.h>
#include "components/mqtt.h"
#include "components/wifi_portal.h"

/* =================================================================
 *  PIN DEFINITIONS — HUB75 Panel
 * ================================================================= */
#define R1_PIN   25
#define G1_PIN   26
#define B1_PIN   27
#define R2_PIN   14
#define G2_PIN   12
#define B2_PIN   13
#define A_PIN    33
#define B_PIN    32
#define C_PIN    22
#define D_PIN    17
#define E_PIN    -1   // Tied to GND – 1/16 scan (32-row panel)
#define CLK_PIN  16
#define LAT_PIN   4
#define OE_PIN   15

/* PIN DEFINITIONS — SD Card (VSPI) */
#define SD_CS     5
#define SD_SCK   18
#define SD_MOSI  23
#define SD_MISO  19

/* =================================================================
 *  PANEL CONFIGURATION
 * ================================================================= */
#define PANEL_RES_X       64      // Width of one physical module
#define PANEL_RES_Y       32      // Height
#define PANEL_CHAIN        2      // Two 64x32 modules chained → 128x32
#define TOTAL_WIDTH       (PANEL_RES_X * PANEL_CHAIN)

/* =================================================================
 *  PLAYBACK CONFIGURATION
 * ================================================================= */
#define MAX_GIF_SIZE      (300 * 1024)  // 300 KB ceiling per GIF file
#define LISTA_PATH        "/lista.txt"
#define DEFAULT_BRIGHTNESS 25           // 0-255 (~10%)
#define MAX_BRIGHTNESS     80           // Hard cap — no dedicated PSU, protect ESP32

/* Anti-ghosting / display tuning (from RetroPixelLED reference) */
#define LATCH_BLANKING     1            // 1-4 — higher = less ghosting
#define MIN_REFRESH_HZ     90
#define I2S_CLK_SPEED      HUB75_I2S_CFG::HZ_10M
#define CLK_PHASE          false
#define RANDOM_PLAYBACK    true         // true = shuffle, false = sequential

/* =================================================================
 *  GLOBAL STATE
 * ================================================================= */
MatrixPanel_I2S_DMA *dma_display = nullptr;
AnimatedGIF gif;

// Playlist: compact offset table into /lista.txt (4 bytes per entry vs full path strings)
uint32_t *gifOffsets = nullptr;     // byte offset of each valid line in lista.txt (PSRAM when available)
int gifCount    = 0;
int currentIdx  = 0;

// Ping-pong PSRAM buffers for GIF preloading
uint8_t *gifBuf[2]    = {nullptr, nullptr};
size_t   gifBufCap[2]  = {0, 0};     // Actual allocated capacity
size_t   gifBufLen[2]  = {0, 0};
bool     gifBufOk[2]   = {false, false};
int      playBuf       = 0;       // Buffer currently playing
bool     hasDualBuf    = false;   // True when both buffers allocated
volatile bool clearBeforeNextGifDraw = false;

// Preload task coordination
TaskHandle_t      preloadHandle = nullptr;
TaskHandle_t      netTaskHandle = nullptr;
TaskHandle_t      playbackHandle = nullptr;
SemaphoreHandle_t sdMutex       = nullptr;
volatile int plFileIdx = -1;     // File index the preload task should load
volatile int plBufIdx  = -1;     // Buffer index to load into

// GIF centering offsets
int xOff = 0, yOff = 0;

// SD file handle for file-based fallback
File sdGifFile;

// Runtime copy of MAX_BRIGHTNESS for components
uint8_t MAX_BRIGHTNESS_VAL = MAX_BRIGHTNESS;

/* =================================================================
 *  PLAYLIST NAVIGATION — sequential or random (hardware RNG)
 * ================================================================= */
int nextIdx() {
    if (RANDOM_PLAYBACK && gifCount > 1)
        return (int)(esp_random() % (uint32_t)gifCount);
    return (currentIdx + 1) % gifCount;
}

/* =================================================================
 *  UTILITY — Show a single-line message on the panel
 *  Short text (fits on screen): centred.
 *  Long  text: starts at left edge, scrolls left until fully off, then returns.
 *  Optional font size (1–3) and rainbow colour effect.
 *  Blocking call — returns only when the full scroll has finished.
 * ================================================================= */
#define CHAR_W  6   // Adafruit GFX default 5x7 font + 1px gap = 6px per char at size 1
#define CHAR_H  8   // font height at size 1
#define SCROLL_STEP_MS  30   // ms per pixel scroll step
#define CLOCK_MIN_DISPLAY_SECONDS 10
#define NTP_RETRY_MS 30000

/* Convert hue (0–359°) to RGB565 — full saturation and value (HSV with S=V=1) */
static uint16_t hue565(int hue) {
    hue = ((hue % 360) + 360) % 360;
    int   hi = hue / 60;
    uint8_t f = (uint8_t)(255 * (hue % 60) / 60);
    uint8_t r, g, b;
    switch (hi) {
        case 0: r=255; g=f;     b=0;     break;
        case 1: r=255-f; g=255; b=0;     break;
        case 2: r=0;   g=255;   b=f;     break;
        case 3: r=0;   g=255-f; b=255;   break;
        case 4: r=f;   g=0;     b=255;   break;
        default:r=255; g=0;     b=255-f; break;
    }
    return dma_display ? dma_display->color565(r, g, b) : 0xFFFF;
}

/* Draw each character of msg in a different rainbow hue, starting at (x, y) */
static void drawRainbowText(const char *msg, int x, int y, uint8_t size, int hueShift) {
    int cw  = CHAR_W * size;
    int len = (int)strlen(msg);
    if (len == 0) return;
    dma_display->setTextSize(size);
    dma_display->setTextWrap(false);
    for (int i = 0; i < len; i++) {
        int hue = (hueShift + i * (360 / len)) % 360;
        dma_display->setTextColor(hue565(hue));
        dma_display->setCursor(x + i * cw, y);
        dma_display->print((char)msg[i]);
    }
}

void showMessage(const char *msg, uint16_t color, uint8_t size) {
    if (!dma_display) return;
    if (size < 1) size = 1;
    if (size > 3) size = 3;

    int cw    = CHAR_W * size;
    int ch    = CHAR_H * size;
    int len   = (int)strlen(msg);
    int textW = len * cw;
    int cy    = (PANEL_RES_Y - ch) / 2;

    dma_display->setTextSize(size);
    dma_display->setTextWrap(false);
    dma_display->setTextColor(color);

    if (textW <= TOTAL_WIDTH) {
        int cx = (TOTAL_WIDTH - textW) / 2;
        dma_display->fillScreen(0);
        dma_display->setCursor(cx, cy);
        dma_display->print(msg);
        return;
    }

    // Text wider than panel — scroll from left edge to fully off-screen
    int startX = 0;
    int endX   = -textW;

    for (int x = startX; x >= endX; x--) {
        dma_display->fillScreen(0);
        dma_display->setCursor(x, cy);
        dma_display->print(msg);
        delay(SCROLL_STEP_MS);
    }
}

/* =================================================================
 *  UTILITY — Show a text notification on the panel.
 *  Supports fixed display or scrolling, plain colour or rainbow.
 *  duration: if text is wider than panel, number of scroll loops.
 *            otherwise, display time in seconds.
 *  Non-blocking for networking: WiFi/MQTT runs in a separate Core 0 task.
 * ================================================================= */
void showNotification(const char *msg, uint16_t color, uint8_t size,
                      bool rainbow, uint32_t duration) {
    if (!dma_display || !msg || msg[0] == '\0') return;
    if (size < 1) size = 1;
    if (size > 3) size = 3;

    int cw    = CHAR_W * size;
    int ch    = CHAR_H * size;
    int len   = (int)strlen(msg);
    int textW = len * cw;
    int cy    = (PANEL_RES_Y - ch) / 2;
    bool wide = (textW > TOTAL_WIDTH);
    int hueShift = 0;

    if (wide) {
        // duration means number of full right-to-left scroll loops.
        uint32_t loops = (duration == 0) ? 1 : duration;
        for (uint32_t l = 0; l < loops; l++) {
            for (int x = TOTAL_WIDTH; x >= -textW; x--) {
                dma_display->fillScreen(0);
                if (rainbow) {
                    drawRainbowText(msg, x, cy, size, hueShift);
                    hueShift = (hueShift + 8) % 360;
                } else {
                    dma_display->setTextSize(size);
                    dma_display->setTextWrap(false);
                    dma_display->setTextColor(color);
                    dma_display->setCursor(x, cy);
                    dma_display->print(msg);
                }
                delay(SCROLL_STEP_MS);
            }
        }
        return;
    }

    // Non-scrolling text: duration is interpreted as seconds.
    uint32_t seconds = (duration == 0) ? 1 : duration;
    uint32_t durationMs = seconds * 1000UL;
    unsigned long start = millis();
    int cx = (TOTAL_WIDTH - textW) / 2;

    while (millis() - start < durationMs) {
        dma_display->fillScreen(0);
        if (rainbow) {
            drawRainbowText(msg, cx, cy, size, hueShift);
            hueShift = (hueShift + 8) % 360;
        } else {
            dma_display->setTextSize(size);
            dma_display->setTextWrap(false);
            dma_display->setTextColor(color);
            dma_display->setCursor(cx, cy);
            dma_display->print(msg);
        }
        delay(SCROLL_STEP_MS);
    }
}

/* =================================================================
 *  CLOCK MODE — NTP + render helpers
 * ================================================================= */
static bool ntpConfigured = false;
static unsigned long ntpLastAttempt = 0;
static uint32_t gifsSinceClock = 0;

static int textWidthPx(const char *s, uint8_t size) {
    return (int)strlen(s) * CHAR_W * size;
}

static bool haveSyncedTime() {
    time_t now = time(nullptr);
    return now > 1700000000;  // sanity threshold (~2023)
}

static void ensureNtpSync() {
    if (WiFi.status() != WL_CONNECTED) {
        clockTimeValid = false;
        return;
    }

    if (clockConfigDirty) {
        ntpConfigured = false;
        clockConfigDirty = false;
    }

    unsigned long nowMs = millis();
    if (!ntpConfigured && (ntpLastAttempt == 0 || (nowMs - ntpLastAttempt) >= NTP_RETRY_MS)) {
        configTzTime(clockTz, "pool.ntp.org", "time.nist.gov", "time.google.com");
        ntpConfigured = true;
        ntpLastAttempt = nowMs;
        Serial.printf("[CLOCK] NTP config requested (TZ=%s)\n", clockTz);
    }

    clockTimeValid = haveSyncedTime();
    if (!clockTimeValid && (nowMs - ntpLastAttempt) >= NTP_RETRY_MS) {
        ntpConfigured = false;  // trigger another retry
    }
}

static bool showClockMode(uint32_t minSecondsToShow, int waitBuf = -1) {
    if (!clockModeEnabled) return false;
    if (WiFi.status() != WL_CONNECTED) return false;
    if (!clockTimeValid) return false;
    if (!dma_display) return false;

    unsigned long minEndAt = millis() + (minSecondsToShow * 1000UL);
    for (;;) {
        if (textNotif.pending) return false;
        if (WiFi.status() != WL_CONNECTED) return false;

        time_t now = time(nullptr);
        if (now <= 1700000000) return false;

        struct tm ti;
        localtime_r(&now, &ti);

        char hhmmss[9];
        char dateBuf[11];
        char sep = (ti.tm_sec % 2 == 0) ? ':' : ' ';
        snprintf(hhmmss, sizeof(hhmmss), "%02d%c%02d%c%02d",
                 ti.tm_hour, sep, ti.tm_min, sep, ti.tm_sec);
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d",
                 ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);

        int w1 = textWidthPx(hhmmss, 2);
        int w2 = textWidthPx(dateBuf, 1);
        int x1 = (TOTAL_WIDTH - w1) / 2;
        int x2 = (TOTAL_WIDTH - w2) / 2;
        int y1 = 2;
        int y2 = 22;

        dma_display->fillScreen(0);
        dma_display->setTextWrap(false);
        dma_display->setTextSize(2);
        dma_display->setTextColor(dma_display->color565(255, 255, 255));
        dma_display->setCursor(x1, y1);
        dma_display->print(hhmmss);

        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(180, 180, 180));
        dma_display->setCursor(x2, y2);
        dma_display->print(dateBuf);

        bool minElapsed = ((long)(millis() - minEndAt) >= 0);
        bool waitReady = (waitBuf < 0 || waitBuf > 1) ? true : gifBufOk[waitBuf];
        if (minElapsed && waitReady) break;

        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

static bool shouldShowClockAfterGif() {
    gifsSinceClock++;
    if (!clockModeEnabled) return false;
    if (clockEveryNGifs == 0) return false;
    if (gifsSinceClock < clockEveryNGifs) return false;

    ensureNtpSync();
    return clockTimeValid && WiFi.status() == WL_CONNECTED && dma_display;
}

/* =================================================================
 *  GIF DRAW CALLBACK — called per scanline by AnimatedGIF decoder
 * ================================================================= */
void GIFDraw(GIFDRAW *pDraw) {
    if (!dma_display) return;

    if (clearBeforeNextGifDraw) {
        dma_display->clearScreen();
        clearBeforeNextGifDraw = false;
    }

    uint16_t *pal = pDraw->pPalette;
    uint8_t  *px  = pDraw->pPixels;
    int w = pDraw->iWidth;
    if (w + pDraw->iX > TOTAL_WIDTH) w = TOTAL_WIDTH - pDraw->iX;

    int bx = pDraw->iX + xOff;
    int y  = pDraw->iY + pDraw->y + yOff;

    if (y < 0 || y >= PANEL_RES_Y) return;

    if (pDraw->ucHasTransparency) {
        uint8_t tp = pDraw->ucTransparent;
        for (int x = 0; x < w; x++) {
            if (px[x] != tp)
                dma_display->drawPixel(bx + x, y, pal[px[x]]);
        }
    } else {
        for (int x = 0; x < w; x++)
            dma_display->drawPixel(bx + x, y, pal[px[x]]);
    }
}

/* =================================================================
 *  SD FILE CALLBACKS — used only in file-based fallback mode
 * ================================================================= */
static void *fileOpen(const char *fname, int32_t *pSize) {
    sdGifFile = SD.open(fname);
    if (sdGifFile) { *pSize = sdGifFile.size(); return &sdGifFile; }
    return nullptr;
}

static void fileClose(void *h) {
    if (h) static_cast<File *>(h)->close();
}

static int32_t fileRead(GIFFILE *pf, uint8_t *buf, int32_t len) {
    File *f = static_cast<File *>(pf->fHandle);
    if ((pf->iSize - pf->iPos) < len)
        len = pf->iSize - pf->iPos - 1;
    if (len <= 0) return 0;
    len = (int32_t)f->read(buf, len);
    pf->iPos = f->position();
    return len;
}

static int32_t fileSeek(GIFFILE *pf, int32_t pos) {
    File *f = static_cast<File *>(pf->fHandle);
    f->seek(pos);
    pf->iPos = (int32_t)f->position();
    return pf->iPos;
}

/* =================================================================
 *  READ A GIF PATH BY INDEX — reads from SD using stored offset
 * ================================================================= */
bool getGifPath(int idx, char *buf, size_t bufLen) {
    if (idx < 0 || idx >= gifCount) return false;

    File f = SD.open(LISTA_PATH, FILE_READ);
    if (!f) return false;

    f.seek(gifOffsets[idx]);
    String line = f.readStringUntil('\n');
    f.close();
    line.trim();
    if (line.length() == 0) return false;

    if (line[0] != '/') {
        buf[0] = '/';
        strncpy(buf + 1, line.c_str(), bufLen - 2);
        buf[bufLen - 1] = '\0';
    } else {
        strncpy(buf, line.c_str(), bufLen - 1);
        buf[bufLen - 1] = '\0';
    }
    return true;
}

/* =================================================================
 *  LOAD PLAYLIST FROM /lista.txt — stores only byte offsets
 *  Uses a stack buffer to avoid heap-fragmenting String allocations.
 * ================================================================= */
bool loadGifList() {
    Serial.println("[..] Opening " LISTA_PATH);
    File f = SD.open(LISTA_PATH, FILE_READ);
    if (!f) {
        Serial.println("[ERR] Cannot open " LISTA_PATH);
        return false;
    }

    size_t fsize = f.size();
    Serial.printf("[..] File opened, size=%u\n", fsize);

    // --- Pass 1: count valid lines ---
    int count = 0;
    char buf[260];
    while (f.available()) {
        int len = 0;
        while (f.available() && len < (int)sizeof(buf) - 1) {
            char c = (char)f.read();
            if (c == '\n') break;
            if (c != '\r') buf[len++] = c;
        }
        buf[len] = '\0';
        int s = 0;
        while (s < len && buf[s] == ' ') s++;
        int e = len;
        while (e > s && buf[e - 1] == ' ') e--;
        if (e > s && buf[s] != '#') count++;
    }
    Serial.printf("[..] Counted %d valid lines\n", count);
    if (count == 0) { f.close(); return false; }

    // --- Allocate offset array (PSRAM preferred, single allocation) ---
    if (gifOffsets) { free(gifOffsets); gifOffsets = nullptr; }
    size_t allocSz = (size_t)count * sizeof(uint32_t);
    gifOffsets = (uint32_t *)(psramFound() ? ps_malloc(allocSz) : malloc(allocSz));
    if (!gifOffsets) {
        Serial.printf("[ERR] Failed to alloc %u B for offset table\n", allocSz);
        f.close();
        return false;
    }
    Serial.printf("[..] Allocated %u B for %d offsets (%s)\n",
                  allocSz, count, psramFound() ? "PSRAM" : "heap");

    // --- Pass 2: store byte offsets ---
    f.seek(0);
    int idx = 0;
    while (f.available() && idx < count) {
        uint32_t pos = f.position();
        int len = 0;
        while (f.available() && len < (int)sizeof(buf) - 1) {
            char c = (char)f.read();
            if (c == '\n') break;
            if (c != '\r') buf[len++] = c;
        }
        buf[len] = '\0';
        int s = 0;
        while (s < len && buf[s] == ' ') s++;
        int e = len;
        while (e > s && buf[e - 1] == ' ') e--;
        if (e > s && buf[s] != '#') gifOffsets[idx++] = pos;
    }
    f.close();

    gifCount = idx;
    Serial.printf("[OK] Indexed %d GIF path(s) from lista.txt\n", gifCount);
    return gifCount > 0;
}

/* =================================================================
 *  LOAD A GIF FILE INTO A MEMORY BUFFER
 * ================================================================= */
bool loadIntoBuffer(int bi, int fi) {
    if (!gifBuf[bi] || fi < 0 || fi >= gifCount) return false;

    char path[256];
    if (!getGifPath(fi, path, sizeof(path))) return false;

    File f = SD.open(path);
    if (!f) {
        Serial.printf("[ERR] open failed: %s\n", path);
        return false;
    }

    size_t sz = f.size();
    if (sz > gifBufCap[bi]) {
        Serial.printf("[ERR] Too large (%u B, cap %u): %s\n", sz, gifBufCap[bi], path);
        f.close();
        return false;
    }

    size_t rd = f.read(gifBuf[bi], sz);
    f.close();

    gifBufLen[bi] = rd;
    gifBufOk[bi]  = (rd == sz);
    if (gifBufOk[bi])
        Serial.printf("[BUF%d] Loaded %s (%u B)\n", bi, path, sz);
    return gifBufOk[bi];
}

/* =================================================================
 *  PRELOAD TASK — runs on Core 0, loads next GIF while current plays
 * ================================================================= */
void preloadTaskFn(void *) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait for signal

        int bi = plBufIdx;
        int fi = plFileIdx;
        if (bi < 0 || fi < 0) continue;

        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000))) {
            loadIntoBuffer(bi, fi);
            xSemaphoreGive(sdMutex);
        }
    }
}

/* =================================================================
 *  PLAY GIF FROM MEMORY BUFFER (no SD access)
 * ================================================================= */
void playFromBuffer(int bi) {
    if (!gifBufOk[bi]) return;
    if (!panelOn) { vTaskDelay(pdMS_TO_TICKS(100)); return; }

    if (gif.open(gifBuf[bi], (int)gifBufLen[bi], GIFDraw)) {
        xOff = (TOTAL_WIDTH - gif.getCanvasWidth())  / 2;
        yOff = (PANEL_RES_Y - gif.getCanvasHeight()) / 2;
        clearBeforeNextGifDraw = true;

        int d;
        while (gif.playFrame(false, &d) && !textNotif.pending) {
            unsigned long t = millis();
            while ((millis() - t) < (unsigned long)d) { yield(); }
        }
        gif.close();
        clearBeforeNextGifDraw = false;
    } else {
        Serial.printf("[ERR] Decode failed (buf %d)\n", bi);
        clearBeforeNextGifDraw = false;
    }
}

/* =================================================================
 *  PLAY GIF FROM SD FILE (fallback when no PSRAM buffers)
 * ================================================================= */
void playFromFile(int fi) {
    if (fi < 0 || fi >= gifCount) return;
    if (!panelOn) { vTaskDelay(pdMS_TO_TICKS(100)); return; }
    char path[256];
    if (!getGifPath(fi, path, sizeof(path))) return;

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000))) {
        if (gif.open(path, fileOpen, fileClose, fileRead, fileSeek, GIFDraw)) {
            xOff = (TOTAL_WIDTH - gif.getCanvasWidth())  / 2;
            yOff = (PANEL_RES_Y - gif.getCanvasHeight()) / 2;
            clearBeforeNextGifDraw = true;

            int d;
            while (gif.playFrame(false, &d) && !textNotif.pending) {
                unsigned long t = millis();
                while ((millis() - t) < (unsigned long)d) { yield(); }
            }
            gif.close();
            clearBeforeNextGifDraw = false;
        } else {
            Serial.printf("[ERR] Cannot open: %s\n", path);
            clearBeforeNextGifDraw = false;
        }
        xSemaphoreGive(sdMutex);
    }
}

/* =================================================================
 *  NETWORK TASK — Core 0
 *  Serves captive portal + DNS + MQTT independently from GIF rendering.
 * ================================================================= */
void networkTaskFn(void *) {
    wifiSetup();
    mqttSetup();
    for (;;) {
        serviceWeb();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* =================================================================
 *  PLAYBACK TASK — Core 1
 *  Main GIF/notification scheduler, isolated from network task.
 * ================================================================= */
void playbackTaskFn(void *) {
    for (;;) {
        ensureNtpSync();

        // Text notification takes priority over GIF playback
        if (textNotif.pending) {
            textNotif.pending = false;
            showNotification(textNotif.text, textNotif.color, textNotif.size,
                             textNotif.rainbow, textNotif.duration);
            if (dma_display) dma_display->clearScreen();
            continue;
        }

        if (gifCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Double-buffer mode (PSRAM)
        if (hasDualBuf && gifBufOk[playBuf]) {
            playFromBuffer(playBuf);
            bool showClock = shouldShowClockAfterGif();

            // Single-GIF shortcut: just replay from same buffer
            if (gifCount == 1) {
                if (showClock && showClockMode(CLOCK_MIN_DISPLAY_SECONDS, playBuf)) {
                    gifsSinceClock = 0;
                }
                continue;
            }

            // Advance playlist
            currentIdx = nextIdx();

            // Swap buffers
            int doneBuf = playBuf;
            int nextBuf = 1 - playBuf;

            if (showClock && showClockMode(CLOCK_MIN_DISPLAY_SECONDS, nextBuf)) {
                gifsSinceClock = 0;
            }

            playBuf = nextBuf;

            // If the next buffer isn't ready yet, block-load it
            if (!gifBufOk[playBuf]) {
                if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000))) {
                    loadIntoBuffer(playBuf, currentIdx);
                    xSemaphoreGive(sdMutex);
                }
            }

            // Request background preload of the GIF *after* next
            gifBufOk[doneBuf] = false;
            plBufIdx  = doneBuf;
            plFileIdx = nextIdx();
            if (preloadHandle) xTaskNotifyGive(preloadHandle);

            continue;
        }

        // File-based fallback (no PSRAM)
        playFromFile(currentIdx);
        if (shouldShowClockAfterGif() && showClockMode(CLOCK_MIN_DISPLAY_SECONDS)) {
            gifsSinceClock = 0;
        }
        currentIdx = nextIdx();
    }
}

/* =================================================================
 *  SETUP
 * ================================================================= */
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n================================");
    Serial.println("  DeLorean DMD  128x32");
    Serial.println("================================");

    /* ── 1. HUB75 panel init (first, so we can show errors) ── */
    HUB75_I2S_CFG::i2s_pins pins = {
        R1_PIN, G1_PIN, B1_PIN,
        R2_PIN, G2_PIN, B2_PIN,
        A_PIN,  B_PIN,  C_PIN, D_PIN, E_PIN,
        LAT_PIN, OE_PIN, CLK_PIN
    };

    HUB75_I2S_CFG cfg(PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN, pins);

    cfg.clkphase         = CLK_PHASE;
    cfg.latch_blanking   = LATCH_BLANKING;
    cfg.i2sspeed         = I2S_CLK_SPEED;
    cfg.min_refresh_rate = MIN_REFRESH_HZ;
    cfg.double_buff      = false;

    Serial.printf("[..] Creating display %dx%d (chain=%d)...\n",
                  PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
    Serial.printf("[..] Free heap before display: %u\n", ESP.getFreeHeap());

    dma_display = new MatrixPanel_I2S_DMA(cfg);
    if (!dma_display) {
        Serial.println("[FATAL] Display allocation failed");
        while (true) delay(1000);
    }

    if (!dma_display->begin()) {
        Serial.println("[FATAL] Display init failed");
        while (true) delay(1000);
    }
    dma_display->setBrightness8(min((uint8_t)DEFAULT_BRIGHTNESS, (uint8_t)MAX_BRIGHTNESS));
    dma_display->clearScreen();
    Serial.printf("[OK] Display %dx%d initialised\n", TOTAL_WIDTH, PANEL_RES_Y);

    /* ── 2. SD Card ────────────────────────────────────────── */
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        Serial.println("[FATAL] SD mount failed");
        showMessage("SD FAIL", dma_display->color565(255, 0, 0));
        while (true) {
            dma_display->setBrightness8(min((uint8_t)DEFAULT_BRIGHTNESS, (uint8_t)MAX_BRIGHTNESS));
            delay(500);
            dma_display->setBrightness8(0);
            delay(500);
        }
    }
    Serial.println("[OK] SD mounted");

    /* ── 3. Mutex ──────────────────────────────────────────── */
    sdMutex = xSemaphoreCreateMutex();
    Serial.println("[OK] Mutex created");

    /* ── 4. Load playlist ──────────────────────────────────── */
    Serial.println("[..] Loading playlist...");
    if (!loadGifList()) {
        Serial.println("[FATAL] No GIFs in " LISTA_PATH);
        showMessage("NO GIFs", dma_display->color565(255, 80, 0));
        while (true) {
            dma_display->setBrightness8(min((uint8_t)DEFAULT_BRIGHTNESS, (uint8_t)MAX_BRIGHTNESS));
            delay(500);
            dma_display->setBrightness8(0);
            delay(500);
        }
    }

    /* ── 5. AnimatedGIF decoder ────────────────────────────── */
    gif.begin(LITTLE_ENDIAN_PIXELS);
    Serial.println("[OK] AnimatedGIF ready");

    /* ── 6. WiFi / Captive Portal ──────────────────────────── */
    // wifiSetup() and mqttSetup() run on Core 0 inside networkTaskFn

    /* ── 7. Allocate PSRAM ping-pong buffers ───────────────── */
    Serial.printf("[..] PSRAM found: %s\n", psramFound() ? "YES" : "NO");
    if (psramFound()) {
        gifBuf[0] = (uint8_t *)ps_malloc(MAX_GIF_SIZE);
        gifBuf[1] = (uint8_t *)ps_malloc(MAX_GIF_SIZE);
        hasDualBuf = (gifBuf[0] != nullptr && gifBuf[1] != nullptr);
        if (hasDualBuf) {
            gifBufCap[0] = gifBufCap[1] = MAX_GIF_SIZE;
            Serial.printf("[OK] PSRAM double-buffer: 2 x %u KB\n",
                          MAX_GIF_SIZE / 1024);
        }
    }

    if (!hasDualBuf) {
        // Without PSRAM, heap is too limited for dual buffers + DMA display +
        // AnimatedGIF decoder (~22.5 KB).  Use file-based fallback instead.
        Serial.println("[WARN] No PSRAM — file-based fallback");
    }

    /* ── 8. Initial GIF load & preload task ───────────────── */
    if (hasDualBuf) {
        loadIntoBuffer(0, 0);
        playBuf = 0;

        if (gifCount > 1)
            loadIntoBuffer(1, 1);

        xTaskCreatePinnedToCore(
            preloadTaskFn, "Preload", 4096, nullptr, 1, &preloadHandle, 0);
        Serial.println("[OK] Preload task started on Core 0");
    }

    // Network stack (WiFi + MQTT + portal) on Core 0
    xTaskCreatePinnedToCore(
        networkTaskFn, "Net", 6144, nullptr, 2, &netTaskHandle, 0);
    Serial.println("[OK] Network task started on Core 0");

    // GIF playback scheduler on Core 1
    xTaskCreatePinnedToCore(
        playbackTaskFn, "Playback", 6144, nullptr, 1, &playbackHandle, 1);
    Serial.println("[OK] Playback task started on Core 1");

    Serial.println("[OK] Setup complete — starting playback\n");
}

/* =================================================================
 *  MAIN LOOP — idle (work runs in FreeRTOS tasks)
 * ================================================================= */
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
