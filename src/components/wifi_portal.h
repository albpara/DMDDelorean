#pragma once

#include <Arduino.h>
#include <WebServer.h>
#include <DNSServer.h>

/* WiFi / Captive Portal config */
#define AP_SSID              "DeLorean-DMD"
#define WIFI_CONNECT_TIMEOUT 20000   // ms to wait for saved creds on boot
#define WIFI_RETRY_TIMEOUT   15000   // ms to wait when user submits creds

extern WebServer  webServer;
extern DNSServer  dnsServer;
extern bool       apMode;
extern unsigned long wifiConnectedAt;

/* Provided by main.cpp */
class MatrixPanel_I2S_DMA;
extern MatrixPanel_I2S_DMA *dma_display;
void showMessage(const char *msg, uint16_t color, uint8_t size = 1);

void wifiSetup();
void serviceWeb();
