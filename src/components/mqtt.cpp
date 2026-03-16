#include "mqtt.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

/* =================================================================
 *  MQTT globals
 * ================================================================= */
WiFiClient   mqttWifi;
PubSubClient mqttClient(mqttWifi);
String mqttServer, mqttUser, mqttPass, mqttClientId, mqttTopic;
uint16_t mqttPort      = MQTT_DEFAULT_PORT;
bool     mqttEnabled   = false;
unsigned long mqttLastRetry = 0;

// Panel state
bool    panelOn    = true;
uint8_t brightness = 25;   // overwritten from main's DEFAULT_BRIGHTNESS

// Text notification state
TextNotification textNotif = {"", 0xFFFF, 1, EFFECT_SCROLL, 0, false};

/* =================================================================
 *  TEXT NOTIFICATION HELPERS
 * ================================================================= */
static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
}

// Extract a JSON string value: "key":"value"
static bool jsonGetString(const String &json, const char *key, char *out, size_t maxLen) {
    String search = String("\"") + key + "\":\"";
    int idx = json.indexOf(search);
    if (idx < 0) return false;
    idx += search.length();
    int end = json.indexOf('"', idx);
    if (end < 0) return false;
    String val = json.substring(idx, end);
    strncpy(out, val.c_str(), maxLen - 1);
    out[maxLen - 1] = '\0';
    return true;
}

// Extract a JSON integer value: "key":number
static bool jsonGetInt(const String &json, const char *key, int32_t *out) {
    String search = String("\"") + key + "\":";
    int idx = json.indexOf(search);
    if (idx < 0) return false;
    idx += search.length();
    while (idx < (int)json.length() && json[idx] == ' ') idx++;
    *out = (int32_t)json.substring(idx).toInt();
    return true;
}

// Extract a JSON color array: "color":[r,g,b]
static bool jsonGetColorArray(const String &json, uint8_t *r, uint8_t *g, uint8_t *b) {
    String search = "\"color\":[";
    int idx = json.indexOf(search);
    if (idx < 0) return false;
    idx += search.length();
    int end = json.indexOf(']', idx);
    if (end < 0) return false;
    String arr = json.substring(idx, end);
    int c1 = arr.indexOf(',');
    int c2 = arr.indexOf(',', c1 + 1);
    if (c1 < 0 || c2 < 0) return false;
    *r = (uint8_t)arr.substring(0, c1).toInt();
    *g = (uint8_t)arr.substring(c1 + 1, c2).toInt();
    *b = (uint8_t)arr.substring(c2 + 1).toInt();
    return true;
}

/* =================================================================
 *  APPLY TEXT NOTIFICATION
 *  Parses a plain-text or JSON payload and arms textNotif.
 *  JSON: {"text":"…","color":[r,g,b],"size":1,"effect":"scroll","duration":8000}
 *  Plain text: entire payload used as message with defaults.
 * ================================================================= */
void applyTextNotification(const char *payload) {
    String s(payload);
    s.trim();
    if (s.length() == 0) return;

    // Reset to safe defaults
    textNotif.color     = 0xFFFF;   // white
    textNotif.size      = 1;
    textNotif.effect    = EFFECT_SCROLL;
    textNotif.durationMs = 0;

    if (s.startsWith("{")) {
        // --- JSON payload ---
        char txt[256] = "";
        jsonGetString(s, "text", txt, sizeof(txt));
        strncpy(textNotif.text, txt, sizeof(textNotif.text) - 1);
        textNotif.text[sizeof(textNotif.text) - 1] = '\0';

        uint8_t r = 255, g = 255, b = 255;
        if (jsonGetColorArray(s, &r, &g, &b))
            textNotif.color = rgb565(r, g, b);

        int32_t sz = 1;
        if (jsonGetInt(s, "size", &sz)) {
            if (sz < 1) sz = 1;
            if (sz > 3) sz = 3;
            textNotif.size = (uint8_t)sz;
        }

        char eff[16] = "scroll";
        jsonGetString(s, "effect", eff, sizeof(eff));
        if (strcmp(eff, "static") == 0)     textNotif.effect = EFFECT_STATIC;
        else if (strcmp(eff, "blink") == 0) textNotif.effect = EFFECT_BLINK;
        else                                textNotif.effect = EFFECT_SCROLL;

        int32_t dur = 0;
        if (jsonGetInt(s, "duration", &dur) && dur > 0)
            textNotif.durationMs = (uint32_t)dur;
    } else {
        // --- Plain-text payload ---
        strncpy(textNotif.text, s.c_str(), sizeof(textNotif.text) - 1);
        textNotif.text[sizeof(textNotif.text) - 1] = '\0';
    }

    if (strlen(textNotif.text) > 0) {
        textNotif.active = true;
        Serial.printf("[NOTIFY] text='%s' size=%d effect=%d dur=%u\n",
                      textNotif.text, textNotif.size, textNotif.effect, textNotif.durationMs);
    }
}

/* =================================================================
 *  BRIGHTNESS / POWER HELPERS
 * ================================================================= */
void applyBrightness(uint8_t val) {
    brightness = val;
    uint8_t capped = (val > MAX_BRIGHTNESS_VAL) ? MAX_BRIGHTNESS_VAL : val;
    if (dma_display) dma_display->setBrightness8(panelOn ? capped : 0);
}

void applyPanelOn(bool on) {
    panelOn = on;
    if (dma_display) {
        if (!on) {
            dma_display->setBrightness8(0);
        } else {
            uint8_t capped = (brightness > MAX_BRIGHTNESS_VAL) ? MAX_BRIGHTNESS_VAL : brightness;
            dma_display->setBrightness8(capped);
        }
    }
}

/* =================================================================
 *  PUBLISH STATE
 * ================================================================= */
void mqttPublishState() {
    if (!mqttClient.connected()) return;
    String stateTopic = mqttTopic + "/state";
    String payload = "{\"state\":\"";
    payload += panelOn ? "ON" : "OFF";
    payload += "\",\"brightness\":";
    payload += brightness;
    payload += "}";
    mqttClient.publish(stateTopic.c_str(), payload.c_str(), true);
}

/* =================================================================
 *  PUBLISH HA DISCOVERY — includes device block + configuration_url
 * ================================================================= */
void mqttPublishDiscovery() {
    if (!mqttClient.connected()) return;

    String ip = WiFi.localIP().toString();
    String discTopic = "homeassistant/light/" + mqttClientId + "/config";
    String payload = "{\"name\":\"DeLorean DMD\""
        ",\"unique_id\":\"" + mqttClientId + "\""
        ",\"object_id\":\"" + mqttClientId + "\""
        ",\"schema\":\"json\""
        ",\"command_topic\":\"" + mqttTopic + "/set\""
        ",\"state_topic\":\"" + mqttTopic + "/state\""
        ",\"brightness\":true"
        ",\"brightness_scale\":" + String(MAX_BRIGHTNESS_VAL) +
        ",\"device\":{"
            "\"identifiers\":[\"" + mqttClientId + "\"]"
            ",\"name\":\"DeLorean DMD\""
            ",\"model\":\"128x32 HUB75 LED Matrix\""
            ",\"manufacturer\":\"DeLorean DMD\""
            ",\"configuration_url\":\"http://" + ip + "/\""
        "}"
        ",\"availability_topic\":\"" + mqttTopic + "/available\""
        ",\"payload_available\":\"online\""
        ",\"payload_not_available\":\"offline\""
        "}";
    bool ok = mqttClient.publish(discTopic.c_str(), payload.c_str(), true);
    Serial.printf("[MQTT] Discovery %s (%u bytes to %s)\n",
                  ok ? "sent" : "FAILED", payload.length(), discTopic.c_str());
}

/* =================================================================
 *  CALLBACK — handles set commands from Home Assistant
 * ================================================================= */
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    char msg[length + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';

    String t(topic);
    String cmdTopic    = mqttTopic + "/set";
    String brTopic     = mqttTopic + "/brightness/set";
    String notifyTopic = mqttTopic + "/notify";

    if (t == notifyTopic) {
        applyTextNotification(msg);
    } else if (t == cmdTopic) {
        String s(msg);
        if (s.indexOf("\"ON\"") >= 0) applyPanelOn(true);
        else if (s.indexOf("\"OFF\"") >= 0) applyPanelOn(false);

        int bi = s.indexOf("\"brightness\":");
        if (bi >= 0) {
            int val = s.substring(bi + 13).toInt();
            if (val >= 0 && val <= 255) applyBrightness((uint8_t)val);
        }
        mqttPublishState();
    } else if (t == brTopic) {
        int val = String(msg).toInt();
        if (val >= 0 && val <= 255) {
            applyBrightness((uint8_t)val);
            mqttPublishState();
        }
    }
}

/* =================================================================
 *  CONNECT — with WiFi stability gate and LWT
 * ================================================================= */
void mqttConnect() {
    if (!mqttEnabled || mqttServer.length() == 0) return;
    if (WiFi.status() != WL_CONNECTED) return;
    if (mqttClient.connected()) return;

    if (wifiConnectedAt == 0) return;
    if (millis() - wifiConnectedAt < WIFI_STABLE_DELAY) return;

    if (millis() - mqttLastRetry < MQTT_RETRY_INTERVAL) return;
    mqttLastRetry = millis();

    Serial.printf("[MQTT] Connecting to %s:%d as '%s'...\n",
                  mqttServer.c_str(), mqttPort, mqttClientId.c_str());

    mqttClient.setServer(mqttServer.c_str(), mqttPort);
    mqttClient.setBufferSize(1024);
    mqttClient.setCallback(mqttCallback);

    String availTopic = mqttTopic + "/available";
    bool ok;
    if (mqttUser.length() > 0)
        ok = mqttClient.connect(mqttClientId.c_str(), mqttUser.c_str(), mqttPass.c_str(),
                                availTopic.c_str(), 0, true, "offline");
    else
        ok = mqttClient.connect(mqttClientId.c_str(), nullptr, nullptr,
                                availTopic.c_str(), 0, true, "offline");

    if (ok) {
        Serial.println("[MQTT] Connected!");
        mqttClient.publish(availTopic.c_str(), "online", true);
        String cmdTopic    = mqttTopic + "/set";
        String brTopic     = mqttTopic + "/brightness/set";
        String notifyTopic = mqttTopic + "/notify";
        mqttClient.subscribe(cmdTopic.c_str());
        mqttClient.subscribe(brTopic.c_str());
        mqttClient.subscribe(notifyTopic.c_str());
        mqttPublishDiscovery();
        mqttPublishState();
    } else {
        Serial.printf("[MQTT] Failed, rc=%d\n", mqttClient.state());
    }
}

/* =================================================================
 *  LOAD CONFIG from NVS
 * ================================================================= */
void loadMqttConfig() {
    Preferences prefs;
    prefs.begin("mqtt", true);
    mqttServer   = prefs.getString("server", "");
    mqttPort     = prefs.getUShort("port", MQTT_DEFAULT_PORT);
    mqttUser     = prefs.getString("user", "");
    mqttPass     = prefs.getString("pass", "");
    mqttClientId = prefs.getString("client", MQTT_DEFAULT_CLIENT);
    mqttTopic    = prefs.getString("topic", MQTT_DEFAULT_TOPIC);
    prefs.end();
    mqttEnabled = (mqttServer.length() > 0);
    if (mqttEnabled)
        Serial.printf("[MQTT] Config loaded: %s:%d topic=%s\n",
                      mqttServer.c_str(), mqttPort, mqttTopic.c_str());
}

/* =================================================================
 *  SETUP & LOOP helpers
 * ================================================================= */
void mqttSetup() {
    loadMqttConfig();
}

void mqttLoop() {
    if (!mqttEnabled) return;
    if (!mqttClient.connected()) mqttConnect();
    mqttClient.loop();
}
