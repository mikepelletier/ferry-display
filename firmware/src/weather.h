#pragma once
// Rain forecast (Buienradar, next 2 hours) and current temperature (Open-Meteo)

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>
#include "config.h"
#include "secrets.h"

// Your location goes in secrets.h (kept out of the public repo).
// Without it, the forecast falls back to NDSM.
#ifndef HOME_LAT
#define HOME_LAT 52.40
#endif
#ifndef HOME_LON
#define HOME_LON 4.89
#endif

struct RainData {
  uint8_t value[25];   // raw Buienradar value 0..255 (logarithmic)
  char time[25][6];    // "HH:MM"
  int count = 0;
  bool valid = false;
  uint32_t fetchedAt = 0;
};

struct TempData {
  float celsius = NAN;
  bool valid = false;
  uint32_t fetchedAt = 0;
};

// Buienradar value -> mm/h
inline float rainMmPerHour(uint8_t v) {
  return v == 0 ? 0.0f : powf(10.0f, (v - 109) / 32.0f);
}

// Height in the graph (0..1). The raw values are already logarithmic:
// 70 ≈ 0.06 mm/h (bottom), 109 = 1 mm/h (middle), 141 ≈ 10 mm/h (top)
inline float rainLevel(uint8_t v) {
  if (v <= 70) return 0.0f;
  float l = (v - 70) / 71.0f;
  return l > 1.0f ? 1.0f : l;
}

inline bool weatherGet(const String& url, String& body) {
  WiFiClientSecure client;
  client.setInsecure();  // fine for public weather data
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code == 200) body = http.getString();
  else Serial.printf("Weather fetch failed: HTTP %d (%s)\n", code, url.c_str());
  http.end();
  return code == 200;
}

// Response is plain text, one line per 5 minutes: "077|15:15"
inline bool fetchRain(RainData& out) {
  String url = "https://gpsgadget.buienradar.nl/data/raintext?lat=" +
               String(HOME_LAT, 2) + "&lon=" + String(HOME_LON, 2);
  String body;
  if (!weatherGet(url, body)) return false;

  RainData r;
  int pos = 0;
  while (pos < (int)body.length() && r.count < 25) {
    int eol = body.indexOf('\n', pos);
    if (eol < 0) eol = body.length();
    String line = body.substring(pos, eol);
    line.trim();
    pos = eol + 1;
    int bar = line.indexOf('|');
    if (bar < 1 || (int)line.length() < bar + 6) continue;
    r.value[r.count] = (uint8_t)constrain(line.substring(0, bar).toInt(), 0, 255);
    strlcpy(r.time[r.count], line.substring(bar + 1).c_str(), 6);
    r.count++;
  }
  // Buienradar sometimes answers with an error message instead of data;
  // keep showing the previous forecast in that case.
  if (r.count < 2) {
    Serial.println("Rain: no usable data in response");
    return false;
  }
  r.valid = true;
  r.fetchedAt = millis();
  out = r;
  Serial.printf("Rain: %d points\n", r.count);
  return true;
}

inline bool fetchTemp(TempData& out) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" +
               String(HOME_LAT, 2) + "&longitude=" + String(HOME_LON, 2) +
               "&current=temperature_2m";
  String body;
  if (!weatherGet(url, body)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonVariant t = doc["current"]["temperature_2m"];
  if (!t.is<float>()) return false;
  out.celsius = t.as<float>();
  out.valid = true;
  out.fetchedAt = millis();
  Serial.printf("Temp: %.1f C\n", out.celsius);
  return true;
}

// One-line summary for above the graph, plus the forecast maximum
inline void rainSummary(const RainData& r, char* buf, size_t len, float& maxMm) {
  const uint8_t WET = 77;  // ≈ 0.1 mm/h
  int first = -1;
  maxMm = 0;
  for (int i = 0; i < r.count; i++) {
    if (r.value[i] >= WET && first < 0) first = i;
    float mm = rainMmPerHour(r.value[i]);
    if (mm > maxMm) maxMm = mm;
  }
  if (first < 0) {
    snprintf(buf, len, "Dry next 2h");
  } else if (first == 0) {
    int dry = -1;
    for (int i = 1; i < r.count; i++)
      if (r.value[i] < WET) { dry = i; break; }
    if (dry < 0) snprintf(buf, len, "Rain next 2h");
    else snprintf(buf, len, "Rain until ~%s", r.time[dry]);
  } else {
    snprintf(buf, len, "Rain from ~%s", r.time[first]);
  }
}
