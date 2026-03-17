#pragma once

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

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

/*
 * Free GPIOs for future buttons (ESP32 dev module, current pin map):
 * - Recommended (no current assignment here): 21, 34, 35, 36, 39
 * - Also free but boot/serial sensitive: 0, 2, 1(TX0), 3(RX0)
 *
 * Notes:
 * - GPIO 34/35/36/39 are input-only and do NOT have internal pull-up/down,
 *   so buttons on these pins need external resistors.
 * - GPIO 0 and 2 are boot strap pins; use with care to avoid boot issues.
 * - GPIO 1/3 are USB-serial UART pins used for flashing/monitor.
 * - GPIO 6..11 are connected to onboard flash and must not be used.
 */

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
#define MAX_GIF_SIZE       (300 * 1024)  // 300 KB ceiling per GIF file
#define LISTA_PATH         "/lista.txt"
#define DEFAULT_BRIGHTNESS 25            // 0-255 (~10%)
#define MAX_BRIGHTNESS     80            // Hard cap — no dedicated PSU, protect ESP32
#define RANDOM_PLAYBACK    true          // true = shuffle, false = sequential

/* Anti-ghosting / display tuning (from RetroPixelLED reference) */
#define LATCH_BLANKING     1            // 1-4 — higher = less ghosting
#define MIN_REFRESH_HZ     90
#define I2S_CLK_SPEED      HUB75_I2S_CFG::HZ_10M
#define CLK_PHASE          false

/* =================================================================
 *  UI / TEXT / CLOCK CONFIGURATION
 * ================================================================= */
#define CHAR_W                     6
#define CHAR_H                     8
#define SCROLL_STEP_MS            30
#define CLOCK_MIN_DISPLAY_SECONDS 10
#define NTP_RETRY_MS              30000
#define CLOCK_DEFAULT_EVERY       5
#define CLOCK_DEFAULT_TZ          "UTC0"

/* Text notification defaults */
#define NOTIFY_DEFAULT_COLOR_RGB565 0xFFFF
#define NOTIFY_DEFAULT_SIZE         1
#define NOTIFY_DEFAULT_RAINBOW      false
#define NOTIFY_DEFAULT_DURATION     5
#define NOTIFY_QUEUE_LEN            4

/* =================================================================
 *  MQTT CONFIGURATION
 * ================================================================= */
#define MQTT_DEFAULT_PORT    1883
#define MQTT_DEFAULT_CLIENT  "delorean-dmd"
#define MQTT_DEFAULT_TOPIC   "delorean-dmd"
#define MQTT_RETRY_INTERVAL  15000  // ms between reconnect attempts

/* =================================================================
 *  WiFi / Captive Portal CONFIGURATION
 * ================================================================= */
#define AP_SSID                        "DeLorean-DMD"
#define WIFI_CONNECT_TIMEOUT           20000   // ms to wait per saved-credential attempt
#define WIFI_BOOT_CONNECT_MAX_RETRIES  10      // background retries on boot with saved creds
#define WIFI_BOOT_RETRY_INTERVAL       3000    // ms between saved-credential retries
#define WIFI_RETRY_TIMEOUT             15000   // ms to wait when user submits creds
#define WIFI_STABLE_DELAY              3000
