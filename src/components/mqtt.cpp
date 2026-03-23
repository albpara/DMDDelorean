#include "mqtt.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

/* =================================================================
 *  MQTT globals
 * ================================================================= */
WiFiClient   mqttWifi;
PubSubClient mqttClient(mqttWifi);
String mqttServer, mqttUser, mqttPass, mqttClientId, mqttTopic;
uint16_t mqttPort      = 0;
bool     mqttEnabled   = false;
unsigned long mqttLastRetry = 0;

// Panel state
bool    panelOn       = false;
uint8_t brightness    = 0;
bool    safeBrightness = true;

// Clock mode state
bool     clockModeEnabled = false;
uint16_t clockEveryNGifs  = 0;
char     clockTz[64]      = "";
volatile bool clockConfigDirty = false;
volatile bool clockTimeValid   = false;

// Dashboard mode state
bool     dashboardModeEnabled = false;
uint16_t dashboardDwellSeconds = DASHBOARD_DEFAULT_DWELL_SECONDS;
char     dashboardProfile[16] = "";

// Text notification state — filled by MQTT or web UI, consumed by loop()
TextNotification textNotif = {"", 0, 0, false, 0, false};
static portMUX_TYPE textNotifMux = portMUX_INITIALIZER_UNLOCKED;
static TextNotification textNotifQueue[NOTIFY_QUEUE_LEN];
static uint8_t textNotifHead = 0;
static uint8_t textNotifTail = 0;
static uint8_t textNotifCount = 0;
static TextNotification dashboardCards[DASHBOARD_MAX_ITEMS];
static uint8_t dashboardCardCount = 0;
static uint8_t dashboardCardCursor = 0;
static portMUX_TYPE dashboardMux = portMUX_INITIALIZER_UNLOCKED;

static void resetTextNotification(TextNotification &notif) {
    notif.text[0] = '\0';
    notif.color = NOTIFY_DEFAULT_COLOR_RGB565;
    notif.size = NOTIFY_DEFAULT_SIZE;
    notif.rainbow = NOTIFY_DEFAULT_RAINBOW;
    notif.duration = NOTIFY_DEFAULT_DURATION;
    notif.pending = false;
}

static void resetTextNotificationQueue() {
    textNotifHead = 0;
    textNotifTail = 0;
    textNotifCount = 0;
    for (uint8_t i = 0; i < NOTIFY_QUEUE_LEN; i++) {
        resetTextNotification(textNotifQueue[i]);
    }
}

static void resetDashboardCards() {
    dashboardCardCount = 0;
    dashboardCardCursor = 0;
    for (uint8_t i = 0; i < DASHBOARD_MAX_ITEMS; i++) {
        resetTextNotification(dashboardCards[i]);
    }
}

static bool jsonExtractQuoted(const String &obj, const char *key, String *out) {
    if (!out || !key) return false;
    String needle = String("\"") + key + "\"";
    int ki = obj.indexOf(needle);
    if (ki < 0) return false;
    int colon = obj.indexOf(':', ki + needle.length());
    if (colon < 0) return false;
    int q1 = obj.indexOf('"', colon + 1);
    if (q1 < 0) return false;
    int q2 = obj.indexOf('"', q1 + 1);
    if (q2 <= q1) return false;
    *out = obj.substring(q1 + 1, q2);
    return true;
}

static bool jsonExtractInt(const String &obj, const char *key, int *out) {
    if (!out || !key) return false;
    String needle = String("\"") + key + "\"";
    int ki = obj.indexOf(needle);
    if (ki < 0) return false;
    int colon = obj.indexOf(':', ki + needle.length());
    if (colon < 0) return false;
    *out = obj.substring(colon + 1).toInt();
    return true;
}

static bool jsonExtractBool(const String &obj, const char *key, bool *out) {
    if (!out || !key) return false;
    String needle = String("\"") + key + "\"";
    int ki = obj.indexOf(needle);
    if (ki < 0) return false;
    int colon = obj.indexOf(':', ki + needle.length());
    if (colon < 0) return false;
    String tail = obj.substring(colon + 1);
    tail.trim();
    if (tail.startsWith("true") || tail.startsWith("1")) {
        *out = true;
        return true;
    }
    if (tail.startsWith("false") || tail.startsWith("0")) {
        *out = false;
        return true;
    }
    return false;
}

static void mqttPublishDashboardState() {
    if (!mqttClient.connected()) return;
    String topic = mqttTopic + "/dashboard/state";
    String payload = "{\"enabled\":";
    payload += dashboardModeEnabled ? "true" : "false";
    payload += ",\"dwell\":";
    payload += dashboardDwellSeconds;
    payload += ",\"profile\":\"";
    String profile = String(dashboardProfile);
    profile.replace("\"", "\\\"");
    payload += profile;
    payload += "\",\"count\":";
    payload += dashboardCardCount;
    payload += "}";
    mqttClient.publish(topic.c_str(), payload.c_str(), true);
}

static void initRuntimeDefaults() {
    mqttServer = "";
    mqttUser = "";
    mqttPass = "";
    mqttClientId = MQTT_DEFAULT_CLIENT;
    mqttTopic = MQTT_DEFAULT_TOPIC;
    mqttPort = MQTT_DEFAULT_PORT;
    mqttEnabled = false;
    mqttLastRetry = 0;

    panelOn = true;
    brightness = DEFAULT_BRIGHTNESS;
    safeBrightness = DEFAULT_SAFE_BRIGHTNESS;

    clockModeEnabled = false;
    clockEveryNGifs = CLOCK_DEFAULT_EVERY;
    strncpy(clockTz, CLOCK_DEFAULT_TZ, sizeof(clockTz) - 1);
    clockTz[sizeof(clockTz) - 1] = '\0';
    clockConfigDirty = true;
    clockTimeValid = false;

    dashboardModeEnabled = DASHBOARD_DEFAULT_ENABLED;
    dashboardDwellSeconds = DASHBOARD_DEFAULT_DWELL_SECONDS;
    strncpy(dashboardProfile, DASHBOARD_DEFAULT_PROFILE, sizeof(dashboardProfile) - 1);
    dashboardProfile[sizeof(dashboardProfile) - 1] = '\0';

    resetTextNotification(textNotif);
    resetTextNotificationQueue();
    resetDashboardCards();
}

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
    uint16_t every = prefs.getUShort("every", CLOCK_DEFAULT_EVERY);
    String tz = prefs.getString("tz", CLOCK_DEFAULT_TZ);
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
 *  DASHBOARD CONFIG HELPERS
 * ================================================================= */
static void persistDashboardConfig() {
    Preferences prefs;
    prefs.begin("dash", false);
    prefs.putBool("enabled", dashboardModeEnabled);
    prefs.putUShort("dwell", dashboardDwellSeconds);
    prefs.putString("profile", String(dashboardProfile));
    prefs.end();
}

void loadDashboardConfig() {
    Preferences prefs;
    prefs.begin("dash", true);
    dashboardModeEnabled = prefs.getBool("enabled", DASHBOARD_DEFAULT_ENABLED);
    dashboardDwellSeconds = prefs.getUShort("dwell", DASHBOARD_DEFAULT_DWELL_SECONDS);
    String profile = prefs.getString("profile", DASHBOARD_DEFAULT_PROFILE);
    prefs.end();
    profile.toCharArray(dashboardProfile, sizeof(dashboardProfile));
    if (dashboardDwellSeconds == 0) dashboardDwellSeconds = 1;
}

void applyDashboardModePayload(const char *payload) {
    if (!payload) return;
    String s(payload);
    s.trim();
    s.toUpperCase();
    bool enabled = dashboardModeEnabled;
    if (s == "ON" || s == "1" || s == "TRUE") enabled = true;
    else if (s == "OFF" || s == "0" || s == "FALSE") enabled = false;
    dashboardModeEnabled = enabled;
    persistDashboardConfig();
    mqttPublishDashboardState();
}

void applyDashboardDwellPayload(const char *payload) {
    if (!payload) return;
    int v = String(payload).toInt();
    if (v < 1) v = 1;
    if (v > 120) v = 120;
    dashboardDwellSeconds = (uint16_t)v;
    persistDashboardConfig();
    mqttPublishDashboardState();
}

void applyDashboardProfilePayload(const char *payload) {
    if (!payload) return;
    String s(payload);
    s.trim();
    if (s.length() == 0) return;
    s.toCharArray(dashboardProfile, sizeof(dashboardProfile));
    persistDashboardConfig();
    mqttPublishDashboardState();
}

/* =================================================================
 *  TEXT NOTIFICATION PARSER
 *  Accepts JSON or plain-text payload, arms textNotif for loop().
 *  JSON: {"text":"…","color":"#RRGGBB","size":1,"effect":"rainbow","duration":5}
 *  Plain text: entire payload used as message with defaults.
 * ================================================================= */
bool hasPendingTextNotification() {
    bool pending;
    taskENTER_CRITICAL(&textNotifMux);
    pending = (textNotifCount > 0);
    taskEXIT_CRITICAL(&textNotifMux);
    return pending;
}

bool takePendingTextNotification(TextNotification *out) {
    if (!out) return false;

    bool havePending = false;
    taskENTER_CRITICAL(&textNotifMux);
    if (textNotifCount > 0) {
        const TextNotification &queued = textNotifQueue[textNotifHead];
        memcpy(out->text, queued.text, sizeof(queued.text));
        out->color = queued.color;
        out->size = queued.size;
        out->rainbow = queued.rainbow;
        out->duration = queued.duration;
        out->pending = false;
        resetTextNotification(textNotifQueue[textNotifHead]);
        textNotifHead = (uint8_t)((textNotifHead + 1) % NOTIFY_QUEUE_LEN);
        textNotifCount--;
        havePending = true;
    }
    taskEXIT_CRITICAL(&textNotifMux);

    return havePending;
}

bool hasDashboardCards() {
    bool available;
    taskENTER_CRITICAL(&dashboardMux);
    available = (dashboardCardCount > 0);
    taskEXIT_CRITICAL(&dashboardMux);
    return available;
}

bool takeNextDashboardCard(TextNotification *out) {
    if (!out) return false;
    bool available = false;
    taskENTER_CRITICAL(&dashboardMux);
    if (dashboardCardCount > 0) {
        uint8_t idx = dashboardCardCursor;
        const TextNotification &card = dashboardCards[idx];
        memcpy(out->text, card.text, sizeof(card.text));
        out->color = card.color;
        out->size = card.size;
        out->rainbow = card.rainbow;
        out->duration = card.duration;
        out->pending = false;
        dashboardCardCursor = (uint8_t)((dashboardCardCursor + 1) % dashboardCardCount);
        available = true;
    }
    taskEXIT_CRITICAL(&dashboardMux);
    return available;
}

void applyTextNotification(const char *payload) {
    String s(payload);
    s.trim();
    if (s.length() == 0) return;

    TextNotification nextNotif;
    resetTextNotification(nextNotif);

    if (s.startsWith("{")) {
        // --- JSON payload ---
        // text
        int ti = s.indexOf("\"text\":");
        if (ti >= 0) {
            int tq1 = s.indexOf('"', ti + 7);
            if (tq1 >= 0) {
                int tq2 = s.indexOf('"', tq1 + 1);
                if (tq2 > tq1)
                    s.substring(tq1 + 1, tq2).toCharArray(nextNotif.text, sizeof(nextNotif.text));
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
            if (dma_display) nextNotif.color = dma_display->color565(r, g, b);
        }

        // size (1–3)
        int si = s.indexOf("\"size\":");
        if (si >= 0) {
            int sv = s.substring(si + 7).toInt();
            if (sv >= 1 && sv <= 3) nextNotif.size = (uint8_t)sv;
        }

        // effect
        if (s.indexOf("\"effect\":\"rainbow\"") >= 0) nextNotif.rainbow = true;

        // duration: loops for scrolling text, seconds for non-scrolling text
        int di = s.indexOf("\"duration\":");
        if (di >= 0) nextNotif.duration = (uint32_t)s.substring(di + 11).toInt();
    } else {
        // --- Plain-text payload ---
        s.toCharArray(nextNotif.text, sizeof(nextNotif.text));
    }

    if (nextNotif.text[0] != '\0') {
        bool enqueued = false;
        uint8_t depth = 0;
        taskENTER_CRITICAL(&textNotifMux);
        if (textNotifCount < NOTIFY_QUEUE_LEN) {
            TextNotification &slot = textNotifQueue[textNotifTail];
            memcpy(slot.text, nextNotif.text, sizeof(slot.text));
            slot.color = nextNotif.color;
            slot.size = nextNotif.size;
            slot.rainbow = nextNotif.rainbow;
            slot.duration = nextNotif.duration;
            slot.pending = true;

            textNotifTail = (uint8_t)((textNotifTail + 1) % NOTIFY_QUEUE_LEN);
            textNotifCount++;
            depth = textNotifCount;

            memcpy(textNotif.text, nextNotif.text, sizeof(textNotif.text));
            textNotif.color = nextNotif.color;
            textNotif.size = nextNotif.size;
            textNotif.rainbow = nextNotif.rainbow;
            textNotif.duration = nextNotif.duration;
            textNotif.pending = true;
            enqueued = true;
        }
        taskEXIT_CRITICAL(&textNotifMux);

        if (enqueued) {
            Serial.printf("[NOTIFY] queued text='%s' size=%d rainbow=%d dur=%u depth=%u\n",
                          nextNotif.text, nextNotif.size,
                          (int)nextNotif.rainbow, nextNotif.duration, depth);
        } else {
            Serial.printf("[NOTIFY] dropped text='%s' (queue full: %u)\n",
                          nextNotif.text, (unsigned)NOTIFY_QUEUE_LEN);
        }
    }
}

void applyDashboardPayload(const char *payload) {
    if (!payload) return;
    String s(payload);
    s.trim();
    if (s.length() == 0) return;

    String wrapped = s;
    if (s.startsWith("{")) {
        wrapped = "[" + s + "]";
    }
    if (!wrapped.startsWith("[")) {
        Serial.println("[DASH] invalid payload: expected JSON array/object");
        return;
    }

    TextNotification parsed[DASHBOARD_MAX_ITEMS];
    for (uint8_t i = 0; i < DASHBOARD_MAX_ITEMS; i++) {
        resetTextNotification(parsed[i]);
        parsed[i].duration = dashboardDwellSeconds;
    }

    uint8_t parsedCount = 0;
    int depth = 0;
    int objStart = -1;
    bool inStr = false;
    char prev = '\0';

    for (int i = 0; i < wrapped.length() && parsedCount < DASHBOARD_MAX_ITEMS; i++) {
        char c = wrapped[i];
        if (c == '"' && prev != '\\') inStr = !inStr;
        if (!inStr) {
            if (c == '{') {
                if (depth == 0) objStart = i;
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0 && objStart >= 0) {
                    String obj = wrapped.substring(objStart, i + 1);
                    TextNotification card;
                    resetTextNotification(card);
                    card.duration = dashboardDwellSeconds;

                    String type;
                    if (!jsonExtractQuoted(obj, "type", &type)) type = "text";

                    String text;
                    String title;
                    String value;
                    String unit;
                    jsonExtractQuoted(obj, "text", &text);
                    jsonExtractQuoted(obj, "title", &title);
                    jsonExtractQuoted(obj, "value", &value);
                    jsonExtractQuoted(obj, "unit", &unit);

                    if (type == "sensor") {
                        String built;
                        if (title.length() > 0) {
                            built += title;
                            built += ": ";
                        }
                        if (value.length() > 0) built += value;
                        if (unit.length() > 0) built += unit;
                        text = built;
                    }

                    if (text.length() == 0) {
                        objStart = -1;
                        prev = c;
                        continue;
                    }

                    text.toCharArray(card.text, sizeof(card.text));

                    String color;
                    if (jsonExtractQuoted(obj, "color", &color)) {
                        if (color.length() == 7 && color[0] == '#') {
                            long v = strtol(color.substring(1).c_str(), nullptr, 16);
                            uint8_t r = (uint8_t)((v >> 16) & 0xFF);
                            uint8_t g = (uint8_t)((v >> 8) & 0xFF);
                            uint8_t b = (uint8_t)(v & 0xFF);
                            if (dma_display) card.color = dma_display->color565(r, g, b);
                        }
                    }

                    int sz = 0;
                    if (jsonExtractInt(obj, "size", &sz) && sz >= 1 && sz <= 3) {
                        card.size = (uint8_t)sz;
                    }

                    bool rb = false;
                    if (jsonExtractBool(obj, "rainbow", &rb)) card.rainbow = rb;
                    String effect;
                    if (jsonExtractQuoted(obj, "effect", &effect)) {
                        effect.toLowerCase();
                        if (effect == "rainbow") card.rainbow = true;
                    }

                    int dur = 0;
                    if (jsonExtractInt(obj, "duration", &dur) && dur > 0) {
                        card.duration = (uint32_t)dur;
                    }

                    parsed[parsedCount++] = card;
                    objStart = -1;
                }
            }
        }
        prev = c;
    }

    taskENTER_CRITICAL(&dashboardMux);
    resetDashboardCards();
    for (uint8_t i = 0; i < parsedCount; i++) {
        dashboardCards[i] = parsed[i];
    }
    dashboardCardCount = parsedCount;
    dashboardCardCursor = 0;
    taskEXIT_CRITICAL(&dashboardMux);

    Serial.printf("[DASH] loaded %u card(s)\n", parsedCount);
    mqttPublishDashboardState();
}

/* =================================================================
 *  BRIGHTNESS / POWER HELPERS
 * ================================================================= */
void applyBrightness(uint8_t val) {
    brightness = val;
    uint8_t capped = (safeBrightness && val > MAX_BRIGHTNESS_VAL) ? MAX_BRIGHTNESS_VAL : val;
    if (dma_display) dma_display->setBrightness8(panelOn ? capped : 0);
    Serial.printf("[BRIGHT] requested=%u effective=%u safe=%s\n",
                  val, panelOn ? capped : 0, safeBrightness ? "ON" : "OFF");
}

void applyPanelOn(bool on) {
    panelOn = on;
    if (dma_display) {
        if (!on) {
            dma_display->setBrightness8(0);
        } else {
            uint8_t capped = (safeBrightness && brightness > MAX_BRIGHTNESS_VAL) ? MAX_BRIGHTNESS_VAL : brightness;
            dma_display->setBrightness8(capped);
        }
    }
}

void applySafeBrightness(bool safe) {
    safeBrightness = safe;
    Preferences prefs;
    prefs.begin("panel", false);
    prefs.putBool("safe_br", safe);
    prefs.end();
    // Re-apply current brightness so the cap change takes effect immediately
    applyBrightness(brightness);
    // Re-publish discovery so HA updates the brightness slider scale
    mqttPublishDiscovery();
    Serial.printf("[BRIGHT] safe_brightness=%s\n", safe ? "ON" : "OFF");
}

void loadPanelConfig() {
    Preferences prefs;
    prefs.begin("panel", true);
    safeBrightness = prefs.getBool("safe_br", DEFAULT_SAFE_BRIGHTNESS);
    prefs.end();
    Serial.printf("[BRIGHT] loadPanelConfig safe_brightness=%s\n", safeBrightness ? "ON" : "OFF");
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
    uint8_t bScale = safeBrightness ? MAX_BRIGHTNESS_VAL : 255;
    String payload = "{\"name\":\"DeLorean DMD\""
        ",\"unique_id\":\"" + mqttClientId + "\""
        ",\"object_id\":\"" + mqttClientId + "\""
        ",\"schema\":\"json\""
        ",\"command_topic\":\"" + mqttTopic + "/set\""
        ",\"state_topic\":\"" + mqttTopic + "/state\""
        ",\"brightness\":true"
        ",\"brightness_scale\":" + String(bScale) +
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
    String swPayload = "{\"name\":\"Clock Mode\""
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
    String numPayload = "{\"name\":\"Clock Every N GIFs\""
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
    String rebootPayload = "{\"name\":\"Reboot\""
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

    String dashSwitchDiscTopic = "homeassistant/switch/" + mqttClientId + "_dashboard/config";
    String dashSwitchPayload = "{\"name\":\"Dashboard Mode\""
        ",\"unique_id\":\"" + mqttClientId + "_dashboard\""
        ",\"object_id\":\"" + mqttClientId + "_dashboard\""
        ",\"command_topic\":\"" + mqttTopic + "/dashboard/mode/set\""
        ",\"state_topic\":\"" + mqttTopic + "/dashboard/state\""
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
    mqttClient.publish(dashSwitchDiscTopic.c_str(), dashSwitchPayload.c_str(), true);

    String dashDwellDiscTopic = "homeassistant/number/" + mqttClientId + "_dashboard_dwell/config";
    String dashDwellPayload = "{\"name\":\"Dashboard Dwell\""
        ",\"unique_id\":\"" + mqttClientId + "_dashboard_dwell\""
        ",\"object_id\":\"" + mqttClientId + "_dashboard_dwell\""
        ",\"command_topic\":\"" + mqttTopic + "/dashboard/dwell/set\""
        ",\"state_topic\":\"" + mqttTopic + "/dashboard/state\""
        ",\"value_template\":\"{{ value_json.dwell }}\""
        ",\"min\":1,\"max\":120,\"step\":1,\"mode\":\"box\""
        ",\"unit_of_measurement\":\"s\""
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
    mqttClient.publish(dashDwellDiscTopic.c_str(), dashDwellPayload.c_str(), true);

    String dashProfileDiscTopic = "homeassistant/select/" + mqttClientId + "_dashboard_profile/config";
    String dashProfilePayload = "{\"name\":\"Dashboard Profile\""
        ",\"unique_id\":\"" + mqttClientId + "_dashboard_profile\""
        ",\"object_id\":\"" + mqttClientId + "_dashboard_profile\""
        ",\"command_topic\":\"" + mqttTopic + "/dashboard/profile/set\""
        ",\"state_topic\":\"" + mqttTopic + "/dashboard/state\""
        ",\"value_template\":\"{{ value_json.profile }}\""
        ",\"options\":[\"default\",\"focus\",\"night\",\"away\"]"
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
    mqttClient.publish(dashProfileDiscTopic.c_str(), dashProfilePayload.c_str(), true);

    String safeBrDiscTopic = "homeassistant/switch/" + mqttClientId + "_safe_brightness/config";
    String safeBrPayload = "{\"name\":\"Safe Brightness\""
        ",\"unique_id\":\"" + mqttClientId + "_safe_brightness\""
        ",\"object_id\":\"" + mqttClientId + "_safe_brightness\""
        ",\"command_topic\":\"" + mqttTopic + "/brightness/safe/set\""
        ",\"state_topic\":\"" + mqttTopic + "/brightness/safe/state\""
        ",\"payload_on\":\"ON\""
        ",\"payload_off\":\"OFF\""
        ",\"state_on\":\"ON\""
        ",\"state_off\":\"OFF\""
        ",\"entity_category\":\"config\""
        ",\"icon\":\"mdi:shield-lock\""
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
    mqttClient.publish(safeBrDiscTopic.c_str(), safeBrPayload.c_str(), true);
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
    String safeBrTopic = mqttTopic + "/brightness/safe/set";
    String notifyTopic = mqttTopic + "/notify";
    String clockTopic  = mqttTopic + "/clock/set";
    String clockEveryTopic = mqttTopic + "/clock/every/set";
    String rebootTopic = mqttTopic + "/reboot/set";
    String dashSetTopic = mqttTopic + "/dashboard/set";
    String dashModeTopic = mqttTopic + "/dashboard/mode/set";
    String dashDwellTopic = mqttTopic + "/dashboard/dwell/set";
    String dashProfileTopic = mqttTopic + "/dashboard/profile/set";

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
    } else if (t == safeBrTopic) {
        String s(msg);
        s.trim();
        s.toUpperCase();
        bool safe = (s == "ON" || s == "1" || s == "TRUE");
        applySafeBrightness(safe);
        String safeBrStateTopic = mqttTopic + "/brightness/safe/state";
        mqttClient.publish(safeBrStateTopic.c_str(), safeBrightness ? "ON" : "OFF", true);
    } else if (t == notifyTopic) {
        // Delegate to shared parser: JSON or plain-text payload
        applyTextNotification(msg);
    } else if (t == clockTopic) {
        applyClockConfigPayload(msg);
    } else if (t == clockEveryTopic) {
        int v = String(msg).toInt();
        if (v > 0 && v < 65535) updateClockConfig(clockModeEnabled, (uint16_t)v, clockTz, true);
    } else if (t == dashSetTopic) {
        applyDashboardPayload(msg);
    } else if (t == dashModeTopic) {
        applyDashboardModePayload(msg);
    } else if (t == dashDwellTopic) {
        applyDashboardDwellPayload(msg);
    } else if (t == dashProfileTopic) {
        applyDashboardProfilePayload(msg);
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
        String safeBrTopic = mqttTopic + "/brightness/safe/set";
        String notifyTopic = mqttTopic + "/notify";
        String clockTopic  = mqttTopic + "/clock/set";
        String clockEveryTopic = mqttTopic + "/clock/every/set";
        String rebootTopic = mqttTopic + "/reboot/set";
        String dashSetTopic = mqttTopic + "/dashboard/set";
        String dashModeTopic = mqttTopic + "/dashboard/mode/set";
        String dashDwellTopic = mqttTopic + "/dashboard/dwell/set";
        String dashProfileTopic = mqttTopic + "/dashboard/profile/set";
        mqttClient.subscribe(cmdTopic.c_str());
        mqttClient.subscribe(brTopic.c_str());
        mqttClient.subscribe(safeBrTopic.c_str());
        mqttClient.subscribe(notifyTopic.c_str());
        mqttClient.subscribe(clockTopic.c_str());
        mqttClient.subscribe(clockEveryTopic.c_str());
        mqttClient.subscribe(rebootTopic.c_str());
        mqttClient.subscribe(dashSetTopic.c_str());
        mqttClient.subscribe(dashModeTopic.c_str());
        mqttClient.subscribe(dashDwellTopic.c_str());
        mqttClient.subscribe(dashProfileTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", notifyTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", clockTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", clockEveryTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", rebootTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", dashSetTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", dashModeTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", dashDwellTopic.c_str());
        Serial.printf("[MQTT] Subscribed to %s\n", dashProfileTopic.c_str());
        mqttPublishDiscovery();
        mqttPublishState();
        mqttPublishClockState();
        mqttPublishDashboardState();
        String safeBrStateTopic = mqttTopic + "/brightness/safe/state";
        mqttClient.publish(safeBrStateTopic.c_str(), safeBrightness ? "ON" : "OFF", true);
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
    initRuntimeDefaults();
    loadMqttConfig();
    loadClockConfig();
    loadDashboardConfig();
    loadPanelConfig();
}

void mqttLoop() {
    if (!mqttEnabled) return;
    if (!mqttClient.connected()) mqttConnect();
    mqttClient.loop();
}
