#include <assert.h>
#include <string.h>

#include "../firmware/adc_samples.h"

static void expectJson(const samples::Buffer& buffer, const char* expected, bool rawCurrent = false) {
  const char* remaining = expected;
  buffer.writeJson([&](const char* chunk) {
    const size_t len = strlen(chunk);
    assert(len < 512);
    assert(strncmp(remaining, chunk, len) == 0);
    remaining += len;
  }, rawCurrent);
  assert(*remaining == '\0');
}

static void testEmpty() {
  samples::Buffer buffer;
  expectJson(buffer, "{\"voltage_mv\":[],\"current_mv\":[]}");
}

static void testPartialAndRepeatedReads() {
  samples::Buffer buffer;
  buffer.record(1650, 1651.25f);
  buffer.record(1702, 1658.5f);
  buffer.record(1751, 1665.75f);
  const char* expected =
      "{\"voltage_mv\":[1650,1702,1751],"
      "\"current_mv\":[1651.250,1658.500,1665.750]}";
  expectJson(buffer, expected, true);
  expectJson(buffer, expected, true);  // fetching must not consume the history
}

static void testAdcLimits() {
  samples::Buffer buffer;
  buffer.record(0, -4096.0f);
  buffer.record(3100, 4095.875f);
  buffer.record(1650, -0.125f);
  expectJson(buffer, "{\"voltage_mv\":[0,3100,1650],"
                     "\"current_mv\":[-4096.000,4095.875,-0.125]}", true);
}

static void testOrdering(size_t total) {
  samples::Buffer buffer;
  for (size_t i = 0; i < total; ++i) {
    buffer.record((uint16_t)i, (float)i + 0.125f);
  }

  const size_t first = total > samples::Buffer::kCapacity
                           ? total - samples::Buffer::kCapacity : 0;
  char expected[8192];
  size_t used = snprintf(expected, sizeof(expected), "{\"voltage_mv\":[");
  for (size_t i = first; i < total; ++i) {
    used += snprintf(expected + used, sizeof(expected) - used, "%s%u",
                     i != first ? "," : "", (unsigned)i);
  }
  used += snprintf(expected + used, sizeof(expected) - used, "],\"current_mv\":[");
  for (size_t i = first; i < total; ++i) {
    used += snprintf(expected + used, sizeof(expected) - used, "%s%u.125",
                     i != first ? "," : "", (unsigned)i);
  }
  used += snprintf(expected + used, sizeof(expected) - used, "]}");
  assert(used < sizeof(expected));
  expectJson(buffer, expected, true);
}

static void testPolarityCorrectedWaveformPreservesBias() {
  samples::Buffer buffer;
  buffer.record(1630, 1612.0f);
  buffer.record(1650, 1610.0f);
  buffer.record(1670, 1608.0f);
  buffer.record(1650, 1610.0f);
  const char* corrected = "{\"voltage_mv\":[1630,1650,1670,1650],"
                          "\"current_mv\":[1608.000,1610.000,1612.000,1610.000]}";
  expectJson(buffer, corrected);
  expectJson(buffer, "{\"voltage_mv\":[1630,1650,1670,1650],"
                     "\"current_mv\":[1612.000,1610.000,1608.000,1610.000]}", true);
  expectJson(buffer, corrected);  // raw reads must not change corrected output
}

static void testPolarityCorrectionUsesTheCurrentRingContents() {
  samples::Buffer buffer;
  for (size_t i = 0; i < samples::Buffer::kCapacity; ++i) buffer.record(1650, 2000.0f);
  // Fully replace the old DC level; none of it should affect the new mean.
  for (size_t i = 0; i < samples::Buffer::kCapacity; ++i) {
    buffer.record(1650, i % 2 ? 1612.0f : 1608.0f);
  }
  char expected[8192];
  size_t used = snprintf(expected, sizeof(expected), "{\"voltage_mv\":[");
  for (size_t i = 0; i < samples::Buffer::kCapacity; ++i) {
    used += snprintf(expected + used, sizeof(expected) - used, "%s1650", i ? "," : "");
  }
  used += snprintf(expected + used, sizeof(expected) - used, "],\"current_mv\":[");
  for (size_t i = 0; i < samples::Buffer::kCapacity; ++i) {
    used += snprintf(expected + used, sizeof(expected) - used, "%s%.3f",
                     i ? "," : "", i % 2 ? 1608.0 : 1612.0);
  }
  used += snprintf(expected + used, sizeof(expected) - used, "]}");
  assert(used < sizeof(expected));
  expectJson(buffer, expected);
}

int main() {
  testEmpty();
  testPartialAndRepeatedReads();
  testAdcLimits();
  testOrdering(256);   // first full buffer
  testOrdering(257);   // first overwrite
  testOrdering(1031);  // multiple wraps; both arrays must remain paired
  testPolarityCorrectedWaveformPreservesBias();
  testPolarityCorrectionUsesTheCurrentRingContents();
  puts("ADC sample buffer/JSON tests passed");
}
