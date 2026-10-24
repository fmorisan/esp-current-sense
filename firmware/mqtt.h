// MQTT client + Home Assistant discovery.
//
// State:    esp-current-sense/state        JSON, every ~5 s (see firmware.ino)
// Availability: esp-current-sense/availability  online/offline, retained,
//           with an LWT so HA marks entities unavailable if the board dies.
// Discovery: homeassistant/sensor/esp-current-sense/{current,power,energy}/config
//           retained, re-published on every (re)connect — HA auto-creates the
//           entities and groups them under one device, zero YAML needed.
//
// Phase 4 runs against a local test broker (see tools/mosquitto-test.conf);
// point MQTT_HOST at the HA box once Mosquitto runs there.
#pragma once

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

#include "secrets.h"

namespace mqtt {

static const char* kStateTopic = HOSTNAME "/state";
static const char* kAvailTopic = HOSTNAME "/availability";
static const uint32_t kRetryMs = 10000;

static WiFiClient s_net;
static PubSubClient s_client(s_net);
static uint32_t s_lastAttempt = 0;

inline void setup() {
  s_client.setServer(MQTT_HOST, MQTT_PORT);
  s_client.setBufferSize(768);  // discovery payloads are ~500 B
}

static bool connect() {
  char clientId[48];
  snprintf(clientId, sizeof(clientId), "%s-%06x", HOSTNAME,
           (unsigned)(ESP.getEfuseMac() & 0xffffff));

  bool ok;
  if (MQTT_USER[0]) {
    ok = s_client.connect(clientId, MQTT_USER, MQTT_PASS, kAvailTopic, 1, true, "offline");
  } else {
    ok = s_client.connect(clientId, nullptr, nullptr, kAvailTopic, 1, true, "offline");
  }
  if (!ok) return false;
  return true;
}

// Retained HA discovery configs — pick fields out of the shared state JSON.
static void publishDiscovery() {
  const char* dev =
      "\"device\":{\"identifiers\":[\"" HOSTNAME "\"],"
      "\"name\":\"Home energy monitor\",\"manufacturer\":\"home-made\","
      "\"model\":\"NodeMCU-32S + SCT-013 100A/1V\"}";

  struct Entity {
    const char* id;
    const char* name;
    const char* field;
    const char* unit;
    const char* deviceClass;
    const char* stateClass;
  };
  static const Entity kEntities[] = {
      {"current", "Home current", "current_a", "A", "current", "measurement"},
      {"power", "Home apparent power", "power_va", "VA", "apparent_power", "measurement"},
      {"energy", "Home energy", "energy_kwh", "kWh", "energy", "total_increasing"},
  };

  char topic[160], payload[640];
  for (const Entity& e : kEntities) {
    snprintf(topic, sizeof(topic), "homeassistant/sensor/%s/%s/config", HOSTNAME, e.id);
    snprintf(payload, sizeof(payload),
             "{\"name\":\"%s\",\"unique_id\":\"%s-%s\",\"state_topic\":\"%s\","
             "\"value_template\":\"{{ value_json.%s }}\",\"unit_of_measurement\":\"%s\","
             "\"device_class\":\"%s\",\"state_class\":\"%s\","
             "\"availability_topic\":\"%s\",%s}",
             e.name, HOSTNAME, e.id, kStateTopic, e.field, e.unit, e.deviceClass,
             e.stateClass, kAvailTopic, dev);
    if (!s_client.publish(topic, payload, true)) {
      Serial.printf("[mqtt] discovery publish failed for %s (state=%d)\r\n", topic,
                    s_client.state());
    }
  }
  s_client.publish(kAvailTopic, "online", true);
}

inline void publishState(const char* json) { s_client.publish(kStateTopic, json, false); }

// Call every loop(): services keepalives, retries broker connection every kRetryMs.
inline void loop() {
  s_client.loop();

  if (s_client.connected() || millis() - s_lastAttempt < kRetryMs) return;
  s_lastAttempt = millis();

  Serial.printf("[mqtt] connecting to %s:%d ", MQTT_HOST, MQTT_PORT);
  if (connect()) {
    Serial.println("ok");
    publishDiscovery();
  } else {
    Serial.printf("failed (rc=%d)\r\n", s_client.state());
  }
}

}  // namespace mqtt
