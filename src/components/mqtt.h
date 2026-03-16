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

/* Text notification (set by MQTT or web UI, consumed by main loop) */
struct TextNotification {
    char     text[256];
    uint16_t color;       // packed RGB565
    uint8_t  size;        // font scale 1–3
    bool     rainbow;     // rainbow colour-cycle effect
    uint32_t durationMs;  // display time in ms (0 = scroll once)
    volatile bool pending;
};
extern TextNotification textNotif;

/* Provided by main.cpp */
extern uint8_t MAX_BRIGHTNESS_VAL;  // runtime copy of MAX_BRIGHTNESS
class MatrixPanel_I2S_DMA;
extern MatrixPanel_I2S_DMA *dma_display;

// Parse a JSON or plain-text payload and arm textNotif.
// JSON: {"text":"…","color":"#RRGGBB","size":1,"effect":"rainbow","duration":5000}
// Plain text: entire payload used as message with defaults.
void applyTextNotification(const char *payload);

void applyBrightness(uint8_t val);
void applyPanelOn(bool on);

void mqttPublishState();
void mqttPublishDiscovery();
void mqttConnect();
void loadMqttConfig();
void mqttSetup();
void mqttLoop();
