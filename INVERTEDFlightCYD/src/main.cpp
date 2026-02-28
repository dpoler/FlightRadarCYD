// FlightCYD - Live Aircraft Radar for CYD (Cheap Yellow Display)
// Data: OpenSky Network (free, no key) — refreshes every 4 minutes
// RADAR mode: mini radar showing aircraft positions relative to your location
// LIST  mode: closest aircraft sorted by distance with detail overlay on tap
// BOOT button: toggle RADAR ↔ LIST  |  Footer touch zones: same
// Setup: first boot opens FlightCYD_Setup AP — enter WiFi + lat/lon + radius

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include "Portal.h"
#include "OpenSky.h"

// ---------------------------------------------------------------------------
// Display — CYD ILI9341 320×240 landscape
// ---------------------------------------------------------------------------
#define GFX_BL 21
Arduino_DataBus *bus = new Arduino_HWSPI(2/*DC*/, 15/*CS*/, 14/*SCK*/, 13/*MOSI*/, 12/*MISO*/);
Arduino_GFX    *gfx = new Arduino_ILI9341(bus, GFX_NOT_DEFINED, 1/*landscape*/);

// ---------------------------------------------------------------------------
// Touch — XPT2046 on VSPI
// ---------------------------------------------------------------------------
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33
#define TOUCH_DEBOUNCE 350

SPIClass touchSPI(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
static unsigned long lastTouchTime = 0;

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
#define HEADER_H   20
#define FOOTER_H   20
#define CONTENT_Y  HEADER_H                          // 20
#define CONTENT_H  (240 - HEADER_H - FOOTER_H)       // 200
#define CONTENT_CX (320 / 2)                         // 160
#define CONTENT_CY (CONTENT_Y + CONTENT_H / 2)       // 120

// Radar: circle fills the 200px tall content area with a small margin
#define RADAR_R    95   // radar circle radius in pixels
// Scale: pixels per km
// (computed at runtime from fc_radius_km)

// ---------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------
#define COL_HEADER_BG  0x0841   // very dark gray
#define COL_FOOTER_BG  0x0841
#define COL_RADAR_BG   0x0020   // very dark green tint
#define COL_RING       0x0200   // dim green ring
#define COL_RING_LABEL 0x0380   // slightly brighter
#define COL_GRID       0x0100   // faint green grid
#define COL_USER       0xFFFF   // white user "+"
#define COL_AC_HIGH    0x07FF   // cyan — airborne > 10 000 ft
#define COL_AC_LOW     0xFFE0   // yellow — airborne ≤ 10 000 ft
#define COL_AC_GND     0x39E7   // gray — on ground
#define COL_SELECTED   0x001F   // blue highlight for selected LIST row
#define COL_TITLE      0x07FF   // cyan title
#define COL_DIM        0x7BEF   // dim gray

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
#define MODE_RADAR 0
#define MODE_LIST  1

static int  fc_mode         = MODE_RADAR;
static int  fc_detail_idx   = -1;          // -1 = no detail overlay
static unsigned long fc_last_fetch = 0;
static bool fc_fetch_ok     = false;
static char fc_last_time[10] = "--:--";    // UTC HH:MM of last successful fetch

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void showStatus(const char *msg) {
  gfx->fillRect(0, 0, gfx->width(), HEADER_H, COL_HEADER_BG);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 6);
  gfx->print(msg);
  Serial.println(msg);
}

// Choose aircraft colour based on altitude / on_ground
static uint16_t acColor(const FlightData &f) {
  if (f.on_ground) return COL_AC_GND;
  if (!isnan(f.alt_m) && f.alt_m <= 3048.0f) return COL_AC_LOW; // ≤ 10 000 ft
  return COL_AC_HIGH;
}

// Convert m/s → knots and m → feet
static float msToKts(float ms) { return ms * 1.94384f; }
static float mToFt(float m)    { return m  * 3.28084f; }

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------
static void drawHeader() {
  gfx->fillRect(0, 0, 320, HEADER_H, COL_HEADER_BG);

  // Title
  gfx->setTextColor(COL_TITLE);
  gfx->setTextSize(1);
  gfx->setCursor(4, 6);
  gfx->print("\x04 FlightCYD");   // ♦ glyph from font (or just use text)

  // Aircraft count
  char buf[32];
  snprintf(buf, sizeof(buf), "%d AC", fc_flight_count);
  gfx->setTextColor(fc_flight_count > 0 ? 0x07E0 : COL_DIM);
  gfx->setCursor(130, 6);
  gfx->print(buf);

  // Last update time
  gfx->setTextColor(COL_DIM);
  snprintf(buf, sizeof(buf), "upd %s UTC", fc_last_time);
  int tw = strlen(buf) * 6;
  gfx->setCursor(320 - tw - 4, 6);
  gfx->print(buf);
}

// ---------------------------------------------------------------------------
// Footer  — [◉ RADAR]  |  [☰ LIST]
// ---------------------------------------------------------------------------
static void drawFooter() {
  int fy = 240 - FOOTER_H;
  gfx->fillRect(0, fy, 320, FOOTER_H, COL_FOOTER_BG);
  gfx->drawFastHLine(0, fy, 320, 0x2104);
  gfx->drawFastVLine(160, fy, FOOTER_H, 0x2104);

  // Left zone — RADAR
  uint16_t lc = (fc_mode == MODE_RADAR) ? COL_TITLE : COL_DIM;
  gfx->setTextColor(lc);
  gfx->setTextSize(1);
  gfx->setCursor(44, fy + 6);
  gfx->print(fc_mode == MODE_RADAR ? "[O] RADAR" : " O  RADAR");

  // Right zone — LIST
  uint16_t rc = (fc_mode == MODE_LIST) ? COL_TITLE : COL_DIM;
  gfx->setTextColor(rc);
  gfx->setCursor(188, fy + 6);
  gfx->print(fc_mode == MODE_LIST ? "[=] LIST" : " =  LIST");
}

// ---------------------------------------------------------------------------
// RADAR display
// ---------------------------------------------------------------------------
static void drawRadar() {
  // Background
  gfx->fillRect(0, CONTENT_Y, 320, CONTENT_H, COL_RADAR_BG);

  float scale = (float)RADAR_R / (float)fc_radius_km;  // px per km
  int cx = CONTENT_CX;
  int cy = CONTENT_CY;

  // Faint cardinal grid lines
  gfx->drawFastHLine(cx - RADAR_R, cy, RADAR_R * 2, COL_GRID);
  gfx->drawFastVLine(cx, cy - RADAR_R, RADAR_R * 2, COL_GRID);

  // Range rings at 33%, 66%, 100%
  for (int ri = 1; ri <= 3; ri++) {
    int rr = (RADAR_R * ri) / 3;
    gfx->drawCircle(cx, cy, rr, ri == 3 ? COL_RING : COL_GRID);
    // Range label
    char rlbl[12];
    snprintf(rlbl, sizeof(rlbl), "%dkm", (fc_radius_km * ri) / 3);
    gfx->setTextColor(COL_RING_LABEL);
    gfx->setTextSize(1);
    gfx->setCursor(cx + rr + 2, cy - 4);
    gfx->print(rlbl);
  }

  // "N" at top of outer ring
  gfx->setTextColor(COL_RING);
  gfx->setCursor(cx - 3, cy - RADAR_R - 10);
  gfx->print("N");

  // User position "+"
  gfx->setTextColor(COL_USER);
  gfx->drawFastHLine(cx - 5, cy, 11, COL_USER);
  gfx->drawFastVLine(cx, cy - 5, 11, COL_USER);

  // Draw aircraft
  for (int i = 0; i < fc_flight_count; i++) {
    const FlightData &f = fc_flights[i];
    float track_rad = f.track * (float)M_PI / 180.0f;
    float bng_rad   = f.bearing * (float)M_PI / 180.0f;

    // Screen position
    int sx = cx + (int)(sinf(bng_rad) * f.dist_km * scale);
    int sy = cy - (int)(cosf(bng_rad) * f.dist_km * scale);

    // Clip to content area
    if (sx < 1 || sx > 318 || sy < CONTENT_Y + 1 || sy > CONTENT_Y + CONTENT_H - 2) continue;

    uint16_t col = acColor(f);

    // Dot
    gfx->fillCircle(sx, sy, 2, col);

    // Heading line (if airborne and track known)
    if (!f.on_ground) {
      int hx = sx + (int)(sinf(track_rad) * 8);
      int hy = sy - (int)(cosf(track_rad) * 8);
      gfx->drawLine(sx, sy, hx, hy, col);
    }

    // Callsign label — only for closer aircraft (within inner ring) to reduce clutter
    if (f.dist_km <= fc_radius_km * 0.5f) {
      gfx->setTextColor(col);
      gfx->setTextSize(1);
      gfx->setCursor(sx + 4, sy - 4);
      gfx->print(f.callsign);
    }
  }

  // "No aircraft" overlay
  if (fc_flight_count == 0 && fc_fetch_ok) {
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    gfx->setCursor(90, cy - 5);
    gfx->print("No aircraft found");
  }
}

// ---------------------------------------------------------------------------
// LIST display — up to 10 closest aircraft, touch row for detail
// ---------------------------------------------------------------------------
#define LIST_ROW_H   17
#define LIST_MAX_ROWS 10

static void drawListRow(int rowIdx, int dataIdx, bool selected) {
  if (dataIdx >= fc_flight_count) return;
  const FlightData &f = fc_flights[dataIdx];
  int ry = CONTENT_Y + 14 + rowIdx * LIST_ROW_H;

  uint16_t bg  = selected ? COL_SELECTED : (rowIdx % 2 == 0 ? 0x0000 : 0x0841);
  uint16_t col = acColor(f);

  gfx->fillRect(0, ry, 320, LIST_ROW_H, bg);

  gfx->setTextColor(col);
  gfx->setTextSize(1);

  // Callsign
  gfx->setCursor(4, ry + 5);
  gfx->print(f.callsign);

  // Altitude
  char buf[48];
  if (f.on_ground) {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(74, ry + 5);
    gfx->print("GND");
  } else if (!isnan(f.alt_m)) {
    snprintf(buf, sizeof(buf), "%5.0fft", mToFt(f.alt_m));
    gfx->setCursor(68, ry + 5);
    gfx->print(buf);
  }

  // Speed
  if (f.vel_ms > 1.0f) {
    snprintf(buf, sizeof(buf), "%3.0fkn", msToKts(f.vel_ms));
    gfx->setTextColor(col);
    gfx->setCursor(138, ry + 5);
    gfx->print(buf);
  }

  // Distance
  snprintf(buf, sizeof(buf), "%4.0fkm", f.dist_km);
  gfx->setTextColor(0xFFE0);
  gfx->setCursor(186, ry + 5);
  gfx->print(buf);

  // Compass direction
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(240, ry + 5);
  gfx->print(fc_compass(f.bearing));

  // Small heading tick "→" approximation using bearing
  gfx->setTextColor(col);
  gfx->setCursor(268, ry + 5);
  const char *arrows[] = { "^", "^>",">" ,"v>","v" ,"v<","<" ,"^<","^" };
  gfx->print(arrows[(int)((f.track + 22.5f) / 45.0f) % 8]);
}

static void drawList() {
  gfx->fillRect(0, CONTENT_Y, 320, CONTENT_H, RGB565_BLACK);

  // Column header bar
  gfx->fillRect(0, CONTENT_Y, 320, 14, COL_HEADER_BG);
  gfx->setTextColor(COL_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(4,   CONTENT_Y + 3); gfx->print("CALLSIGN");
  gfx->setCursor(68,  CONTENT_Y + 3); gfx->print("  ALT");
  gfx->setCursor(138, CONTENT_Y + 3); gfx->print(" SPD");
  gfx->setCursor(186, CONTENT_Y + 3); gfx->print(" DIST");
  gfx->setCursor(240, CONTENT_Y + 3); gfx->print("DIR");
  gfx->setCursor(268, CONTENT_Y + 3); gfx->print("HDG");

  if (fc_flight_count == 0) {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(80, CONTENT_Y + 90);
    gfx->print(fc_fetch_ok ? "No aircraft in range" : "Waiting for data...");
    return;
  }

  int rows = min(fc_flight_count, LIST_MAX_ROWS);
  for (int i = 0; i < rows; i++) {
    drawListRow(i, i, (i == fc_detail_idx));
  }

  if (fc_flight_count > LIST_MAX_ROWS) {
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    int ry = CONTENT_Y + 14 + LIST_MAX_ROWS * LIST_ROW_H;
    gfx->setCursor(4, ry);
    char buf[32];
    snprintf(buf, sizeof(buf), "  + %d more outside view", fc_flight_count - LIST_MAX_ROWS);
    gfx->print(buf);
  }
}

// ---------------------------------------------------------------------------
// Aircraft detail overlay (called over the list after row tap)
// ---------------------------------------------------------------------------
static void drawDetail(int idx) {
  if (idx < 0 || idx >= fc_flight_count) return;
  const FlightData &f = fc_flights[idx];
  uint16_t col = acColor(f);

  // Overlay panel — bottom portion of content area
  int py = CONTENT_Y + CONTENT_H - 76;
  gfx->fillRect(0, py, 320, 76, 0x000A);  // near-black blue tint
  gfx->drawFastHLine(0, py, 320, col);

  char buf[56];
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(4, py + 4);
  gfx->print(f.callsign);

  // Country + ICAO small
  gfx->setTextColor(COL_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(4, py + 24);
  snprintf(buf, sizeof(buf), "%s  [%s]", f.country, f.icao);
  gfx->print(buf);

  // Altitude + speed
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, py + 36);
  if (f.on_ground) {
    gfx->print("ON GROUND");
  } else if (!isnan(f.alt_m)) {
    snprintf(buf, sizeof(buf), "%.0fft  %.0fkn", mToFt(f.alt_m), msToKts(f.vel_ms));
    gfx->print(buf);
  }

  // Distance + heading
  gfx->setCursor(4, py + 48);
  snprintf(buf, sizeof(buf), "%.0fkm  %s  hdg %.0f%c",
           f.dist_km, fc_compass(f.bearing), f.track, 176);
  gfx->print(buf);

  // Dismiss hint
  gfx->setTextColor(0x2104);
  gfx->setCursor(4, py + 60);
  gfx->print("tap again to dismiss");
}

// ---------------------------------------------------------------------------
// Full redraw of current mode
// ---------------------------------------------------------------------------
static void redraw() {
  drawHeader();
  if (fc_mode == MODE_RADAR) drawRadar();
  else                       drawList();
  drawFooter();
  if (fc_mode == MODE_LIST && fc_detail_idx >= 0) drawDetail(fc_detail_idx);
}

// ---------------------------------------------------------------------------
// Fetch + update last-time string
// ---------------------------------------------------------------------------
static void doFetch() {
  showStatus("Fetching aircraft...");
  float uLat = atof(fc_lat);
  float uLon = atof(fc_lon);
  fc_fetch_ok = openSkyFetch(uLat, uLon, (float)fc_radius_km);

  // Update time stamp
  struct tm t;
  if (getLocalTime(&t)) strftime(fc_last_time, sizeof(fc_last_time), "%H:%M", &t);

  fc_last_fetch = millis();
  fc_detail_idx = -1;
  redraw();
}

// ---------------------------------------------------------------------------
// Touch handler
// ---------------------------------------------------------------------------
static void handleTouch(int tx, int ty) {
  int footerY = 240 - FOOTER_H;

  // Footer: switch modes
  if (ty >= footerY) {
    int newMode = (tx < 160) ? MODE_RADAR : MODE_LIST;
    if (newMode != fc_mode) {
      fc_mode       = newMode;
      fc_detail_idx = -1;
      redraw();
    }
    return;
  }

  // LIST mode: tap a row to show/dismiss detail
  if (fc_mode == MODE_LIST && ty >= CONTENT_Y + 14) {
    int rowIdx = (ty - (CONTENT_Y + 14)) / LIST_ROW_H;
    if (rowIdx >= 0 && rowIdx < min(fc_flight_count, LIST_MAX_ROWS)) {
      fc_detail_idx = (fc_detail_idx == rowIdx) ? -1 : rowIdx;
      drawList();
      drawFooter();
      if (fc_detail_idx >= 0) drawDetail(fc_detail_idx);
    }
  }
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("FlightCYD - Live Aircraft Radar");

  // Display init
  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->invertDisplay(true);
  gfx->fillScreen(RGB565_BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // BOOT button (GPIO 0, active LOW)
  pinMode(0, INPUT_PULLUP);

  // Touch init
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

  // Load saved settings
  fcLoadSettings();
  bool showPortal = !fc_has_settings;

  if (!showPortal) {
    showStatus("Hold BOOT to change settings...");
    for (int i = 0; i < 30 && !showPortal; i++) {
      if (digitalRead(0) == LOW) showPortal = true;
      delay(100);
    }
  }

  if (showPortal) {
    fcInitPortal();
    while (!portalDone) { fcRunPortal(); delay(5); }
    fcClosePortal();
  }

  // Connect WiFi
  gfx->fillScreen(RGB565_BLACK);
  WiFi.mode(WIFI_STA);
  WiFi.begin(fc_wifi_ssid, fc_wifi_pass);
  int dots = 0;
  unsigned long wStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - wStart > 30000) {
      char msg[60];
      snprintf(msg, sizeof(msg), "WiFi failed: \"%s\"", fc_wifi_ssid);
      showStatus(msg);
      while (true) delay(1000);
    }
    delay(500);
    char msg[48];
    snprintf(msg, sizeof(msg), "Connecting to WiFi%.*s", (dots % 4) + 1, "....");
    showStatus(msg);
    dots++;
  }
  showStatus("WiFi connected!");

  // NTP
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  delay(600);

  // Draw initial shell so display isn't blank while waiting
  drawHeader();
  if (fc_mode == MODE_RADAR) drawRadar();
  else                       drawList();
  drawFooter();
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {
  // BOOT button: toggle RADAR ↔ LIST
  if (digitalRead(0) == LOW) {
    delay(50);
    if (digitalRead(0) == LOW) {
      fc_mode = (fc_mode == MODE_RADAR) ? MODE_LIST : MODE_RADAR;
      fc_detail_idx = -1;
      redraw();
      while (digitalRead(0) == LOW) delay(10);
    }
  }

  // Auto-fetch on interval
  if (fc_last_fetch == 0 || (millis() - fc_last_fetch) > OPENSKY_INTERVAL) {
    doFetch();
  }

  // Touch
  if (ts.tirqTouched() && ts.touched()) {
    TS_Point p = ts.getPoint();
    unsigned long now = millis();
    if (now - lastTouchTime > TOUCH_DEBOUNCE) {
      lastTouchTime = now;
      int tx = map(p.x, 200, 3900, 0, gfx->width());
      int ty = map(p.y, 240, 3900, 0, gfx->height());
      tx = constrain(tx, 0, gfx->width()  - 1);
      ty = constrain(ty, 0, gfx->height() - 1);
      handleTouch(tx, ty);
    }
  }

  delay(10);
}
