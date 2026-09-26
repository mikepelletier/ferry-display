// Ferry + bus departure board for the LilyGO T-Display-S3
//  - Ferry NDSM -> Centraal: scheduled times from departures.json (GitHub, cached in flash)
//  - Bus 36 Ataturk -> Olof Palmeplein: live from OVapi, schedule as fallback
// Buttons: BOOT (left) = refresh now, KEY (right) = backlight on/off

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <time.h>
#include <vector>
#include <algorithm>

#include "config.h"
#include "secrets.h"

static const char* CACHE_FILE = "/departures.json";

struct Dep {
  time_t t;        // expected departure
  time_t planned;  // scheduled departure
  String line;
  bool live;
};

TFT_eSPI tft;
TFT_eSprite spr(&tft);
bool useSprite = false;

JsonDocument schedule;
bool haveSchedule = false;
String scheduleStamp;  // "generated" field

std::vector<Dep> liveBus;
time_t liveFetchedAt = 0;

uint32_t lastScheduleTry = 0, lastScheduleOk = 0, lastLiveTry = 0, lastDraw = 0, lastWifiTry = 0;
bool backlightOn = true;
bool forceRefresh = false;

// ---------- helpers ----------

bool timeValid() { return time(nullptr) > 1700000000; }

time_t parseIso(const char* s) {
  struct tm t = {};
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday,
                   &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return 0;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  t.tm_isdst = -1;
  return mktime(&t);
}

String hhmm(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  char b[6];
  strftime(b, sizeof b, "%H:%M", &tm);
  return String(b);
}

// ---------- schedule (departures.json) ----------

bool parseSchedule(const String& body) {
  JsonDocument d;
  DeserializationError err = deserializeJson(d, body);
  if (err) {
    Serial.printf("Schedule JSON error: %s\n", err.c_str());
    return false;
  }
  if (!d[FERRY_KEY].is<JsonObject>()) {
    Serial.println("Schedule JSON has no ferry data");
    return false;
  }
  scheduleStamp = d["generated"] | "";
  schedule = std::move(d);
  haveSchedule = true;
  return true;
}

void loadCachedSchedule() {
  File f = LittleFS.open(CACHE_FILE, "r");
  if (!f) { Serial.println("No cached schedule"); return; }
  String body = f.readString();
  f.close();
  if (parseSchedule(body)) Serial.printf("Loaded cached schedule (%s)\n", scheduleStamp.c_str());
}

bool fetchSchedule() {
  WiFiClientSecure client;
  client.setInsecure();  // fine for public timetable data
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(client, DATA_URL)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Schedule download failed: HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();
  if (!parseSchedule(body)) return false;
  File f = LittleFS.open(CACHE_FILE, "w");
  if (f) { f.print(body); f.close(); }
  Serial.printf("Schedule downloaded (%u bytes, %s)\n", body.length(), scheduleStamp.c_str());
  return true;
}

// Scheduled departures for `key` from yesterday/today/tomorrow (handles times like 24:15)
void collectScheduled(const char* key, time_t now, std::vector<Dep>& out) {
  if (!haveSchedule) return;
  JsonObject days = schedule[key];
  if (days.isNull()) return;
  for (int off = -1; off <= 1; off++) {
    time_t d = now + off * 86400;
    struct tm day;
    localtime_r(&d, &day);
    char k[9];
    strftime(k, sizeof k, "%Y%m%d", &day);
    JsonArray arr = days[(const char*)k];
    for (JsonVariant v : arr) {
      const char* s = v.as<const char*>();
      int hh, mm;
      char line[8] = "";
      if (!s || sscanf(s, "%d:%d %7s", &hh, &mm, line) < 2) continue;
      struct tm t = day;
      t.tm_hour = 0;
      t.tm_min = hh * 60 + mm;
      t.tm_sec = 0;
      t.tm_isdst = -1;
      time_t ts = mktime(&t);
      if (ts >= now - 30) out.push_back({ts, ts, String(line), false});
    }
  }
  std::sort(out.begin(), out.end(), [](const Dep& a, const Dep& b) { return a.t < b.t; });
}

// ---------- live bus (OVapi) ----------

bool fetchLive() {
  HTTPClient http;
  http.setTimeout(8000);
  String url = String("http://v0.ovapi.nl/tpc/") + BUS_TPC;
  if (!http.begin(url)) return false;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Live fetch failed: HTTP %d\n", code);
    http.end();
    return false;
  }
  String body = http.getString();  // handles chunked responses
  http.end();

  JsonDocument filter;
  JsonObject f = filter["*"]["Passes"]["*"].to<JsonObject>();
  f["ExpectedDepartureTime"] = true;
  f["TargetDepartureTime"] = true;
  f["LinePublicNumber"] = true;
  f["TripStatus"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("Live JSON error: %s\n", err.c_str());
    return false;
  }

  time_t now = time(nullptr);
  std::vector<Dep> v;
  for (JsonPair stop : doc.as<JsonObject>()) {
    for (JsonPair pass : stop.value()["Passes"].as<JsonObject>()) {
      JsonObject o = pass.value();
      const char* line = o["LinePublicNumber"] | "";
      const char* status = o["TripStatus"] | "";
      if (strcmp(line, BUS_LINE) != 0) continue;
      if (!strcmp(status, "PASSED") || !strcmp(status, "CANCEL")) continue;
      time_t e = parseIso(o["ExpectedDepartureTime"] | "");
      time_t p = parseIso(o["TargetDepartureTime"] | "");
      if (!e) continue;
      if (!p) p = e;
      if (e >= now - 30) v.push_back({e, p, String(line), true});
    }
  }
  std::sort(v.begin(), v.end(), [](const Dep& a, const Dep& b) { return a.t < b.t; });
  liveBus = v;
  liveFetchedAt = now;
  Serial.printf("Live: %u departures of line %s\n", (unsigned)v.size(), BUS_LINE);
  return true;
}

void currentBus(time_t now, std::vector<Dep>& out) {
  if (liveFetchedAt && now - liveFetchedAt < LIVE_MAX_AGE_S) {
    for (auto& d : liveBus)
      if (d.t >= now - 30) out.push_back(d);
    if (!out.empty()) return;
  }
  collectScheduled(BUS_KEY, now, out);
}

// ---------- drawing ----------

void drawColumn(TFT_eSPI& g, int x, const char* title, uint16_t color,
                const std::vector<Dep>& deps, time_t now, bool showLiveDot, bool isLive) {
  const int w = 160;
  g.setTextDatum(TL_DATUM);
  g.setTextColor(color, TFT_BLACK);
  g.drawString(title, x + 6, 26, 2);
  if (showLiveDot) g.fillCircle(x + w - 10, 34, 4, isLive ? TFT_GREEN : TFT_DARKGREY);
  g.drawFastHLine(x + 4, 44, w - 8, TFT_DARKGREY);

  if (deps.empty()) {
    g.setTextColor(TFT_DARKGREY, TFT_BLACK);
    g.drawString(haveSchedule ? "no departures" : "no data", x + 6, 60, 2);
    return;
  }

  for (int i = 0; i < ROWS && i < (int)deps.size(); i++) {
    const Dep& d = deps[i];
    int y = 50 + i * 30;
    long mins = (long)((d.t - now + 59) / 60);
    if (mins < 0) mins = 0;

    uint16_t tc = mins <= 1 ? TFT_ORANGE : TFT_WHITE;
    g.setTextColor(tc, TFT_BLACK);
    g.setTextDatum(TL_DATUM);
    g.drawString(hhmm(d.t), x + 6, y, 4);

    // delay in red, e.g. +3
    long delay = (long)((d.t - d.planned) / 60);
    if (d.live && delay >= 1) {
      g.setTextColor(TFT_RED, TFT_BLACK);
      g.drawString("+" + String(delay), x + 80, y + 6, 2);
    }

    // minutes until departure, right aligned
    g.setTextDatum(TR_DATUM);
    g.setTextColor(mins <= 1 ? TFT_ORANGE : TFT_LIGHTGREY, TFT_BLACK);
    g.drawString(mins == 0 ? "now" : String(mins) + "'", x + w - 6, y + 6, 2);
  }
}

void drawStatus(const char* line1, const char* line2 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line1, 160, 70, 4);
  tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
  tft.drawString(line2, 160, 105, 2);
}

void drawBoard() {
  time_t now = time(nullptr);
  TFT_eSPI& g = useSprite ? (TFT_eSPI&)spr : tft;
  if (useSprite) spr.fillSprite(TFT_BLACK);
  else tft.fillScreen(TFT_BLACK);

  // header: status left, clock right
  g.setTextDatum(TL_DATUM);
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  g.setTextColor(wifiOk ? TFT_DARKGREY : TFT_RED, TFT_BLACK);
  g.drawString(wifiOk ? "Departures" : "WiFi offline", 6, 3, 2);
  g.setTextDatum(TR_DATUM);
  g.setTextColor(TFT_WHITE, TFT_BLACK);
  g.drawString(hhmm(now), 314, 3, 2);

  std::vector<Dep> ferry, bus;
  collectScheduled(FERRY_KEY, now, ferry);
  currentBus(now, bus);
  bool busLive = !bus.empty() && bus[0].live;

  drawColumn(g, 0, FERRY_TITLE, TFT_CYAN, ferry, now, false, false);
  g.drawFastVLine(160, 24, 146, TFT_DARKGREY);
  drawColumn(g, 160, BUS_TITLE, TFT_YELLOW, bus, now, true, busLive);

  if (useSprite) spr.pushSprite(0, 0);
}

// ---------- setup / loop ----------

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  drawStatus("Connecting WiFi", WIFI_SSID);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
  lastWifiTry = millis();
  Serial.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "not connected");
}

void setup() {
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  pinMode(PIN_BTN_LEFT, INPUT_PULLUP);
  pinMode(PIN_BTN_RIGHT, INPUT_PULLUP);

  Serial.begin(115200);
  delay(200);

  tft.init();
  tft.setRotation(1);  // landscape 320x170, USB on the right
  tft.fillScreen(TFT_BLACK);
  spr.setColorDepth(16);
  useSprite = spr.createSprite(320, 170) != nullptr;  // flicker-free drawing
  Serial.printf("Sprite: %s\n", useSprite ? "yes" : "no (drawing direct)");

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");
  loadCachedSchedule();

  connectWifi();
  configTzTime(TZ_INFO, NTP_SERVER_1, NTP_SERVER_2);
  drawStatus("Getting time", "");
  uint32_t start = millis();
  while (!timeValid() && millis() - start < 15000) delay(200);
}

void loop() {
  uint32_t ms = millis();

  // buttons (simple edge detection)
  static bool lPrev = HIGH, rPrev = HIGH;
  bool l = digitalRead(PIN_BTN_LEFT), r = digitalRead(PIN_BTN_RIGHT);
  if (lPrev == HIGH && l == LOW) forceRefresh = true;
  if (rPrev == HIGH && r == LOW) {
    backlightOn = !backlightOn;
    digitalWrite(TFT_BL, backlightOn ? HIGH : LOW);
  }
  lPrev = l;
  rPrev = r;

  // keep WiFi up
  if (WiFi.status() != WL_CONNECTED && ms - lastWifiTry > 30000) {
    lastWifiTry = ms;
    WiFi.reconnect();
  }

  bool online = WiFi.status() == WL_CONNECTED && timeValid();
  if (online) {
    uint32_t schedWait = (haveSchedule && lastScheduleOk) ? SCHEDULE_INTERVAL_MS : SCHEDULE_RETRY_MS;
    if (forceRefresh || lastScheduleTry == 0 || ms - lastScheduleTry > schedWait) {
      lastScheduleTry = ms;
      if (fetchSchedule()) lastScheduleOk = ms;
    }
    if (forceRefresh || lastLiveTry == 0 || ms - lastLiveTry > LIVE_INTERVAL_MS) {
      lastLiveTry = ms;
      fetchLive();
    }
    forceRefresh = false;
  }

  if (ms - lastDraw > 1000) {
    lastDraw = ms;
    if (timeValid()) drawBoard();
    else drawStatus("Waiting for time", WiFi.status() == WL_CONNECTED ? "NTP..." : "WiFi offline");
  }
  delay(20);
}
