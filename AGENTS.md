# DeLorean DMD — AI Agent Instructions

## Project Overview

This is a PlatformIO / Arduino firmware for an **ESP32** driving a **128×32 HUB75 LED matrix panel**.
The device reads animated GIF file paths from `/lista.txt` on an SD card and plays them
on the panel with a **PSRAM ping-pong double-buffer** strategy for seamless transitions.
Playback order is controlled by `RANDOM_PLAYBACK` in `src/components/app_config.h` and is
currently **random by default**.

---

## Maintenance Rule

**Any relevant firmware change must be added to this file.**

That includes changes to:
- task/core layout
- playback behavior
- SD / PSRAM buffering
- MQTT topics / payloads / Home Assistant discovery
- captive portal routes or UI behavior
- notification semantics
- clock rendering or timing behavior
- pin map / hardware assumptions
- defaults and compile-time constants in `src/components/app_config.h`

If behavior changes and this file is not updated, future agents will make wrong assumptions.

---

## Git Workflow Rule (Mandatory)

- Start **every coding session** by creating a dedicated branch from `main`.
- Branch naming should be descriptive, for example: `feat/<topic>`, `fix/<topic>`, `chore/<topic>`.
- **Never push directly to `main`.**
- All changes must be pushed to the session branch and integrated via Pull Request.
- Tags/releases should be created from the merged release commit, not from unmerged local work.

Example flow:

```bash
git checkout main
git pull origin main
git checkout -b chore/update-docs
```

---

## Architecture

| Layer | Description |
|-------|-------------|
| **Display** | ESP32-HUB75-MatrixPanel-I2S-DMA library. Single DMA buffer (double DMA is disabled to preserve correct GIF inter-frame state with transparency). |
| **GIF Decoder** | `bitbank2/AnimatedGIF`. Supports both memory-based (`open(buf, size, cb)`) and file-based (`open(path, openCB, closeCB, readCB, seekCB, drawCB)`) decoding. |
| **Playlist Index** | A compact offset table (`gifOffsets`) is used instead of storing full path strings in RAM. The table is cached on SD at `/lista.idx` and reused on boot when `/lista.txt` size matches. If cache is missing/invalid, firmware rebuilds from `/lista.txt` and writes a fresh cache. |
| **Double Buffer** | Two PSRAM buffers (`gifBuf[0]`, `gifBuf[1]`). While Core 1 plays one buffer, a FreeRTOS task on Core 0 pre-loads the next GIF into the other. Buffers swap instantly between GIFs. Falls back to direct SD file reads when PSRAM is not available. |
| **SD Card** | SPI (VSPI) bus, protected by a FreeRTOS mutex (`sdMutex`). SD access is serialized because playlist lookup, preload, playback fallback, and playback debug path logging all touch the card. |
| **Networking** | WiFi captive portal, HTTP API, DNS captive redirection, and MQTT all run on a dedicated FreeRTOS task (`networkTaskFn`) on Core 0. |
| **Playback Scheduler** | `playbackTaskFn` on Core 1 arbitrates between notifications, GIF playback, and clock mode. |

### Core Assignment

| Core | Responsibility |
|------|----------------|
| Core 0 | `preloadTaskFn` for SD preloading and `networkTaskFn` for WiFi, DNS, HTTP, and MQTT |
| Core 1 | `playbackTaskFn` for notifications, GIF playback, and clock display |

### Main Execution Model

- `setup()` initializes hardware, SD, playlist index, decoder, optional PSRAM buffers, and FreeRTOS tasks.
- Arduino `loop()` is intentionally idle and only sleeps.
- The playback task has priority over GIF content selection and will show queued notifications before returning to GIF playback.

---

## Key Files

| File | Purpose |
|------|---------|
| `platformio.ini` | Board, framework, libraries, PSRAM build flags |
| `src/main.cpp` | Playback scheduler, GIF decode/render pipeline, clock mode, task creation, SD playlist indexing |
| `src/components/app_config.h` | Centralized project constants (pins, playback, MQTT, WiFi, timing defaults) |
| `src/components/mqtt.h` | Shared runtime state structures and interfaces used by MQTT, portal, and playback |
| `src/components/mqtt.cpp` | MQTT config, Home Assistant discovery, notification queue, panel state, clock config persistence |
| `src/components/mqtt_logger.h` | `MqttLogger` class (tees `Serial` + MQTT); global `LOGGER` used everywhere instead of `Serial` for log output |
| `src/components/wifi_portal.h` | Web portal / DNS service declarations |
| `src/components/wifi_portal.cpp` | Captive portal routes, WiFi credential handling, HTTP API, MQTT servicing |
| `src/components/portal_html.h` | Inline captive portal HTML/JS payload served at `/` |
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
| `LATCH_BLANKING` | 1 | 1–4 | Higher = less ghosting, slightly lower brightness |
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

### Extending WiFi / Web UI
WiFi, captive portal, DNS redirection, HTTP API, and MQTT already exist. They run inside `networkTaskFn` on Core 0. If you extend them, preserve the current split: networking on Core 0, playback on Core 1, and SD access protected by `sdMutex`.

### Current WiFi boot behavior (saved credentials)
- On boot, if credentials are stored in NVS, STA connect runs in background.
- Firmware retries saved credentials up to `WIFI_BOOT_CONNECT_MAX_RETRIES`.
- Delay between retries: `WIFI_BOOT_RETRY_INTERVAL`.
- Per-attempt timeout: `WIFI_CONNECT_TIMEOUT`.
- If all retries fail, captive portal AP remains active.
- These are internal compile-time constants in `src/components/app_config.h` (not exposed via UI/MQTT).

### Changing panel resolution
Edit `PANEL_RES_X`, `PANEL_RES_Y`, and `PANEL_CHAIN` in `src/components/app_config.h`. `TOTAL_WIDTH` is computed automatically.

### Adjusting anti-ghosting
Modify `LATCH_BLANKING` (1–4), `MIN_REFRESH_HZ`, `I2S_CLK_SPEED`, and `CLK_PHASE` defines in `src/components/app_config.h`. Higher latch blanking reduces ghosting but dims the display.

### Adding GIF looping / repeat count
Wrap the `playFromBuffer`/`playFromFile` calls in an outer `for` loop. The GIF decoder replays from the beginning after re-opening.

### Switching to random playback
Set `RANDOM_PLAYBACK` in `src/components/app_config.h`. The playlist scheduler already supports both random and sequential modes through `nextIdx()`.

### Text notification system (MQTT & web UI)

A `TextNotification` struct (defined in `mqtt.h`) is shared between the MQTT module and `main.cpp`. Fields:

| Field | Type | Description |
|-------|------|-------------|
| `text` | `char[256]` | Message to display |
| `color` | `uint16_t` | RGB565 foreground colour |
| `size` | `uint8_t` | Font scale 1–3 (Adafruit GFX `setTextSize`) |
| `rainbow` | `bool` | Animate each character in a different hue |
| `duration` | `uint32_t` | For scrolling text: loop count. For non-scrolling text: seconds |
| `pending` | `volatile bool` | Legacy shared flag field; notifications are now consumed by `playbackTaskFn()` through the FIFO queue helpers |
| `scrollVertical` | `bool` | When `true`, text is word-wrapped and scrolled upward (bottom → top) instead of horizontally |

Current runtime semantics:
- Notifications are **queued FIFO**, not replace-latest.
- Queue depth is bounded by `NOTIFY_QUEUE_LEN` in `src/components/app_config.h` and is currently `4`.
- If the queue is full, new notifications are dropped and a serial log is emitted.
- GIF playback and clock mode yield early when the notification queue becomes non-empty.
- An already-running notification is **not** interrupted by newer queued notifications.

**MQTT topic:** `{topic}/notify` (e.g. `delorean-dmd/notify`)  
**Payload (JSON):**
```json
{"text":"Hello!","color":"#FF8800","size":2,"effect":"rainbow","duration":5}
```
All fields except `text` are optional and fall back to defaults (`color=#FFFFFF`, `size=1`, no rainbow, `duration=5`).

**Horizontal scroll speed** — add `"speed":"slow"` for half-speed scrolling (2× step delay):
```json
{"text":"This is a long message that scrolls","speed":"slow","color":"#FFFFFF","duration":2}
```
- `"speed":"fast"` (default) uses `SCROLL_STEP_MS` (30 ms/pixel).
- `"speed":"slow"` doubles the delay to `SCROLL_STEP_MS * 2` (60 ms/pixel).
- Speed only affects scrolling; static (fits-on-panel) text always uses the base refresh rate.
- Horizontal text is **left-aligned** (not centered) when it fits on the panel.
- `\n` (real newline or JSON-escaped `\\n`) in horizontal text is **replaced with a space** — use `"scroll":"vertical"` if multi-line display is needed.

**Vertical scrolling** — add `"scroll":"vertical"` to the payload:
```json
{"text":"Temp: 22C\nHumidity: 60%\nPressure: 1013hPa","scroll":"vertical","color":"#00FF88","size":1,"duration":2}
```
- Text is split on `\n` (JSON-escaped or raw) into separate lines.
- Long lines are automatically word-wrapped to fit the panel width at the chosen font size.
- `duration` = number of full bottom-to-top scroll loops (default 1).
- Works with `"effect":"rainbow"` too.
- Also supported for dashboard cards via `{topic}/dashboard/set`.

**HTTP route:** `POST /notify` exists and uses the same parser as MQTT.

**Important:** the backend supports HTTP notification injection, but the current captive portal HTML does **not** expose a notification form section. Do not assume the web UI contains controls unless `src/components/portal_html.h` has been updated accordingly.

**`showMessage` signature** (used internally for startup messages):
```cpp
void showMessage(const char *msg, uint16_t color, uint8_t size = 1);
```

**`showNotification` function** (used by `loop()` for MQTT/web notifications):
```cpp
void showNotification(const char *msg, uint16_t color, uint8_t size,
                      bool rainbow, uint32_t duration, bool scrollVertical = false,
                      uint8_t speed = 0);
```

**`applyTextNotification` function** (shared parser — called by both MQTT callback and HTTP handler):
```cpp
void applyTextNotification(const char *payload);
```
Accepts a JSON payload (as above) or plain text (uses all defaults). Both MQTT and HTTP routes delegate to this function to avoid duplicating parsing logic.

### MQTT / Home Assistant entities

The firmware currently publishes Home Assistant discovery for:
- main light entity (`{topic}/set`, `{topic}/state`)
- clock mode switch (`{topic}/clock/set`, `{topic}/clock/state`)
- clock cadence number (`{topic}/clock/every/set`, `{topic}/clock/state`)
- reboot button (`{topic}/reboot/set`)
- dashboard mode switch (`{topic}/dashboard/mode/set`, `{topic}/dashboard/state`)
- dashboard dwell number (`{topic}/dashboard/dwell/set`, `{topic}/dashboard/state`)
- dashboard profile select (`{topic}/dashboard/profile/set`, `{topic}/dashboard/state`)
- **safe brightness switch** (`{topic}/brightness/safe/set`, `{topic}/brightness/safe/state`) — `entity_category: config`
- **log forwarding switch** (`{topic}/log/set`, `{topic}/log/state`) — `entity_category: diagnostic`

Other important MQTT topics:
- brightness set topic: `{topic}/brightness/set`
- notification topic: `{topic}/notify`
- dashboard card list topic: `{topic}/dashboard/set`
- availability topic: `{topic}/available`
- log output topic: `{topic}/log` (published when log forwarding is enabled)

### Safe Brightness (power-protection cap)

- `MAX_BRIGHTNESS` is a compile-time cap (currently **40**, ~16%) to protect the ESP32 when the panel draws power through the USB rail.
- At runtime, `safeBrightness` (bool, default `true`) controls whether the cap is enforced.
- When `safeBrightness` is `true`: all brightness values are silently clamped to `MAX_BRIGHTNESS_VAL` before reaching hardware.
- When `safeBrightness` is `false`: the full 0–255 range is allowed (for use with a dedicated panel PSU).
- The setting persists in NVS namespace `panel`, key `safe_br`.
- Every call to `applyBrightness()` emits a serial log: `[BRIGHT] requested=N effective=N safe=ON|OFF`.
- The HA light entity `brightness_scale` is set dynamically: `MAX_BRIGHTNESS_VAL` when safe is ON, `255` when safe is OFF. `mqttPublishDiscovery()` is re-called when the setting changes so HA's slider updates.
- `applySafeBrightness(bool)` — set + persist + re-apply brightness + re-publish discovery.
- `loadPanelConfig()` — load `safe_br` from NVS; called by `mqttSetup()`.

### Dashboard mode details (Phase 1)

- Dashboard mode is opt-in via MQTT (`{topic}/dashboard/mode/set`), OFF by default.
- Dashboard payload accepts a JSON array of cards on `{topic}/dashboard/set`.
- Supported card types: `text`, `sensor`, and **`solar`** (energy overview — see Solar energy card details below).
- Cards are rotated round-robin by `playbackTaskFn()` on Core 1 when dashboard mode is enabled and at least one card exists.
- Notification queue still has highest priority; queued notifications preempt dashboard cards.
- Dashboard settings persist in NVS namespace `dash`:
    - `enabled` (bool)
    - `dwell` (seconds, clamped 1..120)
    - `profile` (string)
- Dashboard state is published as JSON to `{topic}/dashboard/state`:
    - `enabled`
    - `dwell`
    - `profile`
    - `count` (loaded cards)

### Clock mode details

- Clock mode uses NTP via `configTzTime()` and the POSIX timezone string stored in `clockTz`.
- `showClockMode()` can hold the clock on-screen until the next PSRAM buffer is ready, avoiding a black gap between clock and GIF playback.
- The current clock rendering uses **dark green** text: brighter green for time and dimmer green for date.
- The minimum clock screen time is `CLOCK_MIN_DISPLAY_SECONDS`.

### Playback debug logging

- When a GIF starts, the firmware emits a serial log line in the form `[PLAY] GIF idx=N path=/path/file.gif`.
- If the path cannot be resolved quickly, it logs `[PLAY] GIF idx=N`.
- Preload completion logs use `[BUF0]` / `[BUF1]` prefixes.

### Captive portal / HTTP API routes

Routes currently registered by `wifiSetup()`:
- `GET /` portal HTML
- `GET /scan`
- `POST /connect`
- `GET /status`
- `POST /mqtt`
- `POST /panel`
- `POST /clock`
- `POST /notify`
- `POST /log` (toggle MQTT log forwarding — `enabled=0|1`)
- `POST /update` (OTA firmware upload — multipart/form-data, field `firmware`, binary `.bin`; device reboots on success)
- captive-portal redirects for common OS probe URLs

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
6. **SD access must stay serialized through `sdMutex`.** Even debug/path lookup code should not bypass it.
7. **`showNotification()` is a blocking renderer.** Network responsiveness is preserved only because WiFi/MQTT/HTTP live on the separate Core 0 network task.
8. **Some docs may lag the code.** In this repo, prefer `src/components/app_config.h`, `src/components/mqtt.cpp`, `src/components/wifi_portal.cpp`, and `src/main.cpp` over README assumptions.

---

## Recent Firmware Updates (2026-03-27)

15. **MQTT log forwarding:**
        - All `Serial.printf/println` log calls in `main.cpp`, `mqtt.cpp`, and `wifi_portal.cpp` now go through a global `LOGGER` object (`MqttLogger` class in `src/components/mqtt_logger.h`) instead of calling `Serial` directly.
        - `MqttLogger` extends `Print`, wraps the hardware `Serial`, and buffers characters per-line. When `mqttLogForwardingEnabled` is `true` and MQTT is connected, each completed log line is published to `{topic}/log` (non-retained, QoS 0).
        - A re-entrancy guard (`_guard` bool) prevents recursive MQTT publishes (i.e., the act of publishing a log line will not itself trigger another publish).
        - `mqttLogForwardingEnabled` (bool, default `false`) persists in NVS namespace `mqtt`, key `log_fwd`.
        - `applyMqttLogForwarding(bool en)` — set + persist + publish new `{topic}/log/state`.
        - `mqttPublishLog(const char* line)` — publish a single stripped log line to `{topic}/log`.
        - **HA discovery:** new `switch` entity (`entity_category: diagnostic`, icon `mdi:math-log`) subscribes to `{topic}/log/set` and publishes state to `{topic}/log/state`.
        - **HTTP route:** `POST /log` accepts `enabled=0|1`; uses `applyMqttLogForwarding()`.
        - **Portal UI:** "Log forwarding" toggle added to the MQTT section; state is loaded from `GET /status` (`log_fwd` bool field).
        - `GET /status` response now includes `"log_fwd": true|false`.
        - New compile-time constants in `src/components/app_config.h`:
            - `MQTT_LOG_TOPIC_SUFFIX` — suffix for the log topic (default `"log"`)
            - `DEFAULT_MQTT_LOG_FORWARDING` — default enabled state (default `false`)

## Recent Firmware Updates (2026-03-25)

14. **Horizontal scroll improvements:**
        - **`\n` stripping:** `showNotification()` now sanitizes horizontal text before rendering — real `\n` characters and JSON-escaped `\\n` two-char sequences are replaced with spaces so the message flows as a single line. (Vertical scroll still handles `\n` as line breaks unchanged.)
        - **Scroll speed control:** `TextNotification` struct gained a new `speed` (uint8_t) field (`0`=fast/default, `1`=slow). `applyTextNotification()` parses `"speed":"slow"` and `applyDashboardPayload()` also parses it for individual dashboard cards. `showNotification()` signature extended with `uint8_t speed = 0`; when `speed == 1` the scroll step delay is doubled (`SCROLL_STEP_MS * 2`). Speed only affects scrolling text; static (fits-on-panel) text always uses the base `SCROLL_STEP_MS` refresh rate.
        - **Left-aligned static text:** Non-scrolling horizontal text is now left-aligned (`cx = 0`) instead of centered.
        - `playbackTaskFn()` passes `speed` at both notification and dashboard card call sites.
        - **MQTT payload examples:**
            ```json
            {"text":"Fast scrolling message","speed":"fast","duration":2}
            {"text":"Slow scrolling message","speed":"slow","color":"#FFAA00","duration":3}
            ```

13. **Vertical text scrolling:**
        - `TextNotification` struct gained a new `scrollVertical` (bool) field, initialized to `false` by `resetTextNotification()`.
        - `applyTextNotification()` parses `"scroll":"vertical"` from the JSON payload and sets the field; copies it when enqueuing.
        - `applyDashboardPayload()` also parses `"scroll":"vertical"` for individual dashboard cards.
        - Added `VSCROLL_MAX_LINES` (32) constant, `buildWrappedLines()` helper, and `showVerticalScroll()` function in `src/main.cpp`.
            - `buildWrappedLines()` splits text on `\n` (actual or JSON-escaped two-char `\n`) and auto-wraps long lines at word boundaries.
            - `showVerticalScroll()` renders all lines scrolling upward (bottom → top) at `SCROLL_STEP_MS` per pixel step; each line is horizontally centred; supports rainbow effect and loop count via `duration`.
        - `showNotification()` signature extended with `bool scrollVertical` parameter; dispatches to `showVerticalScroll()` when set.
        - `playbackTaskFn()` passes `scrollVertical` at both notification and dashboard card call sites.
        - **MQTT payload example:**
            ```json
            {"text":"Temp: 22C\nHumidity: 60%\nPressure: 1013","scroll":"vertical","color":"#00FF88","size":1,"duration":2}
            ```

## Recent Firmware Updates (2026-03-24)

12. **Solar energy dashboard card (`type: solar`):**
        - New graphical card type rendered entirely with filled rectangles — no text scrolling.
        - **Top 16 rows** — horizontal bar:
                - *Surplus (solar ≥ house):* scale = 3000 W; purple = house consumption, green = solar excess, dim = unused headroom.
                - *Deficit (solar < house):* scale = house_w (bar fills 100 %); green = solar coverage, red = grid draw.
        - **Bottom 16 rows** — house wattage left in purple, net (solar-house) centred, and solar wattage right in orange (all with "W" suffix).
            - Net colour: green when exporting (`solar_w >= house_w`), red when importing (`solar_w < house_w`).
        - `TextNotification` struct gained three new fields: `cardType` (uint8_t, 0=text, 1=solar), `solar_w` (int32_t), `house_w` (int32_t). `resetTextNotification()` zeroes all three.
        - **MQTT payload** on `{topic}/dashboard/set`:
            ```json
            [{"type":"solar","solar_w":2340,"house_w":1850}]
            ```
            `duration` (seconds) is optional; defaults to `dashboardDwellSeconds`. Can be mixed with other card types in the same array.
        - `showSolarCard(solar_w, house_w, duration)` added to `src/main.cpp`.
        - `playbackTaskFn()` routes cards with `cardType == 1` to `showSolarCard()` instead of `showNotification()`.

## Recent Firmware Updates (2026-03-19)

1. **Centralized configuration:** moved firmware constants from multiple files into `src/components/app_config.h`.
2. **WiFi robustness at boot:** added background retry logic for saved credentials (internal compile-time settings only).
3. **MQTT/runtime defaults cleanup:** introduced a startup defaults initializer in `src/components/mqtt.cpp` before loading NVS overrides.
4. **Pin planning aid:** documented currently free GPIOs for future button inputs in `src/components/app_config.h` comments.
5. **Notification queue semantics:** replaced shared live notification mutation with a bounded FIFO queue (`NOTIFY_QUEUE_LEN`, currently 4). New notifications wait their turn; overflow is dropped with a serial log.
6. **Playback debug trace:** added serial logging for the GIF being played (`[PLAY] GIF idx=... path=...`).
7. **Clock visual tuning:** clock mode now renders time/date in dark green tones instead of white/gray.
8. **Phase 1 HA dashboard mode:** added MQTT-driven rotating dashboard cards (`text`/`sensor`), persisted dashboard settings (`enabled`, `dwell`, `profile`), and Home Assistant discovery entities for dashboard mode, dwell, and profile.
9. **OTA firmware update:** added `POST /update` HTTP endpoint (multipart `firmware` field) using the ESP32 `Update` library. The captive portal UI now includes an "Upload Firmware" section with a file picker and progress bar. The device reboots automatically on success. Yellow "OTA..." message shown on panel during upload.
10. **Safe brightness cap tuning and toggle:**
    - Lowered `MAX_BRIGHTNESS` from 80 → **40** (~16%) for safer operation when the panel is powered via the ESP32/USB rail without a dedicated PSU.
    - Added `DEFAULT_SAFE_BRIGHTNESS` constant (`true` by default).
    - Added `bool safeBrightness` runtime variable (NVS-persisted in `panel/safe_br`).
    - `applyBrightness()` now emits `[BRIGHT] requested=N effective=N safe=ON|OFF` log on every call.
    - `applySafeBrightness(bool)` toggles the cap, persists it, re-applies current brightness and re-publishes HA discovery.
    - `loadPanelConfig()` loads `safe_br` from NVS at boot (called by `mqttSetup()`).
    - Added **"DMD Safe Brightness"** switch to HA discovery (`entity_category: config`, topic `{topic}/brightness/safe/set`).
    - Light entity `brightness_scale` is now dynamic: `MAX_BRIGHTNESS_VAL` when safe=ON, 255 when safe=OFF.
    - `POST /panel` accepts optional `safe=0|1` argument; `GET /status` returns `safe_brightness` bool.
11. **Playlist index cache on SD:**
    - Added `/lista.idx` binary cache for `gifOffsets` to reduce startup delay on large playlists.
    - Boot now attempts to load `/lista.idx` first and validates it against `/lista.txt` file size.
    - If cache is missing/invalid, firmware rebuilds offsets from `/lista.txt` and writes a fresh `/lista.idx`.
    - Added compile-time constants in `src/components/app_config.h`: `LISTA_INDEX_PATH`, `LISTA_INDEX_MAGIC`, `LISTA_INDEX_VERSION`.
