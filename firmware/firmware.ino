// esp-current-sense — NodeMCU-32S (ESP32-D0WD-V3)
//
// CT sampling + HTTP /metrics + MQTT with Home Assistant discovery + HTTP OTA.
//
//   LED status:
//     fast blink — joining WiFi
//     steady on  — ready, HTTP server up
//     off        — WiFi lost (retries every 10 s)
//
//   HTTP:      GET / and /metrics (same JSON as the MQTT state topic)
//              GET /samples (paired millivolt arrays, current polarity corrected)
//              GET /samples?raw=1 (uncorrected ADC readings)
//   OTA:       GET/POST /update (browser firmware upload, no authentication)
//   MQTT out:  esp-current-sense/state            JSON every ~5 s
//              esp-current-sense/availability     online/offline (retained, LWT)
//              homeassistant/sensor/…/config      HA discovery (retained)
//
//   Until the clamp's bias divider is wired to GPIO34, the pin floats and
//   readings show ~0 A — that's expected, not a fault.
//
// Roadmap (see README.md):
//   Calibration constants in NVS

#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPUpdateServer.h>
#include <WebServer.h>
#include <WiFi.h>
#include <Wire.h>

#include "mqtt.h"
#include "power_sampler.h"

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
static const uint32_t kDebugPrintMs = 15000;         // serial status cadence
static const uint32_t kSampleBurstMs = 20;          // per-loop sampling budget
static const uint32_t kMqttPublishMs = 1000;        // state publish cadence

static WebServer server(80);
static HTTPUpdateServer httpUpdater(true);  // log update progress/errors to serial
static bool wifiReady = false;
static bool ledState = false;
static uint32_t lastBlinkToggle = 0;
static uint32_t lastReconnectAttempt = 0;
static uint32_t lastDebugPrint = 0;
static uint32_t lastMqttPublish = 0;
static bool haveLastAccum = false;
static uint32_t lastAccumMs = 0;
static double energyWh = 0.0;  // consumed real energy since boot (import only)

static void setLed(bool on) { digitalWrite(LED_BUILTIN, on ? HIGH : LOW); }

// One JSON payload shared by HTTP /metrics and the MQTT state topic so HA
// value_templates and any curl debugging always agree.
static void buildMetricsJson(char* buf, size_t len) {
  snprintf(buf, len,
           "{\"current_a\":%.3f,\"voltage_v\":%.1f,"
           "\"power_va\":%.1f,\"power_w\":%.1f,\"pf\":%.3f,"
           "\"energy_kwh\":%.4f,\"vrms_mv\":%.1f,\"bias_mv\":%.0f,\"mains_hz\":%.1f,"
           "\"ct_vrms_mv\":%.2f,\"ct_bias_mv\":%.0f,\"current_polarity\":%d,"
           "\"win_ms\":%lu,\"windows\":%u,"
           "\"ads_ok\":%s,\"ads_err\":%lu,"
           "\"wifi_rssi_dbm\":%d,\"heap_free\":%u,\"uptime_s\":%lu,"
           "\"source\":\"ads1115+zmapt\"}",
           power::currentA(), power::voltageV(),
           power::apparentVa(), power::realPowerW(), power::powerFactor(),
           energyWh / 1000.0, power::vrmsMillivolts(), power::biasMillivolts(),
           power::measuredMainsHz(), power::ctVrmsMillivolts(),
           power::ctBiasMillivolts(), power::kCurrentPolarity,
           (unsigned long)power::windowDurationMs(),
           (unsigned)power::windowsDoneCount(),
           power::adsPresent() ? "true" : "false",
           (unsigned long)power::adsErrors(),
           (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
           (unsigned long)(millis() / 1000));
}

static void handleMetrics() {
  char buf[512];
  buildMetricsJson(buf, sizeof(buf));
  server.send(200, "application/json", buf);
}

static void handleSamples() {
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  const bool rawCurrent = server.arg("raw") == "1";
  power::recentSamples().writeJson([](const char* chunk) { server.sendContent(chunk); }, rawCurrent);
  server.sendContent("");  // finish the chunked response
}

static void handleRoot() {
  server.send(200, "text/html",
              "<html><head><meta name=\"viewport\" content=\"width=device-width\"></head>"
              "<body><h3>esp-current-sense</h3>"
              "<p><a href=\"/metrics\">/metrics (JSON)</a></p>"
              "<p><a href=\"/samples\">/samples (polarity-corrected arrays, mV)</a>"
              " · <a href=\"/samples?raw=1\">raw ADC arrays</a></p>"
              "<p><a href=\"/i2c\">/i2c (bus scan)</a></p>"
              "<p><a href=\"/update\">/update (firmware OTA)</a> — no authentication; trusted LAN only.</p>"
              "</body></html>");
}

static void handleChangeMQTTUpdateInterval() {
}

// One-shot scan of the I2C bus (GPIO21/22). Lets us tell "no device" from
// "device at an unexpected address" without a serial console.
static void handleI2cScan() {
  char buf[256];
  size_t n = 0;
  uint8_t count = 0;
  bool first = true;
  n += snprintf(buf + n, sizeof(buf) - n, "{\"found\":[");
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      if (!first) n += snprintf(buf + n, sizeof(buf) - n, ",");
      n += snprintf(buf + n, sizeof(buf) - n, "\"0x%02X\"", addr);
      first = false;
      ++count;
    }
  }
  n += snprintf(buf + n, sizeof(buf) - n, "],\"count\":%u}", count);
  server.send(200, "application/json", buf);
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
  Serial.println("\r\n[esp-current-sense] boot — CT (ADS1115) + ZMPT101B + HTTP + MQTT");

  power::setup();
  mqtt::setup();
  connectWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/metrics", HTTP_GET, handleMetrics);
  server.on("/samples", HTTP_GET, handleSamples);
  server.on("/i2c", HTTP_GET, handleI2cScan);
  server.on("/mqtt", HTTP_POST, handleChangeMQTTUpdateInterval);
  // Uses the existing server.handleClient() loop; reboots after a successful
  // upload. Intentionally no credentials for now — do not expose to the Internet.
  httpUpdater.setup(&server, "/update");
  server.onNotFound([]() { server.send(404, "text/plain", "not found\n"); });
  server.begin();
  Serial.println("[http] server listening on :80");
  Serial.println("[ota] /update ready (no authentication; trusted LAN only)");
}

void loop() {
  server.handleClient();
  power::burst(kSampleBurstMs);
  mqtt::loop();

  // Integrate consumed real power -> Wh. Keep HA's total_increasing counter
  // import-only: signed negative power remains visible but cannot decrement it.
  const uint32_t now = millis();
  if (haveLastAccum) {
    const float watts = power::realPowerW();
    if (watts > 0.0f) energyWh += watts * (now - lastAccumMs) / 3600000.0;
  }
  lastAccumMs = now;
  haveLastAccum = true;

  if (now - lastMqttPublish >= kMqttPublishMs) {
    lastMqttPublish = now;
    char buf[512];
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
    Serial.printf("[p] win=%lums Vbias=%.0fmV Vrms=%.1fmV V=%.1f mains=%.1fHz (win=%u)\r\n",
                  (unsigned long)power::windowDurationMs(), power::biasMillivolts(),
                  power::vrmsMillivolts(), power::voltageV(),
                  power::measuredMainsHz(), (unsigned)power::windowsDoneCount());
    Serial.printf("[p] ads=%s Ibias=%.0fmV Irms=%.2fmV I=%.2fA raw=%d..%d err=%lu "
                  "P=%.1fW S=%.1fVA PF=%.3f\r\n",
                  power::adsPresent() ? "ok" : "MISSING", power::ctBiasMillivolts(),
                  power::ctVrmsMillivolts(), power::currentA(),
                  power::ctMinCounts(), power::ctMaxCounts(),
                  (unsigned long)power::adsErrors(),
                  power::realPowerW(), power::apparentVa(), power::powerFactor());
  }

  delay(1);
}
