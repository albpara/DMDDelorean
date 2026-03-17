#pragma once

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include "app_config.h"

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

/* Clock mode state/config (shared with main and wifi_portal) */
extern bool     clockModeEnabled;
extern uint16_t clockEveryNGifs;
extern char     clockTz[64];
extern volatile bool clockConfigDirty;
extern volatile bool clockTimeValid;

/* WiFi stability tracking (set by wifi_portal, read by mqtt) */
extern unsigned long wifiConnectedAt;

/* Text notification (set by MQTT or web UI, consumed by main loop) */
struct TextNotification {
    char     text[256];
    uint16_t color;       // packed RGB565
    uint8_t  size;        // font scale 1–3
    bool     rainbow;     // rainbow colour-cycle effect
    uint32_t duration;    // wide text: loop count, short text: seconds
    volatile bool pending;
};
extern TextNotification textNotif;

/* Provided by main.cpp */
extern uint8_t MAX_BRIGHTNESS_VAL;  // runtime copy of MAX_BRIGHTNESS
class MatrixPanel_I2S_DMA;
extern MatrixPanel_I2S_DMA *dma_display;

// Parse a JSON or plain-text payload and enqueue it for FIFO display.
// JSON: {"text":"...","color":"#RRGGBB","size":1,"effect":"rainbow","duration":5}
// Plain text: entire payload used as message with defaults.
bool hasPendingTextNotification();
bool takePendingTextNotification(TextNotification *out);
void applyTextNotification(const char *payload);
void applyClockConfigPayload(const char *payload);
void updateClockConfig(bool enabled, uint16_t every, const char *tz, bool persist);
void loadClockConfig();

void applyBrightness(uint8_t val);
void applyPanelOn(bool on);

void mqttPublishState();
void mqttPublishDiscovery();
void mqttConnect();
void loadMqttConfig();
void mqttSetup();
void mqttLoop();
