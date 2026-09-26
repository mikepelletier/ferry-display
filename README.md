# Ferry + bus departure board (LilyGO T-Display-S3)

Left: ferry NDSM → Centraal (scheduled). Right: bus 36 Atatürk → Olof Palmeplein (live via OVapi, schedule as fallback).
Buttons: **BOOT** (left) = refresh now, **KEY** (right) = backlight on/off.

```
ferry_timetable.py             builds departures.json from the Dutch GTFS feed
departures.json                next 7 days of scheduled times (updated nightly by GitHub)
.github/workflows/ferry.yml    nightly GitHub Action
firmware/                      PlatformIO project for the ESP32
```

## 1. GitHub (hosts departures.json)

1. Create a **public** repo on github.com (e.g. `ferry-display`), then in this folder:
   ```zsh
   git init && git add . && git commit -m "Ferry display"
   git branch -M main
   git remote add origin https://github.com/YOUR-USERNAME/ferry-display.git
   git push -u origin main
   ```
2. On GitHub: **Actions** tab → *Update departures* → **Run workflow** to test. It runs by itself every night after that.

Data URL for the ESP32:
`https://raw.githubusercontent.com/YOUR-USERNAME/ferry-display/main/departures.json`

## 2. Firmware

```zsh
brew install platformio                       # once
cd firmware
cp include/secrets.h.example include/secrets.h   # then edit WiFi + DATA_URL
pio run -t upload                             # build + flash over USB-C
pio device monitor                            # serial log (Ctrl+C to quit)
```

If upload can't find the board: hold **BOOT**, press and release **RST**, release **BOOT**, and upload again.

## Changing routes

- Scheduled routes: edit `QUERIES` at the top of `ferry_timetable.py`.
- Titles, live bus stop (`BUS_TPC`), refresh intervals: `firmware/include/config.h`.

## Refresh departures.json by hand

```zsh
curl -L -o gtfs-nl.zip http://gtfs.ovapi.nl/nl/gtfs-nl.zip
python3 ferry_timetable.py gtfs-nl.zip 7
```
