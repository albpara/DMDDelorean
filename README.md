# DeLorean DMD — 128×32 HUB75 LED Matrix GIF Player

A PlatformIO/Arduino project for **ESP32** that plays animated GIFs on a **128×32 pixel HUB75 LED matrix panel**, reading the playlist from an SD card. Designed for seamless, freeze-free transitions between animations thanks to a PSRAM double-buffer preloading strategy.

---

## Features

- **128×32 HUB75 LED panel** driven via the high-performance [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) library.
- **SD card playlist** — GIF paths are listed in a simple text file (`/lista.txt`).
- **Double-buffer preloading** — while the current GIF plays entirely from RAM (zero SD access), the next GIF is pre-loaded into a second buffer on a background core. Transitions are instant.
- **Anti-ghosting optimizations** — tuned `latch_blanking`, `clkphase`, refresh-rate, and I2S clock speed settings to minimize ghosting artifacts.
- **Automatic centering** — GIFs smaller than 128×32 are centered on the display.
- **Graceful fallback** — works without PSRAM using direct SD file reads (with minor pauses between GIFs).
- **Home Assistant dashboard mode (Phase 1)** — MQTT-driven rotating cards (`text` and `sensor`) with profile and dwell controls.

---

## Hardware Requirements

| Component | Specification |
|-----------|---------------|
| Microcontroller | ESP32 DevKit (ESP32-WROVER with PSRAM recommended) |
| LED Panel | 128×32 HUB75 (two chained 64×32 modules or a single 128×32) |
| SD Card Module | SPI-based, Micro SD |
| SD Card | FAT32 formatted, with GIF files and `lista.txt` |
| Power Supply | 5 V, 4 A+ (depends on panel brightness and content) |

> **Note:** An ESP32 module **with PSRAM** (e.g., ESP32-WROVER) is strongly recommended for the full double-buffer experience. Without PSRAM the firmware falls back to reading GIFs directly from the SD card, which may cause brief freezes during transitions.

---

## Wiring

### HUB75 Panel → ESP32

| Panel Pin | ESP32 GPIO | Function |
|-----------|------------|----------|
| R1  | 25 | Red (upper half) |
| G1  | 26 | Green (upper half) |
| B1  | 27 | Blue (upper half) |
| R2  | 14 | Red (lower half) |
| G2  | 12 | Green (lower half) |
| B2  | 13 | Blue (lower half) |
| A   | 33 | Row address bit 0 |
| B   | 32 | Row address bit 1 |
| C   | 22 | Row address bit 2 |
| D   | 17 | Row address bit 3 |
| E   | GND | Tied to ground (1/16 scan) |
| CLK | 16 | Pixel clock |
| LAT | 4  | Latch |
| OE  | 15 | Output enable |

### SD Card Module → ESP32 (VSPI)

| SD Signal | ESP32 GPIO |
|-----------|------------|
| CS   | 5  |
| CLK  | 18 |
| MOSI | 23 |
| MISO | 19 |

### Power

- Connect the HUB75 panel's **5 V** and **GND** to an adequate power supply.
- The ESP32 can be powered via USB or from the same 5 V supply through its VIN pin.
- **Do not** power the panel from the ESP32's 3.3 V pin.

---

## SD Card Setup

### 1. Format the SD card as **FAT32**.

### 2. Copy your GIF files to the SD card.

GIFs should ideally be **128×32 pixels** (or smaller — they will be centered). Recommended file size: under 300 KB each.

### 3. Create `/lista.txt` in the SD card root.

Add one GIF path per line:

```
/animations/fire.gif
/animations/nyan_cat.gif
/logos/delorean.gif
/scenes/rain.gif
```

**Rules:**
- One file path per line.
- Paths are relative to the SD root (leading `/` is added automatically if missing).
- Lines starting with `#` are treated as comments and ignored.
- Empty lines are ignored.

---

## Building & Flashing

### Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)

### Build

```bash
cd DeLoreanDMD
pio run
```

### Upload to ESP32

```bash
pio run -t upload
```

### Serial Monitor

```bash
pio device monitor
```

You'll see boot messages, SD card status, buffer allocation results, and the name of each GIF as it loads/plays.

---

## Release Procedure

Use this workflow for every new firmware release.

1. Create a release branch (never push directly to `main`):

```bash
git checkout main
git pull origin main
git checkout -b release/v0.2.0
```

2. Apply and validate changes:

```bash
pio run
```

3. Commit and push the release branch:

```bash
git add -A
git commit -m "Release v0.2.0"
git push -u origin release/v0.2.0
```

4. Open a Pull Request and merge into `main`.

5. Create and push the annotated tag from the merged release commit:

```bash
git checkout main
git pull origin main
git tag -a v0.2.0 -m "Release v0.2.0"
git push origin v0.2.0
```

6. Publish a GitHub Release for that tag:
- GitHub UI: Releases -> Draft a new release -> select `v0.2.0` -> publish.
- GitHub CLI (optional):

```bash
gh release create v0.2.0 --title "v0.2.0" --notes "Release notes here"
```

---

## Configuration

All tunable parameters are `#define` constants at the top of `src/main.cpp`:

### Panel Geometry

| Define | Default | Description |
|--------|---------|-------------|
| `PANEL_RES_X` | 64 | Width of a single panel module (pixels) |
| `PANEL_RES_Y` | 32 | Height of a single panel module (pixels) |
| `PANEL_CHAIN` | 2 | Number of panels chained horizontally |

> Total display width = `PANEL_RES_X × PANEL_CHAIN` = 128 pixels.

### Playback

| Define | Default | Description |
|--------|---------|-------------|
| `MAX_GIF_SIZE` | 307200 (300 KB) | Maximum allowed GIF file size in bytes |
| `DEFAULT_BRIGHTNESS` | 128 | Panel brightness (0–255) |
| `LISTA_PATH` | `/lista.txt` | Path to the playlist file on the SD card |

### Anti-Ghosting / Display Quality

| Define | Default | Range | Description |
|--------|---------|-------|-------------|
| `LATCH_BLANKING` | 2 | 1–4 | Latch blanking period. Higher values reduce ghosting at the cost of brightness |
| `MIN_REFRESH_HZ` | 90 | 30–120 | Minimum display refresh rate (Hz) |
| `I2S_CLK_SPEED` | `HZ_10M` | 8/10/16/20 MHz | I2S clock speed for panel data transfer |
| `CLK_PHASE` | `false` | `true`/`false` | Clock phase. Try toggling if you see artifacts |

---

## How It Works

```
┌──────────────────────────────────────────────────────────┐
│                      SD Card                             │
│  /lista.txt   /animations/fire.gif   /logos/logo.gif ... │
└────────────────────────┬─────────────────────────────────┘
                         │
        ┌────────────────┴────────────────┐
        │        Core 0 — Preloader       │
        │  Loads next GIF into inactive   │
        │  PSRAM buffer while current     │
        │  GIF plays                      │
        └────────────────┬────────────────┘
                         │ (buffer swap)
        ┌────────────────┴────────────────┐
        │     Core 1 — GIF Renderer       │
        │  Decodes active buffer with     │
        │  AnimatedGIF, renders frames    │
        │  to HUB75 panel via DMA         │
        └────────────────┬────────────────┘
                         │
        ┌────────────────┴────────────────┐
        │     128×32 HUB75 LED Panel      │
        └─────────────────────────────────┘
```

1. On boot, the firmware reads `/lista.txt` and loads the first GIF into **Buffer A**.
2. **Buffer B** is pre-loaded with the second GIF.
3. The main loop plays GIF frames from Buffer A — **no SD card access occurs during playback**.
4. When the GIF ends, the buffers swap instantly. Buffer A is now free, and the preload task begins loading the next GIF into it.
5. This ping-pong continues indefinitely through the playlist.

---

## Home Assistant Dashboard (Phase 1)

Dashboard mode adds a simple card rotator on top of GIF playback.

- Notifications still have highest priority.
- Dashboard cards run when dashboard mode is enabled and cards are loaded.
- If dashboard mode is disabled (or card list is empty), normal GIF playback continues.

### MQTT Topics

| Topic | Purpose |
|------|---------|
| `{topic}/dashboard/set` | Load dashboard card list (JSON array) |
| `{topic}/dashboard/mode/set` | Enable/disable dashboard mode (`ON`/`OFF`) |
| `{topic}/dashboard/dwell/set` | Default card dwell time in seconds (1..120) |
| `{topic}/dashboard/profile/set` | Profile label (`default`, `night`, etc.) |
| `{topic}/dashboard/state` | Current dashboard state JSON (`enabled`, `dwell`, `profile`, `count`) |

### Supported Card Types

- `text`: plain text message card
- `sensor`: title + value + unit compact card

### Example Payload (Set Dashboard Cards)

Publish this JSON payload to `{topic}/dashboard/set`:

```json
[
        {
                "type": "text",
                "text": "Welcome Home",
                "color": "#00C8FF",
                "size": 1,
                "duration": 4
        },
        {
                "type": "sensor",
                "title": "Temp",
                "value": "22.6",
                "unit": "C",
                "color": "#00FF66",
                "size": 1,
                "duration": 5
        },
        {
                "type": "sensor",
                "title": "Humidity",
                "value": "48",
                "unit": "%",
                "color": "#FFD166",
                "size": 1,
                "duration": 5
        }
]
```

Then enable dashboard mode by publishing `ON` to `{topic}/dashboard/mode/set`.

### Example (Home Assistant Entity Values via Template)

In Home Assistant, publish entity values by templating the payload in an automation/script:

```yaml
action:
        - service: mqtt.publish
                data:
                        topic: delorean-dmd/dashboard/set
                        retain: true
                        payload: >-
                                [
                                        {
                                                "type":"sensor",
                                                "title":"Temp",
                                                "value":"{{ states('sensor.living_room_temperature') }}",
                                                "unit":"C",
                                                "color":"#00FF66",
                                                "duration":5
                                        },
                                        {
                                                "type":"sensor",
                                                "title":"Humidity",
                                                "value":"{{ states('sensor.living_room_humidity') }}",
                                                "unit":"%",
                                                "color":"#FFD166",
                                                "duration":5
                                        },
                                        {
                                                "type":"text",
                                                "text":"Solar {{ states('sensor.solar_power') }} W",
                                                "color":"#00C8FF",
                                                "duration":4
                                        }
                                ]
```

Tip: use `states('entity_id')` in templates so unavailable entities return `unknown` instead of failing template rendering.

---

## Troubleshooting

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Panel stays black | Wrong pin wiring or panel not powered | Double-check all HUB75 connections and 5 V supply |
| Serial says "SD mount failed" | SD card not inserted, bad wiring, or not FAT32 | Re-seat card, verify SPI wiring, format as FAT32 |
| Serial says "No GIFs in lista.txt" | Missing or empty `/lista.txt` | Create the file with at least one valid GIF path |
| Ghosting / faint duplicate rows | Anti-ghosting params need tuning | Increase `LATCH_BLANKING` (max 4), decrease `I2S_CLK_SPEED` |
| Colors are wrong or shifted | `CLK_PHASE` mismatch | Toggle `CLK_PHASE` between `true` and `false` |
| Brief freeze between GIFs | No PSRAM — using file-based fallback | Use an ESP32-WROVER module with PSRAM |
| "GIF too large" errors | GIF file exceeds `MAX_GIF_SIZE` | Optimize the GIF or increase the limit (requires more memory) |
| ESP32 won't boot (GPIO 12) | GPIO 12 pulled high at startup | Ensure the HUB75 panel doesn't pull G2 high during boot |
| Panel flickers at high brightness | Insufficient power supply | Use a 5 V supply rated for ≥ 4 A |

---

## Project Structure

```
DeLoreanDMD/
├── platformio.ini      # PlatformIO config (board, libs, PSRAM flags)
├── src/
│   └── main.cpp        # Complete firmware source
├── AGENTS.md           # Instructions for AI coding agents
└── README.md           # This file
```

---

## Credits & References

- **ESP32-HUB75-MatrixPanel-DMA** — https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA
- **AnimatedGIF** — https://github.com/bitbank2/AnimatedGIF
- **RetroPixelLED** (anti-ghosting reference) — https://github.com/fjgordillo86/RetroPixelLED

---

## License

This project is provided as-is for personal / educational use.
