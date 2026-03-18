#include "wifi_portal.h"
#include "portal_html.h"
#include "mqtt.h"
#include <WiFi.h>
#include <Preferences.h>
#include <Update.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

/* =================================================================
 *  WiFi / portal globals
 * ================================================================= */
WebServer  webServer(80);
DNSServer  dnsServer;
bool       apMode = false;
unsigned long wifiConnectedAt = 0;
static bool wifiStaConnecting = false;
static unsigned long wifiStaStartedAt = 0;
static bool wifiIpShown = false;
static bool wifiSavedRetryEnabled = false;
static uint8_t wifiSavedRetryAttempt = 0;
static unsigned long wifiNextRetryAt = 0;
static String wifiSavedSSID;
static String wifiSavedPass;

static void startAccessPointPortal() {
    Serial.println("[WIFI] Starting AP: " AP_SSID);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    delay(100);
    Serial.printf("[WIFI] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    dnsServer.start(53, "*", WiFi.softAPIP());
    apMode = true;
    wifiStaConnecting = false;
    wifiIpShown = false;
    Serial.println("[WIFI] Captive portal ready");
}

static void startSavedCredentialAttempt() {
    if (!wifiSavedRetryEnabled || wifiSavedSSID.length() == 0) return;

    wifiSavedRetryAttempt++;
    WiFi.mode(apMode ? WIFI_AP_STA : WIFI_STA);
    WiFi.begin(wifiSavedSSID.c_str(), wifiSavedPass.c_str());
    wifiStaConnecting = true;
    wifiStaStartedAt = millis();
    wifiNextRetryAt = 0;

    Serial.printf("[WIFI] Saved-credential attempt %u/%u: '%s'\n",
                  wifiSavedRetryAttempt,
                  (unsigned)WIFI_BOOT_CONNECT_MAX_RETRIES,
                  wifiSavedSSID.c_str());
}

/* =================================================================
 *  WEB SERVER SERVICING — call from playback frame loops
 * ================================================================= */
void serviceWeb() {
    if (wifiStaConnecting && WiFi.status() == WL_CONNECTED) {
        wifiStaConnecting = false;
        wifiSavedRetryEnabled = false;
        apMode = false;
        wifiConnectedAt = millis();
        String ip = WiFi.localIP().toString();
        Serial.printf("[WIFI] Connected! IP: %s\n", ip.c_str());
        if (!wifiIpShown) {
            showMessage(ip.c_str(), dma_display->color565(0, 200, 200));
            wifiIpShown = true;
        }
        dnsServer.stop();
        WiFi.mode(WIFI_STA);
    } else if (wifiStaConnecting && (millis() - wifiStaStartedAt) >= WIFI_CONNECT_TIMEOUT) {
        Serial.printf("[WIFI] Saved credentials failed (attempt %u/%u)\n",
                      wifiSavedRetryAttempt,
                      (unsigned)WIFI_BOOT_CONNECT_MAX_RETRIES);
        wifiStaConnecting = false;
        WiFi.disconnect();

        if (wifiSavedRetryEnabled && wifiSavedRetryAttempt < WIFI_BOOT_CONNECT_MAX_RETRIES) {
            if (!apMode) startAccessPointPortal();
            wifiNextRetryAt = millis() + WIFI_BOOT_RETRY_INTERVAL;
            Serial.printf("[WIFI] Retrying in background in %u ms\n", (unsigned)WIFI_BOOT_RETRY_INTERVAL);
        } else {
            wifiSavedRetryEnabled = false;
            if (!apMode) startAccessPointPortal();
            Serial.println("[WIFI] Max retry attempts reached, staying in AP mode");
        }
    } else if (!wifiStaConnecting && wifiSavedRetryEnabled && wifiNextRetryAt != 0 &&
               (long)(millis() - wifiNextRetryAt) >= 0) {
        startSavedCredentialAttempt();
    }

    webServer.handleClient();
    if (apMode) dnsServer.processNextRequest();
    mqttLoop();
}

/* =================================================================
 *  HTTP ROUTE HANDLERS
 * ================================================================= */
static void handleRoot() {
    webServer.send_P(200, "text/html", PORTAL_HTML);
}

static void handleCaptiveRedirect() {
    webServer.sendHeader("Location", "http://192.168.4.1/");
    webServer.send(302, "text/plain", "");
}

static void handleScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i) json += ',';
        json += "{\"s\":\"";
        String ssid = WiFi.SSID(i);
        ssid.replace("\"", "\\\"");
        json += ssid;
        json += "\",\"r\":";
        json += WiFi.RSSI(i);
        json += ",\"e\":";
        json += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "true" : "false";
        json += '}';
    }
    json += ']';
    WiFi.scanDelete();
    webServer.send(200, "application/json", json);
}

static void handleConnect() {
    String ssid = webServer.arg("ssid");
    String pass = webServer.arg("pass");

    if (ssid.length() == 0) {
        webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"SSID is empty\"}");
        return;
    }

    Serial.printf("[WIFI] Connecting to '%s'...\n", ssid.c_str());

    wifiSavedRetryEnabled = false;
    wifiStaConnecting = false;
    wifiNextRetryAt = 0;

    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(ssid.c_str(), pass.c_str());

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_RETRY_TIMEOUT) {
        delay(250);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Preferences prefs;
        prefs.begin("wifi", false);
        prefs.putString("ssid", ssid);
        prefs.putString("pass", pass);
        prefs.end();

        String ip = WiFi.localIP().toString();
        Serial.printf("[WIFI] Connected! IP: %s\n", ip.c_str());
        showMessage(ip.c_str(), dma_display->color565(0, 200, 200));
        wifiIpShown = true;
        wifiStaConnecting = false;

        String resp = "{\"ok\":true,\"ip\":\"" + ip + "\"}";
        webServer.send(200, "application/json", resp);

        delay(500);
        dnsServer.stop();
        WiFi.mode(WIFI_STA);
        apMode = false;
        wifiConnectedAt = millis();
        Serial.println("[WIFI] Switched to STA mode");
    } else {
        WiFi.disconnect();
        WiFi.mode(apMode ? WIFI_AP : WIFI_STA);
        Serial.println("[WIFI] Connection failed");
        webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"Connection failed. Check credentials.\"}");
    }
}

static void handleStatus() {
    bool connected = (WiFi.status() == WL_CONNECTED);
    String json = "{\"connected\":";
    json += connected ? "true" : "false";
    if (connected) {
        json += ",\"ssid\":\"";
        String ssid = WiFi.SSID();
        ssid.replace("\"", "\\\"");
        json += ssid;
        json += "\",\"ip\":\"";
        json += WiFi.localIP().toString();
        json += "\"";
    }
    json += ",\"ap\":";
    json += apMode ? "true" : "false";
    // MQTT state
    json += ",\"mqtt\":{\"server\":\"";
    json += mqttServer;
    json += "\",\"port\":";
    json += mqttPort;
    json += ",\"user\":\"";
    json += mqttUser;
    json += "\",\"client\":\"";
    json += mqttClientId;
    json += "\",\"topic\":\"";
    json += mqttTopic;
    json += "\",\"connected\":";
    json += mqttClient.connected() ? "true" : "false";
    json += "}";
    // Panel state
    json += ",\"panel_on\":";
    json += panelOn ? "true" : "false";
    json += ",\"brightness\":";
    json += brightness;
    json += ",\"max_brightness\":";
    json += MAX_BRIGHTNESS_VAL;

    // Clock mode state
    json += ",\"clock\":{\"enabled\":";
    json += clockModeEnabled ? "true" : "false";
    json += ",\"every\":";
    json += clockEveryNGifs;
    json += ",\"tz\":\"";
    String tz = String(clockTz);
    tz.replace("\"", "\\\"");
    json += tz;
    json += "\",\"synced\":";
    json += clockTimeValid ? "true" : "false";
    json += "}";

    json += '}';
    webServer.send(200, "application/json", json);
}

static void handleMqttSave() {
    String server = webServer.arg("server");
    uint16_t port = (uint16_t)webServer.arg("port").toInt();
    String user   = webServer.arg("user");
    String pass   = webServer.arg("pass");
    String client = webServer.arg("client");
    String topic  = webServer.arg("topic");

    if (port == 0) port = MQTT_DEFAULT_PORT;
    if (client.length() == 0) client = MQTT_DEFAULT_CLIENT;
    if (topic.length() == 0)  topic  = MQTT_DEFAULT_TOPIC;

    Preferences prefs;
    prefs.begin("mqtt", false);
    prefs.putString("server", server);
    prefs.putUShort("port", port);
    prefs.putString("user", user);
    prefs.putString("pass", pass);
    prefs.putString("client", client);
    prefs.putString("topic", topic);
    prefs.end();

    mqttServer   = server;
    mqttPort     = port;
    mqttUser     = user;
    mqttPass     = pass;
    mqttClientId = client;
    mqttTopic    = topic;

    if (mqttClient.connected()) mqttClient.disconnect();
    mqttEnabled = (server.length() > 0);
    mqttLastRetry = 0;

    String msg = mqttEnabled ? "Saved. Connecting..." : "MQTT disabled (no server)";
    webServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"" + msg + "\"}");

    Serial.printf("[MQTT] Config saved: %s:%d client=%s topic=%s\n",
                  server.c_str(), port, client.c_str(), topic.c_str());
}

static void handlePanel() {
    bool on   = webServer.arg("on") == "1";
    int  br   = webServer.arg("brightness").toInt();
    if (br < 0) br = 0;
    if (br > 255) br = 255;

    applyPanelOn(on);
    applyBrightness((uint8_t)br);
    mqttPublishState();

    uint8_t effective = (brightness > MAX_BRIGHTNESS_VAL) ? MAX_BRIGHTNESS_VAL : brightness;
    String resp = "{\"ok\":true,\"msg\":\"";
    resp += on ? "ON" : "OFF";
    resp += ", brightness=";
    resp += effective;
    if (brightness > MAX_BRIGHTNESS_VAL) {
        resp += " (capped from ";
        resp += brightness;
        resp += ")";
    }
    resp += "\"}";
    webServer.send(200, "application/json", resp);
}

static void handleClockSave() {
    bool enabled = webServer.arg("enabled") == "1";
    int every = webServer.arg("every").toInt();
    String tz = webServer.arg("tz");

    if (every < 1) every = 1;
    if (every > 1000) every = 1000;
    if (tz.length() == 0) tz = "UTC0";

    updateClockConfig(enabled, (uint16_t)every, tz.c_str(), true);
    webServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"Clock settings saved\"}");
}

static void handleNotify() {
    String text   = webServer.arg("text");
    String color  = webServer.arg("color");   // "#RRGGBB" from color picker
    int    size   = webServer.arg("size").toInt();
    String effect = webServer.arg("effect");
    uint32_t dur  = (uint32_t)webServer.arg("duration").toInt();

    if (text.length() == 0) {
        webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"No text\"}");
        return;
    }
    if (size < 1 || size > 3) size = 1;

    // Build a JSON payload and delegate to the shared parser
    String escaped = text;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");

    String payload = "{\"text\":\"" + escaped + "\"";
    if (color.length() == 7 && color[0] == '#')
        payload += ",\"color\":\"" + color + "\"";
    if (size >= 1 && size <= 3)
        payload += ",\"size\":" + String(size);
    if (effect.length() > 0)
        payload += ",\"effect\":\"" + effect + "\"";
    if (dur > 0)
        payload += ",\"duration\":" + String(dur);
    payload += "}";

    applyTextNotification(payload.c_str());
    webServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"Notification accepted\"}");
}

/* OTA upload response (called after upload completes or fails) */
static void handleOtaFinal() {
    if (Update.hasError()) {
        String err = "OTA failed: ";
        err += Update.errorString();
        Serial.println("[OTA] " + err);
        webServer.send(500, "application/json", "{\"ok\":false,\"msg\":\"" + err + "\"}");
    } else {
        Serial.println("[OTA] Update successful — rebooting");
        webServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"Update successful. Rebooting...\"}");
        delay(500);
        ESP.restart();
    }
}

/* OTA upload progress handler (called per chunk by WebServer) */
static void handleOtaUpload() {
    HTTPUpload &upload = webServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Starting update: %s (%u bytes)\n",
                      upload.filename.c_str(), upload.totalSize);
        showMessage("OTA...", dma_display->color565(255, 200, 0));
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Serial.printf("[OTA] begin() error: %s\n", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Serial.printf("[OTA] write() error: %s\n", Update.errorString());
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Upload complete: %u bytes\n", upload.totalSize);
        } else {
            Serial.printf("[OTA] end() error: %s\n", Update.errorString());
        }
    }
}

static void handleNotFound() {
    if (apMode) {
        handleCaptiveRedirect();
    } else {
        webServer.send(404, "text/plain", "Not found");
    }
}

/* =================================================================
 *  WIFI SETUP — connect with saved creds or start AP with portal
 * ================================================================= */
void wifiSetup() {
    // Register HTTP routes
    webServer.on("/", HTTP_GET, handleRoot);
    webServer.on("/scan", HTTP_GET, handleScan);
    webServer.on("/connect", HTTP_POST, handleConnect);
    webServer.on("/status", HTTP_GET, handleStatus);
    webServer.on("/mqtt", HTTP_POST, handleMqttSave);
    webServer.on("/panel", HTTP_POST, handlePanel);
    webServer.on("/clock", HTTP_POST, handleClockSave);
    webServer.on("/notify", HTTP_POST, handleNotify);
    webServer.on("/update", HTTP_POST, handleOtaFinal, handleOtaUpload);
    webServer.on("/generate_204", HTTP_GET, handleCaptiveRedirect);
    webServer.on("/hotspot-detect.html", HTTP_GET, handleCaptiveRedirect);
    webServer.on("/connecttest.txt", HTTP_GET, handleCaptiveRedirect);
    webServer.on("/redirect", HTTP_GET, handleCaptiveRedirect);
    webServer.on("/canonical.html", HTTP_GET, handleCaptiveRedirect);
    webServer.on("/success.txt", HTTP_GET, handleCaptiveRedirect);
    webServer.onNotFound(handleNotFound);

    Preferences prefs;
    prefs.begin("wifi", true);
    String savedSSID = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");
    prefs.end();

    if (savedSSID.length() > 0) {
        wifiSavedSSID = savedSSID;
        wifiSavedPass = savedPass;
        wifiSavedRetryEnabled = true;
        wifiSavedRetryAttempt = 0;
        wifiNextRetryAt = 0;
        apMode = false;
        wifiIpShown = false;
        Serial.printf("[WIFI] Saved credentials found for '%s'\n", savedSSID.c_str());
        Serial.printf("[WIFI] Background retry enabled (max %u attempts)\n",
                      (unsigned)WIFI_BOOT_CONNECT_MAX_RETRIES);
        startSavedCredentialAttempt();
        webServer.begin();
        return;
    }

    startAccessPointPortal();
    webServer.begin();
}
