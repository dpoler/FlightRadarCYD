# ✈️ FlightRadarCYD — Live Aircraft Radar for the ESP32 CYD

A live aircraft radar and flight tracker built for the **ESP32 CYD** (Cheap Yellow Display — ILI9341 320×240 touchscreen). Pulls real-time ADS-B flight data from the **OpenSky Network** and **ADSBDB** and displays it two ways: a radar sweep view centered on your location, and a sorted flight list with full details on tap. 

---

## 📸 Screenshots

| RADAR Mode | LIST Mode | DETAIL display |
|:---:|:---:|:---:|
| ![Radar](RadarDisplay.png) | ![List](ListDisplay.png) | ![Detail](DetailDisplay.png)

*Shows real flights detected including SWA2864, SWA3168, and N804HS*

---

## 📡 What It Does

FlightRadarCYD connects to your WiFi, fetches live ADS-B state vectors from OpenSky Network and gives you two display modes to explore the airspace around you. ICAO hex code is used to pull aircraft type from ADSBDB for the detail screen. 

### 🟢 RADAR Mode
- You are at the center crosshair `+`
- Aircraft appear as colored dots at their true bearing and distance
- A short heading tick line shows where each aircraft is going
- Three range rings labeled at 33% / 66% / 100% of your configured radius
- Aircraft in the inner half of the radar show their callsign label
- **Dot color by altitude:** cyan = high altitude cruise, yellow = low / approach, gray = on ground

### 📋 LIST Mode
- Closest 10 aircraft sorted by distance, with overflow count shown
- Columns: **CALLSIGN · ALT · SPD · DIST · DIR · HDG · V**
  - ALT in feet (or `GND` if on ground)
  - SPD in knots
  - DIST from your location
  - DIR as compass bearing (N / NE / SE / etc.)
  - HDG as a symbol showing direction of travel
  - V shows vertical state if the aircraft is climbing or descending.
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

---

## ⚙️ Setup

1. Flash the firmware with PlatformIO
2. On first boot, the device opens a WiFi access point: **`FlightRadarCYD_Setup`**
3. Connect to it and navigate to `192.168.4.1` to access the configuration portal.
4. Enter the following settings:

- **WiFi credentials** 
- **Latitude**, **Longitude**, and **Elevation** of your location, or where you want to track. Elevation is used to distinguish low-level flights vs. those at cruise.
- **Units** - Metric or Imperial
- **Scan Radius** Choose a default or enter custom. Inner rings appear at 1/3 and 2/3 of this distance. The maximum OpenSky will support is 500 km / 310 mi.
- **Hide aircraft on ground** - If you live near a major airport, hiding aircraft not in flight will keep the display from being cluttered.
- **OpenSky OAuth2 credentials** - OpenSky's API will allow 400 anonymous queries per day. If you sign up for a (free) account and supply your credentials here, the limit increases to 4,000 queries per day. You can enter your credentials or simply paste the credentials.json file. See Data Sources section below for more information.

5. Save — device restarts and begins tracking

> **Tip:** Hold the BOOT button on power-up at any time to re-open the setup portal and change your settings.

---

## 🔄 Switching Modes

- **BOOT button** — toggles between RADAR and LIST
- **Footer touch zones** — left half = RADAR, right half = LIST

---

## 📦 Data Source

| Source | Endpoint | Auth |
|---|---|---|
| [OpenSky Network](https://opensky-network.org) | `/api/states/all` (bounding box) | Anonymous or with API key |
| [ADSBDB](https://www.adsbdb.com) | `/v0/aircraft/{icao}` | None — anonymous |

- OpenSky Network returns ADS-B state vectors: position, altitude, velocity, heading, callsign
- Free anonymous access — approximately 400 requests/day
- Signing up for an account is free and increases the limit to 4,000 requests/day. Enter details in the portal.
- If no credentials are provided, 4-minute refresh interval is set, which uses ~360 requests/day.
- If credentials are provided a 30-second refresh interval is set, to take advantage of the increased limit.

---

## 🗂️ Project Structure

```
FlightRadarCYD/
├── src/
│   └── main.cpp          # Display modes, touch, BOOT button, radar geometry
├── include/
│   ├── Portal.h          # Captive portal WiFi + location setup (NVS storage)
│   ├── OpenSky.h         # OpenSky API fetch, parse, distance sort
│   ├── Airlines.h        # Airline name lookup (loads airlines.csv from GitHub at boot)
│   └── ADSBDB.h          # Aircraft type lookup from adsbdb.com (fetched on tap)
├── INVERTEDFlightCYD/    # Inverted display variant (black/white swapped)
├── airlines.csv          # ICAO airline code → name table
└── platformio.ini
```

---

## 🔧 Build

```bash
cd FlightRadarCYD
pio run
pio run --target upload
```

## Known Limitations

- The thing's got a tiny display. Other alternatives exist for fonts but none of them are wonderful. 
- I had originally included origin and destination info for flights, as ADSBDB does have this, but it's historical and wildly inaccurate. I can't find a good (free) source of origin/destination info, if you find one, please let me know.

## 🙏 Acknowledgement

Based largely on Corelillia's [Flight-CYD-ESP32-Radar](https://github.com/Coreymillia/Flight-CYD-ESP32-Radar). There's a bunch of other cool projects cooking over there. 

## 📜 License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).