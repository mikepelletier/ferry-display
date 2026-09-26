#pragma once
// General settings. Personal settings (WiFi, data URL) live in secrets.h.

// Amsterdam time zone incl. summer time
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"
#define NTP_SERVER_1 "nl.pool.ntp.org"
#define NTP_SERVER_2 "pool.ntp.org"

// Left column: ferry, scheduled times from departures.json
#define FERRY_KEY   "ferry"            // key in departures.json
#define FERRY_TITLE "Pont > Centraal"

// Right column: bus, live from OVapi with departures.json as fallback
#define BUS_KEY   "bus36"              // key in departures.json
#define BUS_TITLE "36 > Olof Palme"
#define BUS_TPC   "30001366"           // OVapi platform code (Ataturk, direction Olof Palmeplein)
#define BUS_LINE  "36"

#define ROWS 4                                        // departures shown per column
#define LIVE_INTERVAL_MS      (30UL * 1000)           // live bus refresh
#define LIVE_MAX_AGE_S        (3 * 60)                // older live data -> fall back to schedule
#define SCHEDULE_INTERVAL_MS  (6UL * 60 * 60 * 1000)  // re-download departures.json
#define SCHEDULE_RETRY_MS     (5UL * 60 * 1000)       // retry after a failed download

// T-Display-S3 pins
#define PIN_POWER_ON 15   // must be HIGH to power the display (needed on battery)
#define PIN_BTN_LEFT 0    // BOOT button: refresh now
#define PIN_BTN_RIGHT 14  // KEY button: backlight on/off
