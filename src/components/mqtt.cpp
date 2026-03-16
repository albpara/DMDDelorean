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

// Clock mode state
bool     clockModeEnabled = false;
uint16_t clockEveryNGifs  = 5;
char     clockTz[64]      = "UTC0";
volatile bool clockConfigDirty = true;
volatile bool clockTimeValid   = false;

// Text notification state — filled by MQTT or web UI, consumed by loop()
TextNotification textNotif = {"", 0xFFFF, 1, false, 5, false};

static void mqttPublishClockState() {
    if (!mqttClient.connected()) return;
    String topic = mqttTopic + "/clock/state";
    String payload = "{\"enabled\":";
    payload += clockModeEnabled ? "true" : "false";
    payload += ",\"every\":";
    payload += clockEveryNGifs;
    payload += ",\"tz\":\"";
    String tz = String(clockTz);
    tz.replace("\"", "\\\"");
    payload += tz;
    payload += "\"}";
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

/* =================================================================
 *  CLOCK CONFIG HELPERS
 * ================================================================= */
static void persistClockConfig() {
    Preferences prefs;
    prefs.begin("clock", false);
    prefs.putBool("enabled", clockModeEnabled);
    prefs.putUShort("every", clockEveryNGifs);
    prefs.putString("tz", String(clockTz));
    prefs.end();
}

void updateClockConfig(bool enabled, uint16_t every, const char *tz, bool persist) {
    clockModeEnabled = enabled;
    clockEveryNGifs  = (every == 0) ? 1 : every;
    if (tz && tz[0] != '\0') {
        strncpy(clockTz, tz, sizeof(clockTz) - 1);
        clockTz[sizeof(clockTz) - 1] = '\0';
    }
    clockConfigDirty = true;
    if (persist) persistClockConfig();
    mqttPublishClockState();
    Serial.printf("[CLOCK] enabled=%d every=%u tz=%s\n",
                  (int)clockModeEnabled, clockEveryNGifs, clockTz);
}

void loadClockConfig() {
    Preferences prefs;
    prefs.begin("clock", true);
    bool en = prefs.getBool("enabled", false);
    uint16_t every = prefs.getUShort("every", 5);
    String tz = prefs.getString("tz", "UTC0");
    prefs.end();
    updateClockConfig(en, every, tz.c_str(), false);
}

void applyClockConfigPayload(const char *payload) {
    String s(payload);
    s.trim();
    if (s.length() == 0) return;

    bool en = clockModeEnabled;
    uint16_t every = clockEveryNGifs;
    char tz[64];
    strncpy(tz, clockTz, sizeof(tz) - 1);
    tz[sizeof(tz) - 1] = '\0';

    if (s.startsWith("{")) {
        int ei = s.indexOf("\"enabled\":");
        if (ei >= 0) {
            String tail = s.substring(ei + 10);
            en = tail.startsWith("true") || tail.startsWith("1");
        }

        int ni = s.indexOf("\"every\":");
        if (ni >= 0) {
            int v = s.substring(ni + 8).toInt();
            if (v > 0 && v < 65535) every = (uint16_t)v;
        }

        int ti = s.indexOf("\"tz\":");
        if (ti >= 0) {
            int q1 = s.indexOf('"', ti + 5);
            if (q1 >= 0) {
                int q2 = s.indexOf('"', q1 + 1);
                if (q2 > q1) {
                    String tzStr = s.substring(q1 + 1, q2);
                    tzStr.toCharArray(tz, sizeof(tz));
                }
            }
        }
    } else {
        String u = s;
        u.toUpperCase();
        if (u == "ON" || u == "1" || u == "TRUE") en = true;
        else if (u == "OFF" || u == "0" || u == "FALSE") en = false;
    }

    updateClockConfig(en, every, tz, true);
}

/* =================================================================
 *  TEXT NOTIFICATION PARSER
 *  Accepts JSON or plain-text payload, arms textNotif for loop().
 *  JSON: {"text":"…","color":"#RRGGBB","size":1,"effect":"rainbow","duration":5}
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
    textNotif.duration   = 5;
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

        // duration: loops for scrolling text, seconds for non-scrolling text
        int di = s.indexOf("\"duration\":");
        if (di >= 0) textNotif.duration = (uint32_t)s.substring(di + 11).toInt();
    } else {
        // --- Plain-text payload ---
        s.toCharArray(textNotif.text, sizeof(textNotif.text));
    }

    if (textNotif.text[0] != '\0') {
        textNotif.pending = true;
        Serial.printf("[NOTIFY] text='%s' size=%d rainbow=%d dur=%u\n",
                      textNotif.text, textNotif.size,
                      (int)textNotif.rainbow, textNotif.duration);
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
    String lightDiscTopic = "homeassistant/light/" + mqttClientId + "/config";
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
    bool ok = mqttClient.publish(lightDiscTopic.c_str(), payload.c_str(), true);
    Serial.printf("[MQTT] Discovery %s (%u bytes to %s)\n",
                  ok ? "sent" : "FAILED", payload.length(), lightDiscTopic.c_str());

    String clockSwitchDiscTopic = "homeassistant/switch/" + mqttClientId + "_clock/config";
    String swPayload = "{\"name\":\"DMD Clock Mode\""
        ",\"unique_id\":\"" + mqttClientId + "_clock\""
        ",\"object_id\":\"" + mqttClientId + "_clock\""
        ",\"command_topic\":\"" + mqttTopic + "/clock/set\""
        ",\"state_topic\":\"" + mqttTopic + "/clock/state\""
        ",\"payload_on\":\"ON\""
        ",\"payload_off\":\"OFF\""
        ",\"state_on\":\"ON\""
        ",\"state_off\":\"OFF\""
        ",\"value_template\":\"{{ 'ON' if value_json.enabled else 'OFF' }}\""
        ",\"availability_topic\":\"" + mqttTopic + "/available\""
        ",\"payload_available\":\"online\""
        ",\"payload_not_available\":\"offline\""
        ",\"device\":{"
            "\"identifiers\":[\"" + mqttClientId + "\"]"
            ",\"name\":\"DeLorean DMD\""
            ",\"model\":\"128x32 HUB75 LED Matrix\""
            ",\"manufacturer\":\"DeLorean DMD\""
            ",\"configuration_url\":\"http://" + ip + "/\""
        "}"
        "}";
    mqttClient.publish(clockSwitchDiscTopic.c_str(), swPayload.c_str(), true);

    String clockNumDiscTopic = "homeassistant/number/" + mqttClientId + "_clock_every/config";
    String numPayload = "{\"name\":\"DMD Clock Every N GIFs\""
        ",\"unique_id\":\"" + mqttClientId + "_clock_every\""
        ",\"object_id\":\"" + mqttClientId + "_clock_every\""
        ",\"command_topic\":\"" + mqttTopic + "/clock/every/set\""
        ",\"state_topic\":\"" + mqttTopic + "/clock/state\""
        ",\"value_template\":\"{{ value_json.every }}\""
        ",\"min\":1,\"max\":1000,\"step\":1,\"mode\":\"box\""
        ",\"availability_topic\":\"" + mqttTopic + "/available\""
        ",\"payload_available\":\"online\""
        ",\"payload_not_available\":\"offline\""
        ",\"device\":{"
            "\"identifiers\":[\"" + mqttClientId + "\"]"
            ",\"name\":\"DeLorean DMD\""
            ",\"model\":\"128x32 HUB75 LED Matrix\""
            ",\"manufacturer\":\"DeLorean DMD\""
            ",\"configuration_url\":\"http://" + ip + "/\""
        "}"
        "}";
    mqttClient.publish(clockNumDiscTopic.c_str(), numPayload.c_str(), true);

    String rebootBtnDiscTopic = "homeassistant/button/" + mqttClientId + "_reboot/config";
    String rebootPayload = "{\"name\":\"DMD Reboot\""
        ",\"unique_id\":\"" + mqttClientId + "_reboot\""
        ",\"object_id\":\"" + mqttClientId + "_reboot\""
        ",\"command_topic\":\"" + mqttTopic + "/reboot/set\""
        ",\"payload_press\":\"PRESS\""
        ",\"entity_category\":\"diagnostic\""
        ",\"device_class\":\"restart\""
        ",\"availability_topic\":\"" + mqttTopic + "/available\""
        ",\"payload_available\":\"online\""
        ",\"payload_not_available\":\"offline\""
        ",\"device\":{"
            "\"identifiers\":[\"" + mqttClientId + "\"]"
            ",\"name\":\"DeLorean DMD\""
            ",\"model\":\"128x32 HUB75 LED Matrix\""
            ",\"manufacturer\":\"DeLorean DMD\""
            ",\"configuration_url\":\"http://" + ip + "/\""
        "}"
        "}";
    mqttClient.publish(rebootBtnDiscTopic.c_str(), rebootPayload.c_str(), true);
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
    String clockTopic  = mqttTopic + "/clock/set";
    String clockEveryTopic = mqttTopic + "/clock/every/set";
    String rebootTopic = mqttTopic + "/reboot/set";

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
    } else if (t == clockTopic) {
        applyClockConfigPayload(msg);
    } else if (t == clockEveryTopic) {
        int v = String(msg).toInt();
        if (v > 0 && v < 65535) updateClockConfig(clockModeEnabled, (uint16_t)v, clockTz, true);
    } else if (t == rebootTopic) {
        String s(msg);
        s.trim();
        s.toUpperCase();
        if (s.length() == 0 || s == "PRESS" || s == "REBOOT" || s == "ON" || s == "1") {
            Serial.println("[MQTT] Reboot command received");
            String availTopic = mqttTopic + "/available";
            mqttClient.publish(availTopic.c_str(), "offline", true);
            mqttClient.loop();
            delay(100);
            ESP.restart();
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
        String clockTopic  = mqttTopic + "/clock/set";
        String clockEveryTopic = mqttTopic + "/clock/every/set";
        String rebootTopic = mqttTopic + "/reboot/set";
        mqttClient.subscribe(cmdTopic.c_str());
        mqttClient.subscribe(brTopic.c_str());
        mqttClient.subscribe(notifyTopic.c_str());
        mqttClient.subscribe(clockTopic.c_str());
        mqttClient.subscribe(clockEveryTopic.c_str());
        mqttClient.subscribe(rebootTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", notifyTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", clockTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", clockEveryTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", rebootTopic.c_str());
        mqttPublishDiscovery();
        mqttPublishState();
        mqttPublishClockState();
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
    loadClockConfig();
}

void mqttLoop() {
    if (!mqttEnabled) return;
    if (!mqttClient.connected()) mqttConnect();
    mqttClient.loop();
}
