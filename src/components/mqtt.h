#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>

/* MQTT defaults */
#define MQTT_DEFAULT_PORT    1883
#define MQTT_DEFAULT_CLIENT  "delorean-dmd"
#define MQTT_DEFAULT_TOPIC   "delorean-dmd"
#define MQTT_RETRY_INTERVAL  15000  // ms between reconnect attempts

/* Shared MQTT state */
extern WiFiClient   mqttWifi;
extern PubSubClient mqttClient;
extern String mqttServer, mqttUser, mqttPass, mqttClientId, mqttTopic;
extern uint16_t mqttPort;
extern bool     mqttEnabled;
extern unsigned long mqttLastRetry;

/* Panel state (shared with wifi_portal and main) */
extern bool    panelOn;
extern uint8_t brightness;

/* WiFi stability tracking (set by wifi_portal, read by mqtt) */
extern unsigned long wifiConnectedAt;
#define WIFI_STABLE_DELAY  3000

/* Provided by main.cpp */
extern uint8_t MAX_BRIGHTNESS_VAL;  // runtime copy of MAX_BRIGHTNESS
class MatrixPanel_I2S_DMA;
extern MatrixPanel_I2S_DMA *dma_display;

/* =================================================================
 *  TEXT NOTIFICATION
 * ================================================================= */
#define EFFECT_STATIC   0   // centred static text
#define EFFECT_SCROLL   1   // scrolls right → left
#define EFFECT_BLINK    2   // blinks on/off

// Default display time (ms) when caller does not specify duration
#define DEFAULT_NOTIFY_DURATION_MS  8000

struct TextNotification {
    char     text[256];
    uint16_t color;      // RGB565
    uint8_t  size;       // Adafruit GFX text scale: 1, 2, or 3
    uint8_t  effect;     // EFFECT_STATIC / EFFECT_SCROLL / EFFECT_BLINK
    uint32_t durationMs; // 0 = use DEFAULT_NOTIFY_DURATION_MS
    volatile bool active;
};

extern TextNotification textNotif;

// Parse a plain-text or JSON payload and activate the notification.
// JSON format: {"text":"…","color":[r,g,b],"size":1,"effect":"scroll","duration":8000}
// Plain text : the entire payload is used as the message with default settings.
void applyTextNotification(const char *payload);

void applyBrightness(uint8_t val);
void applyPanelOn(bool on);

void mqttPublishState();
void mqttPublishDiscovery();
void mqttConnect();
void loadMqttConfig();
void mqttSetup();
void mqttLoop();
