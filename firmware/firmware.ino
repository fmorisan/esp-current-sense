// esp-current-sense — NodeMCU-32S (ESP32-D0WD-V3)
//
// Phase 4: CT sampling + HTTP /metrics + MQTT with Home Assistant discovery.
//
//   LED status:
//     fast blink — joining WiFi
//     steady on  — ready, HTTP server up
//     off        — WiFi lost (retries every 10 s)
//
//   HTTP:      GET / and /metrics (same JSON as the MQTT state topic)
//   MQTT out:  esp-current-sense/state            JSON every ~5 s
//              esp-current-sense/availability     online/offline (retained, LWT)
//              homeassistant/sensor/…/config      HA discovery (retained)
//
//   Until the clamp's bias divider is wired to GPIO34, the pin floats and
//   readings show ~0 A — that's expected, not a fault.
//
// Roadmap (see README.md):
//   5. OTA updates, calibration constants in NVS

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "ct_sensor.h"
#include "mqtt.h"

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
static const uint32_t kDebugPrintMs = 3000;         // serial status cadence
static const uint32_t kSampleBurstMs = 20;          // per-loop sampling budget
static const uint32_t kMqttPublishMs = 5000;        // state publish cadence
static const float kAssumedMainsVoltage = 220.0f;   // until real voltage sensing (phase 6)

static WebServer server(80);
static bool wifiReady = false;
static bool ledState = false;
static uint32_t lastBlinkToggle = 0;
static uint32_t lastReconnectAttempt = 0;
static uint32_t lastDebugPrint = 0;
static uint32_t lastMqttPublish = 0;
static bool haveLastAccum = false;
static uint32_t lastAccumMs = 0;
static double energyWh = 0.0;  // apparent-energy integral since boot

static void setLed(bool on) { digitalWrite(LED_BUILTIN, on ? HIGH : LOW); }

// One JSON payload shared by HTTP /metrics and the MQTT state topic so HA
// value_templates and any curl debugging always agree.
static void buildMetricsJson(char* buf, size_t len) {
  snprintf(buf, len,
           "{\"current_a\":%.3f,\"power_va\":%.1f,\"voltage\":%.1f,"
           "\"energy_kwh\":%.4f,\"vrms_mv\":%.1f,\"bias_mv\":%.0f,\"win_ms\":%lu,\"windows\":%u,"
           "\"wifi_rssi_dbm\":%d,\"heap_free\":%u,\"uptime_s\":%lu,"
           "\"source\":\"adc\"}",
           ct::currentA(), ct::powerVa(kAssumedMainsVoltage), kAssumedMainsVoltage,
           energyWh / 1000.0, ct::vrmsMillivolts(), ct::biasMillivolts(), (unsigned long)ct::windowMs,
           (unsigned)ct::windowsDone, (int)WiFi.RSSI(),
           (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
}

static void handleMetrics() {
  char buf[320];
  buildMetricsJson(buf, sizeof(buf));
  server.send(200, "application/json", buf);
}

static void handleRoot() {
  server.send(200, "text/html",
              "<html><head><meta name=\"viewport\" content=\"width=device-width\"></head>"
              "<body><h3>esp-current-sense</h3>"
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
  Serial.println("\r\n[esp-current-sense] boot — phase 4 (CT + HTTP + MQTT)");

  ct::setup();
  mqtt::setup();
  connectWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });
  server.begin();
  Serial.println("[http] server listening on :80");
}

void loop() {
  server.handleClient();
  ct::burst(kSampleBurstMs);
  mqtt::loop();

  // Integrate apparent power over time -> Wh (resets at boot; HA treats
  // total_increasing drops as a meter reset, so that's safe).
  const uint32_t now = millis();
  if (haveLastAccum) {
    energyWh += ct::powerVa(kAssumedMainsVoltage) * (now - lastAccumMs) / 3600000.0;
  }
  lastAccumMs = now;
  haveLastAccum = true;

  if (now - lastMqttPublish >= kMqttPublishMs) {
    lastMqttPublish = now;
    char buf[320];
    buildMetricsJson(buf, sizeof(buf));
    mqtt::publishState(buf);
  }

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

  if (millis() - lastDebugPrint >= kDebugPrintMs) {
    lastDebugPrint = millis();
    Serial.printf("[ct] win=%lums  bias=%.0fmV  vrms=%.1fmV  I=%.2fA  (windows=%u)\r\n",
                  (unsigned long)ct::windowMs, ct::biasMillivolts(), ct::vrmsMillivolts(),
                  ct::currentA(), (unsigned)ct::windowsDone);
  }

  delay(1);
}
