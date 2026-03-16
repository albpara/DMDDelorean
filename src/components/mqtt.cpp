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

// Text notification state — filled by MQTT or web UI, consumed by loop()
TextNotification textNotif = {"", 0xFFFF, 1, false, 5000, false};

/* =================================================================
 *  TEXT NOTIFICATION PARSER
 *  Accepts JSON or plain-text payload, arms textNotif for loop().
 *  JSON: {"text":"…","color":"#RRGGBB","size":1,"effect":"rainbow","duration":5000}
 *  Plain text: entire payload used as message with defaults.
 * ================================================================= */
void applyTextNotification(const char *payload) {
    String s(payload);
    s.trim();
    if (s.length() == 0) return;

    // Reset to defaults
    textNotif.color      = 0xFFFF;
    textNotif.size       = 1;
    textNotif.rainbow    = false;
    textNotif.durationMs = 5000;
    textNotif.text[0]    = '\0';

    if (s.startsWith("{")) {
        // --- JSON payload ---
        // text
        int ti = s.indexOf("\"text\":");
        if (ti >= 0) {
            int tq1 = s.indexOf('"', ti + 7);
            if (tq1 >= 0) {
                int tq2 = s.indexOf('"', tq1 + 1);
                if (tq2 > tq1)
                    s.substring(tq1 + 1, tq2).toCharArray(textNotif.text, sizeof(textNotif.text));
            }
        }

        // color — "#RRGGBB"
        int ci = s.indexOf("\"color\":\"#");
        if (ci >= 0 && (ci + 16) <= (int)s.length()) {
            char hex[7];
            s.substring(ci + 10, ci + 16).toCharArray(hex, sizeof(hex));
            long v = strtol(hex, nullptr, 16);
            uint8_t r = (uint8_t)((v >> 16) & 0xFF);
            uint8_t g = (uint8_t)((v >> 8)  & 0xFF);
            uint8_t b = (uint8_t)( v        & 0xFF);
            if (dma_display) textNotif.color = dma_display->color565(r, g, b);
        }

        // size (1–3)
        int si = s.indexOf("\"size\":");
        if (si >= 0) {
            int sv = s.substring(si + 7).toInt();
            if (sv >= 1 && sv <= 3) textNotif.size = (uint8_t)sv;
        }

        // effect
        if (s.indexOf("\"effect\":\"rainbow\"") >= 0) textNotif.rainbow = true;

        // duration (ms)
        int di = s.indexOf("\"duration\":");
        if (di >= 0) textNotif.durationMs = (uint32_t)s.substring(di + 11).toInt();
    } else {
        // --- Plain-text payload ---
        s.toCharArray(textNotif.text, sizeof(textNotif.text));
    }

    if (textNotif.text[0] != '\0') {
        textNotif.pending = true;
        Serial.printf("[NOTIFY] text='%s' size=%d rainbow=%d dur=%u\n",
                      textNotif.text, textNotif.size,
                      (int)textNotif.rainbow, textNotif.durationMs);
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
 *  CALLBACK — handles set commands from Home Assistant / raw MQTT
 * ================================================================= */
static void mqttCallback(char *topic, byte *payload, unsigned int length) {
    char msg[length + 1];
    memcpy(msg, payload, length);
    msg[length] = '\0';

    String t(topic);
    String cmdTopic    = mqttTopic + "/set";
    String brTopic     = mqttTopic + "/brightness/set";
    String notifyTopic = mqttTopic + "/notify";

    if (t == cmdTopic) {
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
    } else if (t == notifyTopic) {
        // Delegate to shared parser: JSON or plain-text payload
        applyTextNotification(msg);
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
        Serial.printf("[MQTT] Subscribed to %s\n", notifyTopic.c_str());
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
