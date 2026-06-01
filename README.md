# ✈️ FlightRadarCYD — Live Aircraft Radar for the ESP32 CYD

A live aircraft radar and flight tracker built for the **ESP32 CYD** (Cheap Yellow Display — ILI9341 320×240 touchscreen). Pulls real-time ADS-B flight data from the **OpenSky Network** and **ADSBDB** and displays it three ways: a radar sweep view centered on your location, a sorted flight list with full details on tap, and a daily statistics screen tracking records and traffic patterns.

---

## 📸 Screenshots

| Setup Portal | RADAR Mode | LIST Mode | DETAIL display | STATS Mode |
|:---:|:---:|:---:|:---:|:---:|
| ![Portal](PortalDisplay.png) | ![Radar](RadarDisplay.png) | ![List](ListDisplay.png) | ![Detail](DetailDisplay.png) | ![Stats](StatsDisplay.png) |

> **📷 Screenshot update needed:** Portal screenshot not yet captured. Radar screenshot should show aircraft trails. Stats screenshot should reflect updated Max label and chart legend.

*Shows real flights detected including SWA2864, SWA3168, and N804HS*

---

## 📡 What It Does

FlightRadarCYD connects to your WiFi, fetches live ADS-B state vectors from OpenSky Network, and gives you three display modes to explore the airspace around you. ICAO hex code is used to pull aircraft type from ADSBDB for the detail screen and stats records.

### 🟢 RADAR Mode
- You are at the center crosshair `+`
- Aircraft appear as colored dots at their true bearing and distance
- A short heading tick line shows where each aircraft is going
- Three range rings labeled at 33% / 66% / 100% of your configured radius
- Aircraft in the inner half of the radar show their callsign label
- **Dot color by altitude:** cyan = high altitude cruise, yellow = low / approach, gray = on ground
- **Aircraft trails** — when using an authenticated API key (30-second refresh), each aircraft leaves a fading dotted trail of up to 5 previous positions, rendered as a smooth Catmull-Rom curve. Trails fade from dim green (oldest) to the aircraft's own color (most recent). Not shown in anonymous mode where 4-minute intervals make trails too sparse to be useful.

### 📊 STATS Mode
A rolling 24-hour summary. Stats and records persist across reboots via NVS and automatically expire after 24 hours — so the screen always shows the most recent activity regardless of when you last powered up.

**CHART** (upper right) — unique aircraft seen per clock hour over the last 24 hours, showing when the airspace around you is busiest. Bars are colored from orange (quiet hours) to cyan (peak hours). The legend shows the peak hourly count as `unique AC/hr (Max: N)`. X-axis runs from -24h on the left to now on the right.

**COUNTS** (upper left)
- **Current:** aircraft visible right now
- **Max:** most aircraft simultaneously visible at any single refresh, and when
- **Unique:** unique aircraft seen in the last 24 hours (rolling)
- **Updates:** number of data fetches since boot

**RECORDS** — each record shows the aircraft callsign, type (fetched in the background from ADSBDB), value + compass direction, and the time it was set:
- **Closest:** nearest aircraft and bearing from your location
- **Highest:** peak altitude and direction
- **Fastest:** top ground speed and direction
- **Climb:** steepest climb rate (fpm) and direction
- **Descent:** steepest descent rate (fpm) and direction

Aircraft types for record holders are fetched automatically in the background — the screen updates as each one arrives without blocking navigation. All stats and records roll on a 24-hour window and persist across reboots.

### 📋 LIST Mode
- Closest 10 aircraft sorted by distance, with overflow count shown
- Columns: **CALLSIGN · ALT · SPD · DIST · DIR · HDG · V**
  - ALT in meters or feet (or `GND` if on ground)
  - SPD in knots
  - DIST from your location
  - DIR as compass bearing (N / NE / SE / etc.)
  - HDG as a symbol showing direction of travel
  - V shows vertical state if the aircraft is climbing or descending
- **Tap any row** for a full detail overlay: Airline, altitude, speed, bearing, heading. Aircraft type will populate next to airline — it takes a couple seconds as it's a separate API call to ADSBDB. Airline names are pulled by matching ICAO codes from a CSV loaded from GitHub on boot.
- Tap again to dismiss

---

## 🛠️ Hardware

| Component | Detail |
|---|---|
| Board | ESP32 CYD (Cheap Yellow Display) |
| Display | ILI9341 320×240 TFT (HWSPI) |
| Touch | XPT2046 resistive (VSPI: CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36) |
| Backlight | GPIO 21 |
| Mode toggle | BOOT button (GPIO 0) |

> **Hardware variants:** Some CYD production batches require display color inversion for correct rendering. The setup portal includes an "Invert display colors" option — check this if colors look wrong on your board. Inverted-variant boards typically have noticeably better color and contrast than original boards.

---

## ⚙️ Setup

1. Flash the firmware with PlatformIO (`pio run --target upload`)
2. On first boot, the device opens a WiFi access point: **`FlightRadarCYD_Setup`**
3. Connect to it and navigate to `192.168.4.1` to access the configuration portal
4. Enter the following settings:

- **WiFi credentials**
- **Latitude**, **Longitude**, and **Elevation** of your location. Elevation is used to distinguish low-level flights from those at cruise altitude.
- **Time Zone** — for correct local times on the stats screen and the daily midnight reset
- **Units** — Metric or Imperial
- **Scan Radius** — choose a preset or enter custom. Inner rings appear at 1/3 and 2/3 of this distance. OpenSky maximum: 500 km / 310 mi.
- **Hide aircraft on ground** — reduces clutter near major airports
- **Invert display colors** — required for some CYD hardware variants; if colors look wrong, enable this
- **OpenSky OAuth2 credentials** — anonymous access allows ~400 queries/day (4-minute refresh). A free OpenSky account increases this to 4,000 queries/day (30-second refresh) and enables aircraft trails on the radar. You can paste a `credentials.json` file directly or enter the fields manually.

5. Tap **Save & Connect** — the device connects and begins tracking immediately

> **Tip:** Hold the BOOT button on power-up at any time to re-open the setup portal and change your settings.

---

## 🔄 Switching Modes

- **BOOT button** — cycles through RADAR → STATS → LIST
- **Footer touch zones** — three equal zones: left = RADAR, center = STATS, right = LIST

---

## 🔔 Status Indicator

The **▲** triangle in the header shows fetch status at a glance:
- **Amber** — fetch in progress
- **Cyan** — last fetch succeeded
- **Red** — last fetch failed (e.g. network error)

---

## 📦 Data Sources

| Source | Endpoint | Auth |
|---|---|---|
| [OpenSky Network](https://opensky-network.org) | `/api/states/all` (bounding box) | Anonymous or OAuth2 |
| [ADSBDB](https://www.adsbdb.com) | `/v0/aircraft/{icao}` | None — anonymous |

- OpenSky Network returns ADS-B state vectors: position, altitude, velocity, heading, callsign
- Anonymous access: ~400 requests/day, 4-minute refresh interval
- Authenticated access: ~4,000 requests/day, 30-second refresh interval — also enables aircraft trails on the radar screen

---

## 🗂️ Project Structure

```
FlightRadarCYD/
├── src/
│   ├── main.cpp          # Display modes, radar geometry, aircraft trails, touch, header
│   ├── Stats.cpp         # 24-hour rolling stats, hourly chart data, LittleFS persistence
│   ├── Portal.cpp        # Captive portal: WiFi, location, timezone, display settings
│   ├── OpenSky.cpp       # OpenSky API fetch, parse, distance sort
│   ├── Airlines.cpp      # Airline name lookup (loads airlines.csv from GitHub at boot)
│   └── ADSBDB.cpp        # Aircraft type lookup (detail tap + stats records)
├── include/
│   ├── Stats.h
│   ├── Portal.h
│   ├── OpenSky.h
│   ├── Airlines.h
│   └── ADSBDB.h
├── airlines.csv          # ICAO airline code → name table
└── platformio.ini
```

---

## 🔧 Build

```bash
pio run --target upload
```

No separate filesystem upload step is required. The LittleFS partition is formatted automatically on first boot and the device writes its own runtime data files.

---

## ⚠️ Known Limitations

- The display is small. Other font options exist but none are wonderful at this size.
- Origin/destination info is not shown — ADSBDB has this data but it's historical and often inaccurate. If you find a reliable free source, please open an issue.
- `airlines.csv` is intentionally incomplete. It covers US carriers, common international carriers, and charter/flight school/fractional operators. Contributions welcome.

## 🙏 Acknowledgement

Based largely on Corelillia's [Flight-CYD-ESP32-Radar](https://github.com/Coreymillia/Flight-CYD-ESP32-Radar). There's a bunch of other cool projects cooking over there.

## 📜 License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).
