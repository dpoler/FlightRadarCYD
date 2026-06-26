# ✈️ FlightRadarCYD — Live Aircraft Radar for the ESP32 CYD

A live aircraft radar and flight tracker built for the **ESP32 CYD** (Cheap Yellow Display — ILI9341 320×240 touchscreen). Pulls real-time ADS-B flight data from the **OpenSky Network** and **ADSBDB** and displays it three ways: a radar sweep view centered on your location, a sorted flight list with full details on tap, and a daily statistics screen tracking records and traffic patterns.

---

## 📸 Screenshots

| RADAR Mode | LIST Mode | DETAIL display | STATS Mode | Settings Overlay |
|:---:|:---:|:---:|:---:|:---:|
| ![Radar](RadarDisplay.png) | ![List](ListDisplay.png) | ![Detail](DetailDisplay.png) | ![Stats](StatsDisplay.png) | ![Settings](SettingsOverlay.png) |

*Shows real flights detected including DAL2310, EDV4923, and AAL2808*

---

## 📡 What It Does

FlightRadarCYD connects to your WiFi, fetches live ADS-B state vectors from OpenSky Network, and gives you three display modes to explore the airspace around you. ICAO hex codes are used to pull aircraft types from ADSBDB for the detail screen and stats records.

### 🟢 RADAR Mode

<!-- SCREENSHOT: Replace RadarDisplay.png — ideally shows green (climbing) or orange (descending) dots to demonstrate the full color palette, not just cyan/yellow/gray -->

- You are at the center crosshair `+`
- Aircraft appear as colored dots at their true bearing and distance
- A short heading tick line shows where each aircraft is going
- Three range rings labeled at 33% / 66% / 100% of your configured radius

**Dot color by flight state:**
| Color | Meaning |
|---|---|
| Cyan | High altitude (>10,000 ft AGL) |
| Green | Climbing (≥2 m/s vertical rate, low altitude) |
| Orange/Amber | Descending (≤−2 m/s vertical rate, low altitude) |
| Yellow | Low altitude, level flight |
| Gray | On ground |

**Callsign labels** — aircraft within the inner half of the radar show their callsign. Labels can be toggled on/off in the settings overlay to reduce clutter at busy airports.

**Aircraft trails** — when using an authenticated API key (see below), each aircraft leaves a fading dotted trail of up to 5 previous positions using Catmull-Rom spline interpolation. Trails fade from dim green (oldest) to the aircraft's own color (most recent). Not shown in anonymous mode where 4-minute intervals make trails too sparse to be useful.

### 📊 STATS Mode

<!-- SCREENSHOT: Replace StatsDisplay.png — should show the updated header with location name ("Stats: LOCNAME (24h)") and the "hold STATS to reset" hint on the right -->

A rolling 24-hour summary per location. Stats and records persist across reboots and automatically expire after 24 hours. If you have multiple locations configured, the header shows which location's stats are displayed.

**Long-press the STATS tab** for 2 seconds to reset stats for the current location.

**CHART** (upper right) — unique aircraft seen per clock hour over the last 24 hours. Bars are colored from orange (quiet) to cyan (peak). Legend shows the peak hourly count. X-axis runs from −24h on the left to now on the right.

**COUNTS** (upper left)
- **Current:** aircraft visible right now
- **Max:** peak simultaneous aircraft count at any single refresh
- **Unique:** distinct aircraft seen in the last 24 hours
- **Updates:** total data fetches (session, not 24h)

**RECORDS** — callsign, aircraft type, value, compass direction, and time set:
- **Closest** — nearest approach and bearing
- **Highest** — peak altitude and direction
- **Fastest** — top ground speed and direction
- **Climb** — steepest climb rate (fpm) and direction
- **Descent** — steepest descent rate (fpm) and direction

Aircraft types for records are fetched in the background from ADSBDB and appear as they arrive.

### 📋 LIST Mode

- Closest 10 aircraft sorted by distance, with overflow count shown
- Columns: **CALLSIGN · ALT · SPD · DIST · DIR · HDG · V**
  - ALT in ft or m (or `GND` if on ground)
  - SPD in knots
  - DIST from your location
  - DIR as compass bearing (N / NE / SE / etc.)
  - HDG shows direction of travel as a symbol
  - V shows climb/descent state
- **Tap any row** for a full detail overlay: airline, altitude, speed, bearing, heading, aircraft type. Airline names are matched from an ICAO code table loaded at boot. Aircraft type is a separate ADSBDB call and appears within a couple of seconds.
- Tap again to dismiss.

---

## 🕹️ Navigation

### Footer

The footer always shows four touch zones:

| Zone | Action |
|---|---|
| **RADAR** (left) | Switch to radar view |
| **STATS** (center-left) | Switch to stats view |
| **LIST** (center-right) | Switch to list view |
| **\*** (right) | Open settings overlay |

### BOOT Button

| Press | Action |
|---|---|
| Short press | Cycle through RADAR → STATS → LIST |
| Long press (1s) | Open settings overlay |
| Long press on power-up | Re-open WiFi setup portal |

---

## 🔔 Status Indicator

The **▲** triangle in the header shows fetch status at a glance:
- **Amber** — fetch in progress
- **Cyan** — last fetch succeeded
- **Red** — last fetch failed (e.g. network error)

---

## ⚙️ Setup

### First Boot — WiFi Portal

1. Flash the firmware with PlatformIO (`pio run --target upload`)
2. On first boot, the device opens a WiFi access point: **`FlightRadarCYD_Setup`**
3. Connect your phone or computer to that network and open `192.168.4.1`

| | |
|:---:|:---:|
| ![](<Portal 1.png>) | ![](<Portal 2.png>) |

<!-- SCREENSHOTS: Replace Portal 1.png and Portal 2.png with updated portal photos showing the revised layout (scan radius and hide-ground removed; factory reset button at bottom) -->

4. Configure the following:

**Locations** — up to 4 named locations, each with latitude, longitude, and elevation. Elevation is used to distinguish low-level flights from cruise altitude. Google Maps (right-click a pin) gives lat/lon; [mapcoordinates.net](https://mapcoordinates.net) gives elevation.

**Time Zone** — for correct local times in the header clock and daily midnight reset.

**Distance Units** — metric or imperial. Some aviation units (knots, fpm) are always imperial by convention.

**Display Options** — invert display colors if your CYD variant shows washed-out or inverted colors. Inverted-variant boards typically have better contrast than original boards.

**OpenSky Credentials** — anonymous access gives ~400 requests/day at 4-minute refresh. A free OpenSky account gives ~4,000/day at 30-second refresh and enables aircraft trails. Paste a `credentials.json` file directly or enter Client ID and Client Secret manually.

5. Tap **Save & Connect** — the device connects and begins tracking immediately.

> **Tip:** To re-open the portal at any time, hold the BOOT button while powering on.

> **Factory Reset:** A **Factory Reset** button at the bottom of the portal page erases all settings, locations, and stats and restarts the device into the setup portal. A browser confirmation dialog is required.

---

### On-Device Settings Overlay

Tap **\*** in the footer or long-press the BOOT button to open the settings overlay without leaving the current view.

![Settings Overlay](SettingsOverlay.png)

<!-- SCREENSHOT: SettingsOverlay.png — on-device settings overlay showing all 6 rows: Radius buttons, Units buttons, color-coded Filter row (GND/CLB/DSC/HI/LO), Labels toggle, Location selector, Firmware/OTA row -->

| Setting | Options |
|---|---|
| **Radius** | 25/50/100 km or 15/30/60 mi presets |
| **Units** | km / mi |
| **Filter** | Toggle any combination of GND · CLB · DSC · HI · LO independently |
| **Labels** | Show or hide callsign labels on the radar |
| **Location** | Switch between your saved locations (arrow buttons) |
| **Firmware** | Shows current version; CHECK / UPDATE for OTA firmware updates |

Changing **Radius** (larger) or the **GND filter** triggers an immediate data re-fetch. Switching locations resets the stats view to that location's history.

**Filter categories:**
- **GND** — aircraft on the ground (filtered at fetch time to preserve slots near busy airports)
- **CLB** — climbing (≥2 m/s, low altitude)
- **DSC** — descending (≤−2 m/s, low altitude)
- **HI** — high altitude (>10,000 ft AGL)
- **LO** — low altitude, level flight

Each button is color-coded to match the radar dot color for that category.

---

## 🔄 OTA Firmware Updates

The settings overlay shows the current firmware version and a **CHECK** button. If a newer release is available on GitHub, the button changes to **UPDATE** — tap it to download and flash automatically. The device restarts after a successful update.

---

## 📦 Data Sources

| Source | Endpoint | Auth |
|---|---|---|
| [OpenSky Network](https://opensky-network.org) | `/api/states/all` (bounding box) | Anonymous or OAuth2 |
| [ADSBDB](https://www.adsbdb.com) | `/v0/aircraft/{icao}` | None |
| GitHub Releases | `/releases/latest` | None |

- OpenSky returns ADS-B state vectors: position, altitude, velocity, heading, callsign
- Anonymous: ~400 requests/day, 4-minute refresh
- Authenticated: ~4,000 requests/day, 30-second refresh + aircraft trails

---

## 🛠️ Hardware

| Component | Detail |
|---|---|
| Board | ESP32 CYD (Cheap Yellow Display) |
| Display | ILI9341 320×240 TFT (HWSPI) |
| Touch | XPT2046 resistive (VSPI: CLK=25, MISO=39, MOSI=32, CS=33, IRQ=36) |
| Backlight | GPIO 21 |
| Mode toggle | BOOT button (GPIO 0) |

> **Hardware variants:** Some CYD production batches require display color inversion. The setup portal includes an "Invert display colors" option — enable it if colors look wrong.

---

## 🗂️ Project Structure

```
FlightRadarCYD/
├── src/
│   ├── main.cpp          # Display modes, radar, trails, settings overlay, touch, header/footer
│   ├── Portal.cpp        # Captive portal: WiFi, locations, timezone, credentials, factory reset
│   ├── Stats.cpp         # 24-hour rolling stats, hourly chart, per-location NVS/LittleFS persistence
│   ├── OpenSky.cpp       # OpenSky API fetch, streaming JSON parse, distance filter, auth token
│   ├── OTA.cpp           # GitHub release check and OTA firmware update
│   ├── Airlines.cpp      # Airline name lookup (loads airlines.csv from GitHub at boot)
│   └── ADSBDB.cpp        # Aircraft type lookup (detail tap + stats records)
├── include/
│   ├── Portal.h
│   ├── Stats.h
│   ├── OpenSky.h
│   ├── OTA.h
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

- Origin/destination info is not available — ADSBDB has this data but it's historical and often inaccurate. If you find a reliable free source, please open an issue.
- `airlines.csv` covers US carriers, common international carriers, and charter/flight school/fractional operators. Contributions welcome.
- The anonymous OpenSky limit (400/day) is enforced per IP. Multiple anonymous devices behind the same IP will share the limit.
- Maximum configurable radius is 500 km / 310 mi (OpenSky API limit).
- At very busy airports, ground aircraft can consume fetch slots even with the GND filter off — the filter moves GND exclusion to fetch time to keep airborne aircraft visible.

---

## 🙏 Acknowledgement

Based on Coreymillia's [Flight-CYD-ESP32-Radar](https://github.com/Coreymillia/Flight-CYD-ESP32-Radar).

## 📜 License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).
