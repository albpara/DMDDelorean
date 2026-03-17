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
| `src/main.cpp` | All firmware code (pin defs, setup, playback loop, preload task, GIF callbacks) |
| `src/components/app_config.h` | Centralized project constants (pins, playback, MQTT, WiFi, timing defaults) |
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

These values (in `#define` constants in `src/components/app_config.h`) reduce ghosting on HUB75 panels:

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

### Current WiFi boot behavior (saved credentials)
- On boot, if credentials are stored in NVS, STA connect runs in background.
- Firmware retries saved credentials up to `WIFI_BOOT_CONNECT_MAX_RETRIES`.
- Delay between retries: `WIFI_BOOT_RETRY_INTERVAL`.
- Per-attempt timeout: `WIFI_CONNECT_TIMEOUT`.
- If all retries fail, captive portal AP remains active.
- These are internal compile-time constants in `src/components/app_config.h` (not exposed via UI/MQTT).

### Changing panel resolution
Edit `PANEL_RES_X`, `PANEL_RES_Y`, and `PANEL_CHAIN` at the top of `main.cpp`. `TOTAL_WIDTH` is computed automatically.

### Adjusting anti-ghosting
Modify `LATCH_BLANKING` (1–4), `MIN_REFRESH_HZ`, `I2S_CLK_SPEED`, and `CLK_PHASE` defines in `src/components/app_config.h`. Higher latch blanking reduces ghosting but dims the display.

### Adding GIF looping / repeat count
Wrap the `playFromBuffer`/`playFromFile` calls in an outer `for` loop. The GIF decoder replays from the beginning after re-opening.

### Switching to random playback
Replace the sequential `currentIdx = (currentIdx + 1) % gifList.size()` with `currentIdx = esp_random() % gifList.size()` and update the preload target accordingly.

### Text notification system (MQTT & web UI)

A `TextNotification` struct (defined in `mqtt.h`) is shared between the MQTT module and `main.cpp`. Fields:

| Field | Type | Description |
|-------|------|-------------|
| `text` | `char[256]` | Message to display |
| `color` | `uint16_t` | RGB565 foreground colour |
| `size` | `uint8_t` | Font scale 1–3 (Adafruit GFX `setTextSize`) |
| `rainbow` | `bool` | Animate each character in a different hue |
| `duration` | `uint32_t` | For scrolling text: loop count. For non-scrolling text: seconds |
| `pending` | `volatile bool` | Set by MQTT/web, cleared by `loop()` |

**MQTT topic:** `{topic}/notify` (e.g. `delorean-dmd/notify`)  
**Payload (JSON):**
```json
{"text":"Hello!","color":"#FF8800","size":2,"effect":"rainbow","duration":5}
```
All fields except `text` are optional and fall back to defaults (`color=#FFFFFF`, `size=1`, no rainbow, `duration=5`).

**Web UI:** The captive portal includes a *Text Notification* section — fill in the message, pick a colour, size (1–3), effect, and duration, then click **Send Notification**.

**`showMessage` signature** (used internally for startup messages):
```cpp
void showMessage(const char *msg, uint16_t color, uint8_t size = 1);
```

**`showNotification` function** (used by `loop()` for MQTT/web notifications):
```cpp
void showNotification(const char *msg, uint16_t color, uint8_t size,
                      bool rainbow, uint32_t duration);
```

**`applyTextNotification` function** (shared parser — called by both MQTT callback and HTTP handler):
```cpp
void applyTextNotification(const char *payload);
```
Accepts a JSON payload (as above) or plain text (uses all defaults). Both MQTT and HTTP routes delegate to this function to avoid duplicating parsing logic.

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

---

## Recent Firmware Updates (2026-03-17)

1. **Centralized configuration:** moved firmware constants from multiple files into `src/components/app_config.h`.
2. **WiFi robustness at boot:** added background retry logic for saved credentials (internal compile-time settings only).
3. **MQTT/runtime defaults cleanup:** introduced a startup defaults initializer in `src/components/mqtt.cpp` before loading NVS overrides.
4. **Pin planning aid:** documented currently free GPIOs for future button inputs in `src/components/app_config.h` comments.
