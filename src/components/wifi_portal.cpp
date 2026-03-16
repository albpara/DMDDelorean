#include "wifi_portal.h"
#include "portal_html.h"
#include "mqtt.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

/* =================================================================
 *  WiFi / portal globals
 * ================================================================= */
WebServer  webServer(80);
DNSServer  dnsServer;
bool       apMode = false;
unsigned long wifiConnectedAt = 0;

/* =================================================================
 *  WEB SERVER SERVICING — call from playback frame loops
 * ================================================================= */
void serviceWeb() {
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

static void handleNotify() {
    String text     = webServer.arg("text");
    String colorStr = webServer.arg("color");    // "r,g,b"
    String effectStr = webServer.arg("effect");  // scroll / static / blink
    int    size     = webServer.arg("size").toInt();
    int    dur      = webServer.arg("duration").toInt();

    if (text.length() == 0) {
        webServer.send(200, "application/json", "{\"ok\":false,\"msg\":\"text required\"}");
        return;
    }

    // Escape the text value for safe JSON embedding
    String escaped = text;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");

    // Build a JSON payload and delegate to the shared parser
    String payload = "{\"text\":\"" + escaped + "\"";
    if (colorStr.length() > 0)
        payload += ",\"color\":[" + colorStr + "]";
    if (size >= 1 && size <= 3)
        payload += ",\"size\":" + String(size);
    if (effectStr.length() > 0)
        payload += ",\"effect\":\"" + effectStr + "\"";
    if (dur > 0)
        payload += ",\"duration\":" + String(dur);
    payload += "}";

    applyTextNotification(payload.c_str());
    webServer.send(200, "application/json", "{\"ok\":true,\"msg\":\"Notification queued\"}");
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
    webServer.on("/notify", HTTP_POST, handleNotify);
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
        Serial.printf("[WIFI] Trying saved network '%s'...\n", savedSSID.c_str());
        showMessage("WiFi...", dma_display->color565(0, 200, 200));
        WiFi.mode(WIFI_STA);
        WiFi.begin(savedSSID.c_str(), savedPass.c_str());

        unsigned long start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_CONNECT_TIMEOUT) {
            delay(250);
        }

        if (WiFi.status() == WL_CONNECTED) {
            apMode = false;
            wifiConnectedAt = millis();
            Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
            webServer.begin();
            return;
        }
        Serial.println("[WIFI] Saved credentials failed");
        WiFi.disconnect();
    }

    Serial.println("[WIFI] Starting AP: " AP_SSID);
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    delay(100);
    Serial.printf("[WIFI] AP IP: %s\n", WiFi.softAPIP().toString().c_str());

    dnsServer.start(53, "*", WiFi.softAPIP());
    apMode = true;

    webServer.begin();
    Serial.println("[WIFI] Captive portal ready");
}
