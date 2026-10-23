// esp-current-sense — NodeMCU-32S (ESP32-D0WD-V3)
//
// Phase 2: WiFi + HTTP /metrics endpoint.
//
//   LED status:
//     fast blink — joining WiFi
//     steady on  — ready, HTTP server up
//     off        — WiFi lost (retries every 10 s)
//
//   Endpoints:
//     GET /         tiny index page (handy from a phone browser)
//     GET /metrics  JSON readings (current values are placeholders until
//                   the CT frontend lands in phase 3)
//
//   Find it at:  http://<ip>/metrics  or  http://esp-current-sense.local/metrics
//
// Roadmap (see README.md):
//   3. CT sampling on GPIO34 (ADC1_CH6) + RMS current
//   4. MQTT + Home Assistant discovery
//   5. OTA updates, calibration constants in NVS

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Missing firmware/secrets.h — run: cp firmware/secrets.h.example firmware/secrets.h and fill in WiFi credentials."
#endif

#ifndef LED_BUILTIN
#define LED_BUILTIN 2  // NodeMCU-32S onboard blue LED (may be active-low on some clones)
#endif

static const uint32_t kSerialBaud = 115200;
static const uint32_t kConnectBlinkMs = 100;        // fast blink while joining
static const uint32_t kReconnectCheckMs = 10000;    // WiFi retry interval
static const uint32_t kConnectTimeoutMs = 15000;
static const float kAssumedMainsVoltage = 230.0f;   // until real voltage sensing (phase 6)

static WebServer server(80);
static bool wifiReady = false;
static bool ledState = false;
static uint32_t lastBlinkToggle = 0;
static uint32_t lastReconnectAttempt = 0;

static void setLed(bool on) { digitalWrite(LED_BUILTIN, on ? HIGH : LOW); }

static void handleMetrics() {
  // Placeholder readings until phase 3 wires up the CT on GPIO34.
  const float currentA = 0.0f;
  const float powerVa = currentA * kAssumedMainsVoltage;

  char buf[256];
  snprintf(buf, sizeof(buf),
           "{\"current_a\":%.3f,\"power_va\":%.1f,\"voltage\":%.1f,"
           "\"wifi_rssi_dbm\":%d,\"heap_free\":%u,\"uptime_s\":%lu,"
           "\"source\":\"placeholder\"}",
           currentA, powerVa, kAssumedMainsVoltage, (int)WiFi.RSSI(),
           (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
  server.send(200, "application/json", buf);
}

static void handleRoot() {
  server.send(200, "text/html",
              "<html><head><meta name=\"viewport\" content=\"width=device-width\"></head>"
              "<body><h3>esp-current-sense</h3>"
              "<p>energy monitor &mdash; CT not wired yet (placeholder data)</p>"
              "<p><a href=\"/metrics\">/metrics (JSON)</a></p></body></html>");
}

static void connectWifi() {
  Serial.printf("[wifi] connecting to \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < kConnectTimeoutMs) {
    if (millis() - lastBlinkToggle >= kConnectBlinkMs) {  // fast blink while joining
      lastBlinkToggle = millis();
      ledState = !ledState;
      setLed(ledState);
    }
    delay(5);
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiReady = true;
    setLed(true);  // steady on = ready
    Serial.printf("\r\n[wifi] connected  ip=%s  rssi=%d dBm\r\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (MDNS.begin(HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);  // advertise so Bonjour/resolvers actually answer
      Serial.printf("[mdns] http://%s.local/metrics\r\n", HOSTNAME);
    }
  } else {
    wifiReady = false;
    setLed(false);
    Serial.println("\r\n[wifi] failed to connect (check secrets.h / signal)");
  }
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  Serial.begin(kSerialBaud);
  delay(200);
  Serial.println("\r\n[esp-current-sense] boot — phase 2 (wifi + http)");

  connectWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });
  server.begin();
  Serial.println("[http] server listening on :80");
}

void loop() {
  server.handleClient();

  if (wifiReady && WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] connection lost");
    wifiReady = false;
    setLed(false);
  }
  if (!wifiReady && millis() - lastReconnectAttempt >= kReconnectCheckMs) {
    lastReconnectAttempt = millis();
    Serial.println("[wifi] retrying…");
    WiFi.disconnect();
    connectWifi();
  }
  delay(2);
}
