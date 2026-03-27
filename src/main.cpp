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
#include "components/app_config.h"
#include "components/mqtt.h"
#include "components/wifi_portal.h"

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

struct PlaylistIndexHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    uint32_t listaSize;
    uint32_t count;
};

/* =================================================================
 *  PLAYLIST NAVIGATION — sequential or random (hardware RNG)
 * ================================================================= */
int nextIdx() {
    if (RANDOM_PLAYBACK && gifCount > 1)
        return (int)(esp_random() % (uint32_t)gifCount);
    return (currentIdx + 1) % gifCount;
}

static bool loadGifIndex(size_t listaSize) {
    File idx = SD.open(LISTA_INDEX_PATH, FILE_READ);
    if (!idx) return false;

    PlaylistIndexHeader hdr;
    if (idx.read((uint8_t *)&hdr, sizeof(hdr)) != (int)sizeof(hdr)) {
        idx.close();
        LOGGER.println("[IDX] Header read failed");
        return false;
    }

    if (hdr.magic != LISTA_INDEX_MAGIC || hdr.version != LISTA_INDEX_VERSION) {
        idx.close();
        LOGGER.println("[IDX] Header mismatch");
        return false;
    }

    if (hdr.listaSize != (uint32_t)listaSize || hdr.count == 0) {
        idx.close();
        LOGGER.println("[IDX] Cache metadata mismatch");
        return false;
    }

    size_t expectedSize = sizeof(PlaylistIndexHeader) + ((size_t)hdr.count * sizeof(uint32_t));
    if ((size_t)idx.size() != expectedSize) {
        idx.close();
        LOGGER.println("[IDX] Cache size mismatch");
        return false;
    }

    if (gifOffsets) {
        free(gifOffsets);
        gifOffsets = nullptr;
    }

    size_t allocSz = (size_t)hdr.count * sizeof(uint32_t);
    gifOffsets = (uint32_t *)(psramFound() ? ps_malloc(allocSz) : malloc(allocSz));
    if (!gifOffsets) {
        idx.close();
        LOGGER.printf("[IDX] Failed to alloc %u B for cache\n", allocSz);
        return false;
    }

    if (idx.read((uint8_t *)gifOffsets, allocSz) != (int)allocSz) {
        idx.close();
        free(gifOffsets);
        gifOffsets = nullptr;
        LOGGER.println("[IDX] Offset read failed");
        return false;
    }

    idx.close();
    gifCount = (int)hdr.count;
    LOGGER.printf("[IDX] Loaded %d offsets from %s\n", gifCount, LISTA_INDEX_PATH);
    return true;
}

static bool saveGifIndex(size_t listaSize, int count) {
    if (!gifOffsets || count <= 0) return false;

    if (SD.exists(LISTA_INDEX_PATH)) {
        SD.remove(LISTA_INDEX_PATH);
    }

    File idx = SD.open(LISTA_INDEX_PATH, FILE_WRITE);
    if (!idx) {
        LOGGER.println("[IDX] Failed to open cache for write");
        return false;
    }

    PlaylistIndexHeader hdr;
    hdr.magic = LISTA_INDEX_MAGIC;
    hdr.version = LISTA_INDEX_VERSION;
    hdr.reserved = 0;
    hdr.listaSize = (uint32_t)listaSize;
    hdr.count = (uint32_t)count;

    size_t dataSz = (size_t)count * sizeof(uint32_t);
    bool ok = (idx.write((const uint8_t *)&hdr, sizeof(hdr)) == (int)sizeof(hdr));
    ok = ok && (idx.write((const uint8_t *)gifOffsets, dataSz) == (int)dataSz);
    idx.close();

    if (!ok) {
        LOGGER.println("[IDX] Failed to write full cache");
        SD.remove(LISTA_INDEX_PATH);
        return false;
    }

    LOGGER.printf("[IDX] Saved %d offsets to %s\n", count, LISTA_INDEX_PATH);
    return true;
}

/* =================================================================
 *  UTILITY — Show a single-line message on the panel
 *  Short text (fits on screen): centred.
 *  Long  text: starts at left edge, scrolls left until fully off, then returns.
 *  Optional font size (1–3) and rainbow colour effect.
 *  Blocking call — returns only when the full scroll has finished.
 * ================================================================= */
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

/* =================================================================
 *  VERTICAL SCROLL HELPERS
 * ================================================================= */

// Split text into word-wrapped lines for vertical scrolling.
// Handles both actual newline characters (0x0A) and the two-char
// sequence '\' + 'n' (as produced by JSON string literals).
// Returns the number of lines produced.
static int buildWrappedLines(const char *text, uint8_t size,
                              char lines[][TOTAL_WIDTH + 1], int maxLines) {
    if (!text || !*text) return 0;
    int maxChars = TOTAL_WIDTH / (CHAR_W * size);
    if (maxChars < 1) maxChars = 1;

    int lineCount = 0;
    char lineBuf[TOTAL_WIDTH + 1];
    int lineBufLen = 0;

    const char *p = text;
    while (*p && lineCount < maxLines) {
        // Detect line break: actual newline OR JSON-escaped two-char sequence
        bool isNewline = (*p == '\n') || (*p == '\\' && *(p + 1) == 'n');
        if (isNewline) {
            lineBuf[lineBufLen] = '\0';
            strncpy(lines[lineCount], lineBuf, TOTAL_WIDTH);
            lines[lineCount][TOTAL_WIDTH] = '\0';
            lineCount++;
            lineBufLen = 0;
            p += (*p == '\\') ? 2 : 1;
            continue;
        }

        lineBuf[lineBufLen++] = *p++;

        if (lineBufLen >= maxChars) {
            // Try to wrap at the last space in the second half of the buffer
            int splitAt = lineBufLen;
            for (int i = lineBufLen - 1; i > lineBufLen / 2; i--) {
                if (lineBuf[i] == ' ') { splitAt = i; break; }
            }

            if (splitAt < lineBufLen) {
                // Wrap at space
                lineBuf[splitAt] = '\0';
                strncpy(lines[lineCount], lineBuf, TOTAL_WIDTH);
                lines[lineCount][TOTAL_WIDTH] = '\0';
                lineCount++;
                int remainLen = lineBufLen - splitAt - 1;
                memmove(lineBuf, lineBuf + splitAt + 1, remainLen);
                lineBufLen = remainLen;
            } else {
                // No space found — hard-cut
                lineBuf[lineBufLen] = '\0';
                strncpy(lines[lineCount], lineBuf, TOTAL_WIDTH);
                lines[lineCount][TOTAL_WIDTH] = '\0';
                lineCount++;
                lineBufLen = 0;
            }
        }
    }

    // Flush any remaining text as the final line
    if (lineBufLen > 0 && lineCount < maxLines) {
        lineBuf[lineBufLen] = '\0';
        strncpy(lines[lineCount], lineBuf, TOTAL_WIDTH);
        lines[lineCount][TOTAL_WIDTH] = '\0';
        lineCount++;
    }

    return lineCount;
}

// Scroll all lines upward (bottom → top). duration = number of full scroll loops.
static void showVerticalScroll(const char *msg, uint16_t color, uint8_t size,
                                bool rainbow, uint32_t duration) {
    if (!dma_display || !msg || !*msg) return;
    if (size < 1) size = 1;
    if (size > 3) size = 3;

    static char lines[VSCROLL_MAX_LINES][TOTAL_WIDTH + 1];
    int lineCount = buildWrappedLines(msg, size, lines, VSCROLL_MAX_LINES);
    if (lineCount == 0) return;

    int ch          = CHAR_H * size;
    int totalHeight = lineCount * ch;
    int hueShift    = 0;

    uint32_t loops = (duration == 0) ? 1 : duration;
    for (uint32_t l = 0; l < loops; l++) {
        for (int y = PANEL_RES_Y; y >= -totalHeight; y--) {
            dma_display->fillScreen(0);
            for (int i = 0; i < lineCount; i++) {
                int lineY = y + i * ch;
                if (lineY < -ch || lineY >= PANEL_RES_Y) continue;
                int lineLen = (int)strlen(lines[i]);
                int lineW   = lineLen * (CHAR_W * size);
                int cx      = (TOTAL_WIDTH - lineW) / 2;
                if (cx < 0) cx = 0;
                if (rainbow) {
                    drawRainbowText(lines[i], cx, lineY, size, hueShift);
                } else {
                    dma_display->setTextSize(size);
                    dma_display->setTextWrap(false);
                    dma_display->setTextColor(color);
                    dma_display->setCursor(cx, lineY);
                    dma_display->print(lines[i]);
                }
            }
            if (rainbow) hueShift = (hueShift + 2) % 360;
            delay(SCROLL_STEP_MS);
        }
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
                      bool rainbow, uint32_t duration, bool scrollVertical = false,
                      uint8_t speed = 0) {
    if (!dma_display || !msg || msg[0] == '\0') return;
    if (size < 1) size = 1;
    if (size > 3) size = 3;

    if (scrollVertical) {
        showVerticalScroll(msg, color, size, rainbow, duration);
        return;
    }

    // Sanitize for horizontal: replace real \n and JSON-escaped \\n with spaces
    char sanitizedMsg[256];
    {
        const char *s = msg;
        char *d = sanitizedMsg;
        char *end = sanitizedMsg + sizeof(sanitizedMsg) - 1;
        while (*s && d < end) {
            if (*s == '\n') {
                *d++ = ' '; s++;
            } else if (*s == '\\' && *(s + 1) == 'n') {
                *d++ = ' '; s += 2;
            } else {
                *d++ = *s++;
            }
        }
        *d = '\0';
    }

    int stepMs = (speed == 1) ? SCROLL_STEP_MS * 2 : SCROLL_STEP_MS;
    int cw    = CHAR_W * size;
    int ch    = CHAR_H * size;
    int len   = (int)strlen(sanitizedMsg);
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
                    drawRainbowText(sanitizedMsg, x, cy, size, hueShift);
                    hueShift = (hueShift + 8) % 360;
                } else {
                    dma_display->setTextSize(size);
                    dma_display->setTextWrap(false);
                    dma_display->setTextColor(color);
                    dma_display->setCursor(x, cy);
                    dma_display->print(sanitizedMsg);
                }
                delay(stepMs);
            }
        }
        return;
    }

    // Non-scrolling text: duration is interpreted as seconds; left-aligned.
    // Use base refresh rate regardless of speed (speed only applies to scrolling).
    uint32_t seconds = (duration == 0) ? 1 : duration;
    uint32_t durationMs = seconds * 1000UL;
    unsigned long start = millis();
    int cx = 0;

    while (millis() - start < durationMs) {
        dma_display->fillScreen(0);
        if (rainbow) {
            drawRainbowText(sanitizedMsg, cx, cy, size, hueShift);
            hueShift = (hueShift + 8) % 360;
        } else {
            dma_display->setTextSize(size);
            dma_display->setTextWrap(false);
            dma_display->setTextColor(color);
            dma_display->setCursor(cx, cy);
            dma_display->print(sanitizedMsg);
        }
        delay(SCROLL_STEP_MS);
    }
}

/* =================================================================
 *  SOLAR ENERGY CARD
 * ================================================================= */
// Renders a 128x32 energy overview:
//   Top 16 rows  — horizontal bar (surplus: purple=house, green=excess;
//                                  deficit: green=solar, red=grid draw)
//   Bottom 16 rows — house_w left (purple), net middle (green/red), solar_w right (orange)
static void showSolarCard(int solar_w, int house_w, uint32_t duration) {
    if (!dma_display) return;
    if (solar_w < 0) solar_w = 0;
    if (house_w < 0) house_w = 0;

    const uint16_t cGreen     = dma_display->color565(0,   200, 0);
    const uint16_t cPurple    = dma_display->color565(160, 0,   200);
    const uint16_t cRed       = dma_display->color565(220, 0,   0);
    const uint16_t cOrange    = dma_display->color565(255, 120, 0);
    const uint16_t cDark      = dma_display->color565(15,  15,  15);
    const uint16_t cTxtPurple = dma_display->color565(160, 0,   200);
    const uint16_t cTxtGreen  = dma_display->color565(0,   160, 0);

    // Bar occupies rows 2–13 (12 px tall, 2 px padding at top)
    const int BAR_Y = 2;
    const int BAR_H = 12;
    // Text row: vertically centred in bottom half (rows 16–31)
    const int TXT_Y = 20;

    uint32_t durationMs = ((duration == 0) ? 5 : duration) * 1000UL;
    unsigned long start = millis();

    while (millis() - start < durationMs) {
        dma_display->fillScreen(0);

        if (solar_w >= house_w) {
            // Surplus mode — scale 0..3000 W
            int housePx  = (house_w * TOTAL_WIDTH) / 3000;
            int solarPx  = (solar_w * TOTAL_WIDTH) / 3000;
            if (housePx > TOTAL_WIDTH) housePx = TOTAL_WIDTH;
            if (solarPx > TOTAL_WIDTH) solarPx = TOTAL_WIDTH;
            // Dark background for unused portion
            dma_display->fillRect(0, BAR_Y, TOTAL_WIDTH, BAR_H, cDark);
            // Purple: house consumption
            if (housePx > 0)
                dma_display->fillRect(0, BAR_Y, housePx, BAR_H, cPurple);
            // Green: solar excess (solar − house)
            if (solarPx - housePx > 0)
                dma_display->fillRect(housePx, BAR_Y, solarPx - housePx, BAR_H, cGreen);
        } else {
            // Deficit mode — scale 0..house_w, bar fills 100 %
            int scale   = (house_w > 0) ? house_w : 1;
            int solarPx = (solar_w * TOTAL_WIDTH) / scale;
            if (solarPx > TOTAL_WIDTH) solarPx = TOTAL_WIDTH;
            // Green: fraction covered by solar
            if (solarPx > 0)
                dma_display->fillRect(0, BAR_Y, solarPx, BAR_H, cGreen);
            // Red: grid draw (house − solar)
            if (TOTAL_WIDTH - solarPx > 0)
                dma_display->fillRect(solarPx, BAR_Y, TOTAL_WIDTH - solarPx, BAR_H, cRed);
        }

        // Bottom row: house value left, net value center, solar value right
        char buf[16];
        dma_display->setTextSize(1);
        dma_display->setTextWrap(false);

        snprintf(buf, sizeof(buf), "%dW", house_w);
        dma_display->setTextColor(cTxtPurple);
        dma_display->setCursor(2, TXT_Y);
        dma_display->print(buf);

        int net = solar_w - house_w;
        snprintf(buf, sizeof(buf), "%+dW", net);
        int twNet = (int)strlen(buf) * CHAR_W;
        dma_display->setTextColor((net >= 0) ? cTxtGreen : cRed);
        dma_display->setCursor((TOTAL_WIDTH - twNet) / 2, TXT_Y);
        dma_display->print(buf);

        snprintf(buf, sizeof(buf), "%dW", solar_w);
        int tw = (int)strlen(buf) * CHAR_W;
        dma_display->setTextColor(cOrange);
        dma_display->setCursor(TOTAL_WIDTH - tw - 2, TXT_Y);
        dma_display->print(buf);

        delay(30);
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
        LOGGER.printf("[CLOCK] NTP config requested (TZ=%s)\n", clockTz);
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
        if (hasPendingTextNotification()) return false;
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
        dma_display->setTextColor(dma_display->color565(0, 120, 0));
        dma_display->setCursor(x1, y1);
        dma_display->print(hhmmss);

        dma_display->setTextSize(1);
        dma_display->setTextColor(dma_display->color565(0, 80, 0));
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

static void logGifPlaybackStart(int idx) {
    char path[256];
    bool havePath = false;

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(250))) {
        havePath = getGifPath(idx, path, sizeof(path));
        xSemaphoreGive(sdMutex);
    }

    if (havePath) {
        LOGGER.printf("[PLAY] GIF idx=%d path=%s\n", idx, path);
    } else {
        LOGGER.printf("[PLAY] GIF idx=%d\n", idx);
    }
}

/* =================================================================
 *  LOAD PLAYLIST FROM /lista.txt — stores only byte offsets
 *  Uses a stack buffer to avoid heap-fragmenting String allocations.
 * ================================================================= */
bool loadGifList() {
    LOGGER.println("[..] Opening " LISTA_PATH);
    File f = SD.open(LISTA_PATH, FILE_READ);
    if (!f) {
        LOGGER.println("[ERR] Cannot open " LISTA_PATH);
        return false;
    }

    size_t fsize = f.size();
    LOGGER.printf("[..] File opened, size=%u\n", fsize);

    if (loadGifIndex(fsize)) {
        f.close();
        return true;
    }

    if (SD.exists(LISTA_INDEX_PATH)) {
        LOGGER.println("[IDX] Cache invalid or unreadable; rebuilding");
    } else {
        LOGGER.println("[IDX] Cache missing; building from lista.txt");
    }

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
    LOGGER.printf("[..] Counted %d valid lines\n", count);
    if (count == 0) { f.close(); return false; }

    // --- Allocate offset array (PSRAM preferred, single allocation) ---
    if (gifOffsets) { free(gifOffsets); gifOffsets = nullptr; }
    size_t allocSz = (size_t)count * sizeof(uint32_t);
    gifOffsets = (uint32_t *)(psramFound() ? ps_malloc(allocSz) : malloc(allocSz));
    if (!gifOffsets) {
        LOGGER.printf("[ERR] Failed to alloc %u B for offset table\n", allocSz);
        f.close();
        return false;
    }
    LOGGER.printf("[..] Allocated %u B for %d offsets (%s)\n",
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
    LOGGER.printf("[OK] Indexed %d GIF path(s) from lista.txt\n", gifCount);
    if (!saveGifIndex(fsize, gifCount)) {
        LOGGER.println("[IDX] Warning: cache save failed");
    }
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
        LOGGER.printf("[ERR] open failed: %s\n", path);
        return false;
    }

    size_t sz = f.size();
    if (sz > gifBufCap[bi]) {
        LOGGER.printf("[ERR] Too large (%u B, cap %u): %s\n", sz, gifBufCap[bi], path);
        f.close();
        return false;
    }

    size_t rd = f.read(gifBuf[bi], sz);
    f.close();

    gifBufLen[bi] = rd;
    gifBufOk[bi]  = (rd == sz);
    if (gifBufOk[bi])
        LOGGER.printf("[BUF%d] Loaded %s (%u B)\n", bi, path, sz);
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
        while (gif.playFrame(false, &d) && !hasPendingTextNotification()) {
            unsigned long t = millis();
            while ((millis() - t) < (unsigned long)d) { yield(); }
        }
        gif.close();
        clearBeforeNextGifDraw = false;
    } else {
        LOGGER.printf("[ERR] Decode failed (buf %d)\n", bi);
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
            while (gif.playFrame(false, &d) && !hasPendingTextNotification()) {
                unsigned long t = millis();
                while ((millis() - t) < (unsigned long)d) { yield(); }
            }
            gif.close();
            clearBeforeNextGifDraw = false;
        } else {
            LOGGER.printf("[ERR] Cannot open: %s\n", path);
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
        TextNotification pendingNotif;
        if (takePendingTextNotification(&pendingNotif)) {
            showNotification(pendingNotif.text, pendingNotif.color, pendingNotif.size,
                             pendingNotif.rainbow, pendingNotif.duration,
                             pendingNotif.scrollVertical, pendingNotif.speed);
            if (dma_display) dma_display->clearScreen();
            continue;
        }

        // Dashboard mode loops through cards published by Home Assistant.
        if (dashboardModeEnabled && hasDashboardCards()) {
            TextNotification dashboardCard;
            if (takeNextDashboardCard(&dashboardCard)) {
                if (dashboardCard.cardType == 1) {
                    showSolarCard(dashboardCard.solar_w, dashboardCard.house_w,
                                  dashboardCard.duration);
                } else {
                    showNotification(dashboardCard.text, dashboardCard.color, dashboardCard.size,
                                     dashboardCard.rainbow, dashboardCard.duration,
                                     dashboardCard.scrollVertical, dashboardCard.speed);
                }
                if (dma_display) dma_display->clearScreen();
                continue;
            }
        }

        if (gifCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Double-buffer mode (PSRAM)
        if (hasDualBuf && gifBufOk[playBuf]) {
            logGifPlaybackStart(currentIdx);
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
        logGifPlaybackStart(currentIdx);
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
    LOGGER.println("\n================================");
    LOGGER.println("  DeLorean DMD  128x32");
    LOGGER.println("================================");

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

    LOGGER.printf("[..] Creating display %dx%d (chain=%d)...\n",
                  PANEL_RES_X, PANEL_RES_Y, PANEL_CHAIN);
    LOGGER.printf("[..] Free heap before display: %u\n", ESP.getFreeHeap());

    dma_display = new MatrixPanel_I2S_DMA(cfg);
    if (!dma_display) {
        LOGGER.println("[FATAL] Display allocation failed");
        while (true) delay(1000);
    }

    if (!dma_display->begin()) {
        LOGGER.println("[FATAL] Display init failed");
        while (true) delay(1000);
    }
    dma_display->setBrightness8(min((uint8_t)DEFAULT_BRIGHTNESS, (uint8_t)MAX_BRIGHTNESS));
    dma_display->clearScreen();
    LOGGER.printf("[OK] Display %dx%d initialised\n", TOTAL_WIDTH, PANEL_RES_Y);

    /* ── 2. SD Card ────────────────────────────────────────── */
    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        LOGGER.println("[FATAL] SD mount failed");
        showMessage("SD FAIL", dma_display->color565(255, 0, 0));
        while (true) {
            dma_display->setBrightness8(min((uint8_t)DEFAULT_BRIGHTNESS, (uint8_t)MAX_BRIGHTNESS));
            delay(500);
            dma_display->setBrightness8(0);
            delay(500);
        }
    }
    LOGGER.println("[OK] SD mounted");

    /* ── 3. Mutex ──────────────────────────────────────────── */
    sdMutex = xSemaphoreCreateMutex();
    LOGGER.println("[OK] Mutex created");

    /* ── 4. Load playlist ──────────────────────────────────── */
    LOGGER.println("[..] Loading playlist...");
    if (!loadGifList()) {
        LOGGER.println("[FATAL] No GIFs in " LISTA_PATH);
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
    LOGGER.println("[OK] AnimatedGIF ready");

    /* ── 6. WiFi / Captive Portal ──────────────────────────── */
    // wifiSetup() and mqttSetup() run on Core 0 inside networkTaskFn

    /* ── 7. Allocate PSRAM ping-pong buffers ───────────────── */
    LOGGER.printf("[..] PSRAM found: %s\n", psramFound() ? "YES" : "NO");
    if (psramFound()) {
        gifBuf[0] = (uint8_t *)ps_malloc(MAX_GIF_SIZE);
        gifBuf[1] = (uint8_t *)ps_malloc(MAX_GIF_SIZE);
        hasDualBuf = (gifBuf[0] != nullptr && gifBuf[1] != nullptr);
        if (hasDualBuf) {
            gifBufCap[0] = gifBufCap[1] = MAX_GIF_SIZE;
            LOGGER.printf("[OK] PSRAM double-buffer: 2 x %u KB\n",
                          MAX_GIF_SIZE / 1024);
        }
    }

    if (!hasDualBuf) {
        // Without PSRAM, heap is too limited for dual buffers + DMA display +
        // AnimatedGIF decoder (~22.5 KB).  Use file-based fallback instead.
        LOGGER.println("[WARN] No PSRAM — file-based fallback");
    }

    /* ── 8. Initial GIF load & preload task ───────────────── */
    if (hasDualBuf) {
        loadIntoBuffer(0, 0);
        playBuf = 0;

        if (gifCount > 1)
            loadIntoBuffer(1, 1);

        xTaskCreatePinnedToCore(
            preloadTaskFn, "Preload", 4096, nullptr, 1, &preloadHandle, 0);
        LOGGER.println("[OK] Preload task started on Core 0");
    }

    // Network stack (WiFi + MQTT + portal) on Core 0
    xTaskCreatePinnedToCore(
        networkTaskFn, "Net", 6144, nullptr, 2, &netTaskHandle, 0);
    LOGGER.println("[OK] Network task started on Core 0");

    // GIF playback scheduler on Core 1
    xTaskCreatePinnedToCore(
        playbackTaskFn, "Playback", 6144, nullptr, 1, &playbackHandle, 1);
    LOGGER.println("[OK] Playback task started on Core 1");

    LOGGER.println("[OK] Setup complete — starting playback\n");
}

/* =================================================================
 *  MAIN LOOP — idle (work runs in FreeRTOS tasks)
 * ================================================================= */
void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}
