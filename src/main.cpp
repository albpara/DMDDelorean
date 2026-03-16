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

// Preload task coordination
TaskHandle_t      preloadHandle = nullptr;
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
 *  UTILITY — Show a single-line message on the panel (centered + scroll)
 *  Short text: centred on screen.
 *  Long  text: starts centred, scrolls left until the last character
 *              disappears off the left edge, then returns.
 *  Blocking call — returns only when the full scroll has finished.
 * ================================================================= */
#define CHAR_W  6   // Adafruit GFX default 5x7 font + 1px gap = 6px per char
#define CHAR_H  8   // font height
#define SCROLL_STEP_MS  30   // ms per pixel scroll step

void showMessage(const char *msg, uint16_t color = 0xFFFF) {
    if (!dma_display) return;

    int len    = (int)strlen(msg);
    int textW  = len * CHAR_W;                 // total pixel width of message
    int cy     = (PANEL_RES_Y - CHAR_H) / 2;  // vertical centre

    dma_display->setTextSize(1);
    dma_display->setTextWrap(false);
    dma_display->setTextColor(color);

    if (textW <= TOTAL_WIDTH) {
        // Fits on screen — just centre it
        int cx = (TOTAL_WIDTH - textW) / 2;
        dma_display->fillScreen(0);
        dma_display->setCursor(cx, cy);
        dma_display->print(msg);
        return;
    }

    // Text wider than panel — scroll from centred position to fully off-left
    int startX = (TOTAL_WIDTH - textW) / 2;   // centred (will be negative for very long text, clamp to 0)
    if (startX < 0) startX = 0;               // begin just at left edge
    int endX   = -textW;                       // last char just disappeared off the left

    for (int x = startX; x >= endX; x--) {
        dma_display->fillScreen(0);
        dma_display->setCursor(x, cy);
        dma_display->print(msg);
        delay(SCROLL_STEP_MS);
    }
}

/* =================================================================
 *  TEXT NOTIFICATION RENDERER
 *  Renders textNotif to the panel until its duration expires or
 *  textNotif.active is cleared externally (e.g. by a new notification).
 *  Calls serviceWeb() in all inner loops so WiFi/MQTT remain responsive.
 * ================================================================= */
void handleTextNotification() {
    if (!dma_display || !textNotif.active) return;

    int sz    = (textNotif.size >= 1 && textNotif.size <= 3) ? textNotif.size : 1;
    int charW = 6 * sz;   // Adafruit GFX cursor advance per char
    int charH = 8 * sz;
    int len   = (int)strlen(textNotif.text);
    int textW = len * charW;
    int cy    = (PANEL_RES_Y - charH) / 2;
    if (cy < 0) cy = 0;

    dma_display->setTextSize(sz);
    dma_display->setTextWrap(false);
    dma_display->setTextColor(textNotif.color);

    uint32_t dur = (textNotif.durationMs > 0)
                 ? textNotif.durationMs
                 : (uint32_t)DEFAULT_NOTIFY_DURATION_MS;
    unsigned long startTime = millis();

    switch (textNotif.effect) {

        case EFFECT_STATIC: {
            int cx = (TOTAL_WIDTH - textW) / 2;
            if (cx < 0) cx = 0;
            dma_display->fillScreen(0);
            dma_display->setCursor(cx, cy);
            dma_display->print(textNotif.text);
            while (textNotif.active && (millis() - startTime) < dur) {
                serviceWeb();
                delay(50);
            }
            textNotif.active = false;
            break;
        }

        case EFFECT_SCROLL: {
            // Text enters from right edge, exits off left edge.
            // Loops until the duration expires.
            int startX = TOTAL_WIDTH;
            int endX   = -textW;
            bool done  = false;
            while (!done && textNotif.active) {
                for (int x = startX; x >= endX; x--) {
                    dma_display->fillScreen(0);
                    dma_display->setCursor(x, cy);
                    dma_display->print(textNotif.text);
                    serviceWeb();
                    delay(SCROLL_STEP_MS);
                    if (!textNotif.active) return;
                    if ((millis() - startTime) >= dur) { done = true; break; }
                }
            }
            textNotif.active = false;
            break;
        }

        case EFFECT_BLINK: {
            int cx = (TOTAL_WIDTH - textW) / 2;
            if (cx < 0) cx = 0;
            bool visible = true;
            unsigned long lastToggle = millis();
            dma_display->fillScreen(0);
            dma_display->setCursor(cx, cy);
            dma_display->print(textNotif.text);
            while (textNotif.active && (millis() - startTime) < dur) {
                if (millis() - lastToggle >= 500) {
                    visible = !visible;
                    lastToggle = millis();
                    dma_display->fillScreen(0);
                    if (visible) {
                        dma_display->setCursor(cx, cy);
                        dma_display->print(textNotif.text);
                    }
                }
                serviceWeb();
                delay(50);
            }
            textNotif.active = false;
            break;
        }

        default:
            textNotif.active = false;
            break;
    }

    // Restore default text settings for subsequent GIF status messages
    dma_display->setTextSize(1);
}

/* =================================================================
 *  GIF DRAW CALLBACK — called per scanline by AnimatedGIF decoder
 * ================================================================= */
void GIFDraw(GIFDRAW *pDraw) {
    if (!dma_display) return;

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
    if (!panelOn) { delay(100); serviceWeb(); return; }

    if (gif.open(gifBuf[bi], (int)gifBufLen[bi], GIFDraw)) {
        xOff = (TOTAL_WIDTH - gif.getCanvasWidth())  / 2;
        yOff = (PANEL_RES_Y - gif.getCanvasHeight()) / 2;
        dma_display->clearScreen();

        int d;
        while (gif.playFrame(false, &d)) {
            unsigned long t = millis();
            while ((millis() - t) < (unsigned long)d) {
                serviceWeb();
                if (textNotif.active) break;
                yield();
            }
            if (textNotif.active) break;
        }
        gif.close();
    } else {
        Serial.printf("[ERR] Decode failed (buf %d)\n", bi);
    }
}

/* =================================================================
 *  PLAY GIF FROM SD FILE (fallback when no PSRAM buffers)
 * ================================================================= */
void playFromFile(int fi) {
    if (fi < 0 || fi >= gifCount) return;
    if (!panelOn) { delay(100); serviceWeb(); return; }
    char path[256];
    if (!getGifPath(fi, path, sizeof(path))) return;

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(5000))) {
        if (gif.open(path, fileOpen, fileClose, fileRead, fileSeek, GIFDraw)) {
            xOff = (TOTAL_WIDTH - gif.getCanvasWidth())  / 2;
            yOff = (PANEL_RES_Y - gif.getCanvasHeight()) / 2;
            dma_display->clearScreen();

            int d;
            while (gif.playFrame(false, &d)) {
                unsigned long t = millis();
                while ((millis() - t) < (unsigned long)d) {
                    serviceWeb();
                    if (textNotif.active) break;
                    yield();
                }
                if (textNotif.active) break;
            }
            gif.close();
        } else {
            Serial.printf("[ERR] Cannot open: %s\n", path);
        }
        xSemaphoreGive(sdMutex);
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
    Serial.printf("[..] Free heap before WiFi: %u\n", ESP.getFreeHeap());
    wifiSetup();
    mqttSetup();
    Serial.printf("[..] Free heap after WiFi: %u\n", ESP.getFreeHeap());

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

    /* ── 8. Initial load & preload task ────────────────────── */
    showMessage("Loading...", dma_display->color565(0, 200, 200));

    if (hasDualBuf) {
        loadIntoBuffer(0, 0);
        playBuf = 0;

        if (gifCount > 1)
            loadIntoBuffer(1, 1);

        xTaskCreatePinnedToCore(
            preloadTaskFn, "Preload", 4096, nullptr, 1, &preloadHandle, 0);
        Serial.println("[OK] Preload task started on Core 0");
    }

    Serial.println("[OK] Setup complete — starting playback\n");
}

/* =================================================================
 *  MAIN LOOP — plays GIFs sequentially, with seamless preloading
 * ================================================================= */
void loop() {
    // Text notifications take priority over GIF playback
    if (textNotif.active) {
        handleTextNotification();
        return;
    }

    if (gifCount == 0) {
        serviceWeb();
        delay(100);
        return;
    }

    /* ─── Double-buffer mode (PSRAM) ─────────────────────── */
    if (hasDualBuf && gifBufOk[playBuf]) {
        playFromBuffer(playBuf);

        // Single-GIF shortcut: just replay from same buffer
        if (gifCount == 1) return;

        // Advance playlist
        currentIdx = nextIdx();

        // Swap buffers
        int doneBuf = playBuf;
        playBuf = 1 - playBuf;

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
        xTaskNotifyGive(preloadHandle);

        return;
    }

    /* ─── File-based fallback (no PSRAM) ─────────────────── */
    playFromFile(currentIdx);
    currentIdx = nextIdx();
}
