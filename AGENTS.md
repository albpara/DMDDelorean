# DeLorean DMD — AI Agent Instructions

## Project Overview

This is a PlatformIO / Arduino firmware for an **ESP32** driving a **128×32 HUB75 LED matrix panel**.
The device reads animated GIF file paths from `/lista.txt` on an SD card and plays them
sequentially on the panel with a **PSRAM ping-pong double-buffer** strategy for seamless transitions.

---

## Architecture

| Layer | Description |
|-------|-------------|
| **Display** | ESP32-HUB75-MatrixPanel-I2S-DMA library. Single DMA buffer (double DMA is disabled to preserve correct GIF inter-frame state with transparency). |
| **GIF Decoder** | `bitbank2/AnimatedGIF`. Supports both memory-based (`open(buf, size, cb)`) and file-based (`open(path, openCB, closeCB, readCB, seekCB, drawCB)`) decoding. |
| **Double Buffer** | Two PSRAM buffers (`gifBuf[0]`, `gifBuf[1]`). While Core 1 plays one buffer, a FreeRTOS task on Core 0 pre-loads the next GIF into the other. Buffers swap instantly between GIFs. Falls back to direct SD file reads when PSRAM is not available. |
| **SD Card** | SPI (VSPI) bus, protected by a FreeRTOS mutex (`sdMutex`). Only the preload task and the fallback playback path access the SD. |
| **Text Notifications** | `TextNotification` struct holds text, RGB565 color, font size (1-3), effect (static/scroll/blink), and duration. Activated by MQTT or HTTP POST. `loop()` checks `textNotif.active` first; GIF frame-delay loops also exit early so notifications appear within one frame interval. |

### Core Assignment

| Core | Responsibility |
|------|----------------|
| Core 0 | `preloadTaskFn` — loads the next GIF from SD into the inactive buffer |
| Core 1 | Arduino `loop()` — decodes & renders the active GIF on the panel |

---

## Key Files

| File | Purpose |
|------|---------|
| `platformio.ini` | Board, framework, libraries, PSRAM build flags |
| `src/main.cpp` | All firmware code (pin defs, setup, playback loop, preload task, GIF callbacks, `handleTextNotification`) |
| `src/components/mqtt.h` | Shared types: `TextNotification` struct, effect constants, extern declarations |
| `src/components/mqtt.cpp` | MQTT client, `applyTextNotification()` (JSON/plain-text parser), notification topic subscription |
| `src/components/wifi_portal.cpp` | Web server routes including `/notify` POST endpoint |
| `src/components/portal_html.h` | Single-file captive portal UI (WiFi, MQTT, Panel, Text Display) |
| `AGENTS.md` | This file — context for AI coding agents |
| `README.md` | Human-facing documentation |

---

## Pin Map

### HUB75 Panel

| Signal | GPIO | Notes |
|--------|------|-------|
| R1     | 25   | Upper-half red |
| G1     | 26   | Upper-half green |
| B1     | 27   | Upper-half blue |
| R2     | 14   | Lower-half red |
| G2     | 12   | Lower-half green |
| B2     | 13   | Lower-half blue |
| A      | 33   | Row address |
| B      | 32   | Row address |
| C      | 22   | Row address |
| D      | 17   | Row address |
| E      | GND  | Tied low (1/16 scan) — firmware uses `-1` |
| CLK    | 16   | Pixel clock |
| LAT    | 4    | Latch |
| OE     | 15   | Output enable |

### SD Card (VSPI)

| Signal | GPIO |
|--------|------|
| CS     | 5    |
| CLK    | 18   |
| MOSI   | 23   |
| MISO   | 19   |

---

## Anti-Ghosting Settings

These values (in `#define` constants at the top of `main.cpp`) reduce ghosting on HUB75 panels:

| Parameter | Default | Range | Effect |
|-----------|---------|-------|--------|
| `LATCH_BLANKING` | 2 | 1–4 | Higher = less ghosting, slightly lower brightness |
| `MIN_REFRESH_HZ` | 90 | 30–120 | Minimum panel refresh rate |
| `I2S_CLK_SPEED` | `HZ_10M` | 8/10/16/20 MHz | I2S bus speed |
| `CLK_PHASE` | `false` | true/false | Clock phase inversion |

Derived from the [RetroPixelLED](https://github.com/fjgordillo86/RetroPixelLED) project.

---

## SD Card Content Format

The SD card root must contain a file `/lista.txt` with one GIF path per line:

```
/animations/fire.gif
/animations/nyan.gif
/logos/delorean.gif
```

- Paths are relative to the SD card root.
- Leading `/` is added automatically if missing.
- Empty lines and lines starting with `#` are ignored.
- GIF files should be sized for 128×32 pixels (they will be centered if smaller).
- Maximum file size per GIF: 300 KB (configurable via `MAX_GIF_SIZE`).

---

## Important Library Notes

### ESP32-HUB75-MatrixPanel-I2S-DMA
- Repository: https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA.git
- Header: `<ESP32-HUB75-MatrixPanel-I2S-DMA.h>`
- Key config struct: `HUB75_I2S_CFG` (pins, timing, chain length, double buffering)
- The display object is `MatrixPanel_I2S_DMA *dma_display`.

### AnimatedGIF (bitbank2)
- PlatformIO lib name: `bitbank2/AnimatedGIF`
- Two open modes: `open(buffer, size, drawCB)` for memory, `open(path, open, close, read, seek, drawCB)` for file.
- `playFrame(true, &delay)` decodes one frame, handles inter-frame timing, returns 0 when the animation ends.
- Must call `gif.begin(LITTLE_ENDIAN_PIXELS)` before use (matches ESP32 byte order).

---

## Common Tasks for AI Agents

### Adding WiFi / Web UI
Add `WiFi.h`, `WebServer.h`, and `WiFiManager.h`. Run the web server on Core 0 in `loop()` and move GIF playback to a dedicated task on Core 1. Protect SD access with the existing `sdMutex`.

### Changing panel resolution
Edit `PANEL_RES_X`, `PANEL_RES_Y`, and `PANEL_CHAIN` at the top of `main.cpp`. `TOTAL_WIDTH` is computed automatically.

### Adjusting anti-ghosting
Modify `LATCH_BLANKING` (1–4), `MIN_REFRESH_HZ`, `I2S_CLK_SPEED`, and `CLK_PHASE` defines. Higher latch blanking reduces ghosting but dims the display.

### Adding GIF looping / repeat count
Wrap the `playFromBuffer`/`playFromFile` calls in an outer `for` loop. The GIF decoder replays from the beginning after re-opening.

### Switching to random playback
Replace the sequential `currentIdx = (currentIdx + 1) % gifList.size()` with `currentIdx = esp_random() % gifList.size()` and update the preload target accordingly.

### Text Notifications

Text notifications interrupt GIF playback immediately (within one GIF frame) and resume after the notification duration expires.

**Trigger via MQTT** — publish to `{mqttTopic}/notify`:
```json
{"text":"Hello!","color":[255,128,0],"size":2,"effect":"scroll","duration":8000}
```
Plain-text payloads (no braces) are accepted and use defaults (white, size 1, scroll, 8 s).

**Trigger via HTTP** — POST to `/notify`:
```
text=Hello!&color=255,128,0&size=2&effect=scroll&duration=8000
```

**Parameters**

| Field | Type | Default | Description |
|-------|------|---------|-------------|
| `text` | string | — | Message to display (required) |
| `color` | `[r,g,b]` / `r,g,b` | `[255,255,255]` | RGB 0–255 |
| `size` | 1 / 2 / 3 | 1 | Adafruit GFX text scale |
| `effect` | `scroll` / `static` / `blink` | `scroll` | Display effect |
| `duration` | integer (ms) | 8000 | Display time; 0 = 8 s default |

**Layout notes (128 × 32 panel)**
- Size 1: 6 × 8 px/char → up to 21 chars without scrolling
- Size 2: 12 × 16 px/char → up to 10 chars; fits 16 px vertical centred
- Size 3: 18 × 24 px/char → up to 7 chars; fits 24 px vertical centred

**Shared state** — `TextNotification textNotif` is defined in `mqtt.cpp` and consumed in `main.cpp`. All accesses are on Core 1 (Arduino loop + MQTT callback), so no mutex is required.

---

## Build & Flash

```bash
# Build
pio run

# Upload
pio run -t upload

# Monitor serial
pio device monitor
```

---

## Constraints & Gotchas

1. **DMA double buffer is OFF for GIFs.** Enabling `cfg.double_buff = true` breaks GIF transparency because the AnimatedGIF decoder relies on the previous frame being intact in the draw buffer.
2. **PSRAM is strongly recommended.** Without it the firmware falls back to file-based reads, which can cause brief pauses between GIFs.
3. **SD SPI pins overlap with default VSPI.** If you add other SPI devices, mind bus contention.
4. **`MAX_GIF_SIZE` caps memory use.** GIFs larger than this value are skipped with a serial error.
5. **GPIO 12 (`G2_PIN`) is a bootstrap pin.** Ensure it's not pulled high at boot or the ESP32 may fail to start. The HUB75 panel typically pulls it low, so this is usually fine.
