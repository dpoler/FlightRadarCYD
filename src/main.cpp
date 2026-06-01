// FlightRadarCYD - Live Aircraft Radar for CYD (Cheap Yellow Display)
// Data: OpenSky Network (free, no key) — refreshes every 4 minutes
// RADAR mode: mini radar showing aircraft positions relative to your location
// LIST  mode: closest aircraft sorted by distance with detail overlay on tap
// BOOT button: toggle RADAR ↔ LIST  |  Footer touch zones: same
// Setup: first boot opens FlightRadarCYD_Setup AP — enter WiFi + lat/lon + radius

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include "Portal.h"
#include "OpenSky.h"
#include "ADSBDB.h"
#include "Airlines.h"
#include "Stats.h"

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
#define COL_ACCENT     0xFD20   // orange/amber — section titles, vertical rate values
#define COL_GREEN      0x07E0   // bright green — speed values

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
#define MODE_RADAR    0
#define MODE_STATS    1
#define MODE_LIST     2

static int  fc_mode         = MODE_RADAR;
static int  fc_detail_idx   = -1;          // -1 = no detail overlay
static unsigned long fc_last_fetch = 0;
static bool fc_fetch_ok = false;
static volatile bool fc_fetching   = false;
static volatile bool fc_fetch_done = false;

#define TRAIL_LEN 5
struct TrailEntry { char icao[8]; float bng[TRAIL_LEN]; float dist[TRAIL_LEN]; uint8_t n; uint8_t age; };
static TrailEntry g_trails[MAX_FLIGHTS];


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
  float threshold_m = 3048.0f + fc_elevation_ft * 0.3048f;  // 10 000 ft AGL
  if (!isnan(f.alt_m) && f.alt_m <= threshold_m) return COL_AC_LOW;
  return COL_AC_HIGH;
}

// Convert m/s → knots, m → feet, m/s → feet-per-minute
static float msToKts(float ms) { return ms * 1.94384f; }
static float mToFt(float m)    { return m  * 3.28084f; }
static float msToFpm(float ms) { return ms * 196.85f; }

// Vertical trend arrow with shaft for the V column.
static void drawArrowVert(int cx, int cy, bool up, uint16_t color) {
  if (up) {
    gfx->fillTriangle(cx, cy-5, cx-3, cy, cx+3, cy, color);
    gfx->drawFastVLine(cx, cy, 5, color);
  } else {
    gfx->fillTriangle(cx, cy+5, cx+3, cy, cx-3, cy, color);
    gfx->drawFastVLine(cx, cy-5, 5, color);
  }
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------
static void drawHeader() {
  gfx->fillRect(0, 0, 320, HEADER_H, COL_HEADER_BG);
  char buf[32];

  // Title — symbol color reflects fetch state
  uint16_t symColor = fc_fetching                         ? COL_ACCENT :
                      (fc_fetch_ok || fc_last_fetch == 0) ? COL_TITLE  : RGB565_RED;
  gfx->setTextSize(1);
  gfx->setCursor(4, 6);
  gfx->setTextColor(symColor);
  gfx->print("\x1e");
  gfx->setTextColor(COL_TITLE);
  gfx->print(" FlightRadarCYD");

  // Dual clocks: HHMMZ / HHMM <TZ>
  {
    time_t now  = time(nullptr);
    struct tm utc, loc;
    gmtime_r(&now, &utc);
    getLocalTime(&loc, 0);
    char tz[8];
    strftime(tz, sizeof(tz), "%Z", &loc);
    char zulu[8], local[12];
    snprintf(zulu,  sizeof(zulu),  "%02d%02dZ",   utc.tm_hour, utc.tm_min);
    snprintf(local, sizeof(local), "%02d%02d %s", loc.tm_hour, loc.tm_min, tz[0] ? tz : "LT");
    // center between title (ends ~x=100) and upd text (starts ~x=250)
    int w  = (5 + 3 + (int)strlen(local)) * 6;  // "HHMMZ" + " / " + local
    int cx = 100 + (150 - w) / 2;
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(cx, 6);
    gfx->print(zulu);
    gfx->print(" / ");
    gfx->print(local);
  }

  // Last update — elapsed minutes since last successful fetch
  gfx->setTextColor(COL_DIM);
  if (fc_last_fetch == 0) {
    snprintf(buf, sizeof(buf), "upd --");
  } else {
    int mins = (int)((millis() - fc_last_fetch) / 60000UL);
    if (mins == 0) snprintf(buf, sizeof(buf), "upd <1m ago");
    else           snprintf(buf, sizeof(buf), "upd %dm ago", mins);
  }
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
  gfx->drawFastVLine(107, fy, FOOTER_H, 0x2104);
  gfx->drawFastVLine(214, fy, FOOTER_H, 0x2104);

  gfx->setTextSize(1);

  gfx->setTextColor(fc_mode == MODE_RADAR ? COL_TITLE : COL_DIM);
  gfx->setCursor(26,  fy + 6);
  gfx->print(fc_mode == MODE_RADAR ? "[O] RADAR" : " O  RADAR");

  gfx->setTextColor(fc_mode == MODE_STATS ? COL_TITLE : COL_DIM);
  gfx->setCursor(133, fy + 6);
  gfx->print(fc_mode == MODE_STATS ? "[#] STATS" : " #  STATS");

  gfx->setTextColor(fc_mode == MODE_LIST  ? COL_TITLE : COL_DIM);
  gfx->setCursor(243, fy + 6);
  gfx->print(fc_mode == MODE_LIST  ? "[=] LIST"  : " =  LIST");
}

// ---------------------------------------------------------------------------
static uint16_t lerpColor(uint16_t c1, uint16_t c2, int num, int den) {
  int r = ((c1>>11)&0x1F) + (((int)((c2>>11)&0x1F) - (int)((c1>>11)&0x1F)) * num / den);
  int g = ((c1>>5 )&0x3F) + (((int)((c2>>5 )&0x3F) - (int)((c1>>5 )&0x3F)) * num / den);
  int b = ( c1     &0x1F) + (((int)( c2     &0x1F) - (int)( c1     &0x1F)) * num / den);
  return ((uint16_t)r<<11)|((uint16_t)g<<5)|(uint16_t)b;
}

static void updateTrails() {
  for (int i = 0; i < MAX_FLIGHTS; i++)
    if (g_trails[i].icao[0]) g_trails[i].age++;

  for (int i = 0; i < fc_flight_count; i++) {
    const FlightData &f = fc_flights[i];
    int slot = -1;
    for (int j = 0; j < MAX_FLIGHTS; j++)
      if (strcmp(g_trails[j].icao, f.icao) == 0) { slot = j; break; }
    if (slot < 0)
      for (int j = 0; j < MAX_FLIGHTS; j++)
        if (!g_trails[j].icao[0]) { slot = j; break; }
    if (slot < 0) continue;

    TrailEntry &e = g_trails[slot];
    strncpy(e.icao, f.icao, sizeof(e.icao) - 1);
    e.icao[sizeof(e.icao)-1] = '\0';
    if (e.n == TRAIL_LEN) {
      memmove(e.bng,  e.bng  + 1, (TRAIL_LEN - 1) * sizeof(float));
      memmove(e.dist, e.dist + 1, (TRAIL_LEN - 1) * sizeof(float));
      e.n = TRAIL_LEN - 1;
    }
    e.bng[e.n]  = f.bearing;
    e.dist[e.n] = f.dist_km;
    e.n++;
    e.age = 0;
  }

  for (int i = 0; i < MAX_FLIGHTS; i++)
    if (g_trails[i].icao[0] && g_trails[i].age > 1) {
      g_trails[i].icao[0] = '\0';
      g_trails[i].n = 0;
      g_trails[i].age = 0;
    }
}

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
    if (fc_use_miles)
      snprintf(rlbl, sizeof(rlbl), "%.0fmi", (fc_radius_km * ri) / 3.0f * 0.621371f);
    else
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

  // Trail dots — authenticated mode only (30s fetches make trails meaningful)
  if (fc_client_id[0] != '\0') {
    for (int i = 0; i < fc_flight_count; i++) {
      const FlightData &f = fc_flights[i];
      TrailEntry *e = nullptr;
      for (int j = 0; j < MAX_FLIGHTS; j++)
        if (strcmp(g_trails[j].icao, f.icao) == 0) { e = &g_trails[j]; break; }
      if (!e || e->n == 0) continue;

      uint16_t col = acColor(f);

      // Build screen-space point list: trail history + current position
      float ptx[TRAIL_LEN + 1], pty[TRAIL_LEN + 1];
      int npts = 0;
      for (int k = 0; k < (int)e->n; k++) {
        float tbng = e->bng[k] * (float)M_PI / 180.0f;
        ptx[npts] = cx + sinf(tbng) * e->dist[k] * scale;
        pty[npts] = cy - cosf(tbng) * e->dist[k] * scale;
        npts++;
      }
      float cbng = f.bearing * (float)M_PI / 180.0f;
      ptx[npts] = cx + sinf(cbng) * f.dist_km * scale;
      pty[npts] = cy - cosf(cbng) * f.dist_km * scale;
      npts++;

      // Catmull-Rom spline through stored positions, dot every 4px
      for (int k = 0; k + 1 < npts; k++) {
        int km1 = (k > 0)        ? k - 1 : k;
        int kp2 = (k + 2 < npts) ? k + 2 : k + 1;
        float p0x = ptx[km1], p0y = pty[km1];
        float p1x = ptx[k],   p1y = pty[k];
        float p2x = ptx[k+1], p2y = pty[k+1];
        float p3x = ptx[kp2], p3y = pty[kp2];
        float dx = p2x - p1x, dy = p2y - p1y;
        int steps = max(1, (int)sqrtf(dx*dx + dy*dy) / 4);
        uint16_t dotCol = lerpColor(COL_GRID, col, k + 1, npts);
        for (int s = 0; s < steps; s++) {
          float t = (float)s / steps, t2 = t*t, t3 = t2*t;
          int px = (int)(0.5f * (2*p1x + (-p0x+p2x)*t + (2*p0x-5*p1x+4*p2x-p3x)*t2 + (-p0x+3*p1x-3*p2x+p3x)*t3));
          int py = (int)(0.5f * (2*p1y + (-p0y+p2y)*t + (2*p0y-5*p1y+4*p2y-p3y)*t2 + (-p0y+3*p1y-3*p2y+p3y)*t3));
          if (px < 0 || px >= 320 || py < CONTENT_Y || py >= CONTENT_Y + CONTENT_H) continue;
          gfx->drawPixel(px, py, dotCol);
        }
      }
    }
  }

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
// STATS display
// Layout (CONTENT_Y=20, CONTENT_H=200):
//   +6  "Stats (last 24h)" title  |  +14 divider
//   DIV_X=88: left panel = Current/Peak/Unique/Updates; right panel = unique AC/hr bar chart
//   +15..+106: left: rows at +26/+40/+54/+68  |  right: legend +24, bars ..+94, ticks +96
//   +106 divider  |  +112..+168 five record rows at 14px pitch
// ---------------------------------------------------------------------------
static void drawStats() {
  gfx->fillRect(0, CONTENT_Y, 320, CONTENT_H, 0x0000);
  gfx->setTextSize(1);
  char buf[32];

  // — Header: title + horizontal rule —
  gfx->setTextColor(COL_ACCENT);
  gfx->setCursor(4, CONTENT_Y + 6);
  gfx->print("Stats (last 24h)");
  gfx->drawFastHLine(0, CONTENT_Y + 14, 320, COL_GRID);

  // Two-panel split: left=counts (~1/4), right=chart (~3/4)
  const int DIV_X = 88;
  gfx->drawFastVLine(DIV_X, CONTENT_Y + 15, 91, 0x2104);

  // Left panel: Current / Peak / Unique / Updates
  {
    auto statRow = [&](int y, const char *label, int value) {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(4, y);
      gfx->print(label);
      snprintf(buf, sizeof(buf), " %d", value);
      gfx->setTextColor(COL_TITLE);
      gfx->print(buf);
    };
    statRow(CONTENT_Y + 26, "Current:", fc_flight_count);
    statRow(CONTENT_Y + 40, "Max:",     stats_peak_count);
    statRow(CONTENT_Y + 54, "Unique:",  stats_unique_count);
    statRow(CONTENT_Y + 68, "Updates:", stats_fetch_count);
  }

  // Right panel: bar chart + legend
  {
    const int SPARK_X   = DIV_X + 3;  // x=91
    const int SPARK_BOT = CONTENT_Y + 94;
    const int SPARK_H   = 62;
    const int BAR_STEP  = 9;   // 24 × 9 = 216 px
    const int BAR_W     = 3;

    int cur_h = stats_current_hour >= 0 ? stats_current_hour : 0;

    uint8_t trueMax = 0;
    for (int bar = 0; bar < 24; bar++) {
      int hour_idx = (cur_h + 1 + bar) % 24;
      if (stats_hourly_unique[hour_idx] > trueMax) trueMax = stats_hourly_unique[hour_idx];
    }
    uint8_t maxH = trueMax > 0 ? trueMax : 1;

    gfx->setCursor(SPARK_X, CONTENT_Y + 24);
    gfx->setTextColor(COL_DIM);
    gfx->print("unique AC/hr");
    if (trueMax > 0) {
      gfx->print(" (Max: ");
      gfx->setTextColor(COL_TITLE);
      snprintf(buf, sizeof(buf), "%d", trueMax);
      gfx->print(buf);
      gfx->setTextColor(COL_DIM);
      gfx->print(")");
    }

    gfx->drawFastHLine(SPARK_X, SPARK_BOT, 24 * BAR_STEP, COL_GRID);

    for (int bar = 0; bar < 24; bar++) {
      int hour_idx = (cur_h + 1 + bar) % 24;
      if (stats_hourly_unique[hour_idx] == 0) continue;
      int bx   = SPARK_X + bar * BAR_STEP + 3;  // 3px left gap within step
      int barH = max(1, (int)((long)SPARK_H * stats_hourly_unique[hour_idx] / maxH));
      int val  = stats_hourly_unique[hour_idx];
      // Heatmap: lerp from COL_ACCENT orange (R31 G41 B0) → COL_TITLE cyan (R0 G63 B31)
      int r = (31 * (maxH - val)) / maxH;
      int g = 41 + (22 * val) / maxH;
      int b = (31 * val) / maxH;
      uint16_t col = ((uint16_t)r << 11) | ((uint16_t)g << 5) | (uint16_t)b;
      gfx->fillRect(bx, SPARK_BOT - barH, BAR_W, barH, col);
    }

    // Tick marks + labels: -24h (left), -12h (center), now (right)
    const struct { int bar; const char *lbl; } ticks[] = {
      {0,  "-24h"},
      {11, "-12h"},
      {23, "now" },
    };
    gfx->setTextColor(COL_DIM);
    for (int i = 0; i < 3; i++) {
      int cx = SPARK_X + ticks[i].bar * BAR_STEP + BAR_STEP / 2;
      gfx->drawFastVLine(cx, SPARK_BOT, 2, COL_DIM);
      int lblW = (int)strlen(ticks[i].lbl) * 6;
      int tx = (i == 0) ? cx - 2 : (i == 2) ? cx - lblW + 2 : cx - lblW / 2;
      gfx->setCursor(tx, SPARK_BOT + 2);
      gfx->print(ticks[i].lbl);
    }
  }
  gfx->drawFastHLine(0, CONTENT_Y + 106, 320, COL_GRID);

  // — Records section (no heading — context clear from layout) —
  // Columns: label x=4 | callsign x=60 | type x=120 | value right@276 | time x=286
  auto printRecord = [&](int y, const char *label, const StatRecord &r, const char *value) {
    const uint16_t valColor = COL_TITLE;
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(4, y);
    gfx->print(label);
    if (r.cs[0]) {
      gfx->setTextColor(COL_AC_HIGH);
      gfx->setCursor(60, y);
      gfx->print(r.cs);
      if (r.ac_type[0]) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(120, y);
        gfx->print(r.ac_type);
      }
      gfx->setTextColor(valColor);
      gfx->setCursor(276 - (int)strlen(value) * 6, y);
      gfx->print(value);
      if (r.hhmm[0]) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(286, y);
        gfx->print(r.hhmm);
      }
    } else {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(60, y);
      gfx->print("--");
    }
  };

  if (stats_closest.cs[0]) {
    float d = fc_use_miles ? stats_closest_dist * 0.621371f : stats_closest_dist;
    snprintf(buf, sizeof(buf), "%.1f%s %s", d, fc_use_miles ? "mi" : "km",
             fc_compass(stats_closest.bearing));
  }
  printRecord(CONTENT_Y + 112, "Closest:",  stats_closest, buf);

  if (stats_highest.cs[0])
    snprintf(buf, sizeof(buf), "%dft %s", (int)mToFt(stats_highest_alt),
             fc_compass(stats_highest.bearing));
  printRecord(CONTENT_Y + 126, "Highest:",  stats_highest, buf);

  if (stats_fastest.cs[0])
    snprintf(buf, sizeof(buf), "%dkts %s", (int)msToKts(stats_fastest_spd),
             fc_compass(stats_fastest.bearing));
  printRecord(CONTENT_Y + 140, "Fastest:",  stats_fastest, buf);

  if (stats_climb.cs[0])
    snprintf(buf, sizeof(buf), "%dfpm %s", (int)msToFpm(stats_climb_rate),
             fc_compass(stats_climb.bearing));
  printRecord(CONTENT_Y + 154, "Climb:",    stats_climb,   buf);

  if (stats_desc.cs[0])
    snprintf(buf, sizeof(buf), "%dfpm %s", (int)msToFpm(-stats_desc_rate),
             fc_compass(stats_desc.bearing));
  printRecord(CONTENT_Y + 168, "Descent:",  stats_desc,    buf);
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
  if (fc_use_miles)
    snprintf(buf, sizeof(buf), "%4.0fmi", f.dist_km * 0.621371f);
  else
    snprintf(buf, sizeof(buf), "%4.0fkm", f.dist_km);
  gfx->setTextColor(col);
  gfx->setCursor(186, ry + 5);
  gfx->print(buf);

  // Compass direction
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(240, ry + 5);
  gfx->print(fc_compass(f.bearing));

  // Heading indicator: centre dot + direction line snapped to 45° increments
  static const int8_t hdg_dx[8] = { 0,  5,  6,  5,  0, -5, -6, -5};
  static const int8_t hdg_dy[8] = {-6, -5,  0,  5,  6,  5,  0, -5};
  int dir8 = (int)((f.track + 22.5f) / 45.0f) % 8;
  gfx->fillCircle(280, ry + 8, 2, col);
  gfx->drawLine(280, ry + 8, 280 + hdg_dx[dir8], ry + 8 + hdg_dy[dir8], col);

  // Vertical trend
  if (!f.on_ground && !isnan(f.vert_ms)) {
    if (f.vert_ms >= 2.0f)
      drawArrowVert(307, ry + 8, true,  col);
    else if (f.vert_ms <= -2.0f)
      drawArrowVert(307, ry + 8, false, col);
  }
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
  gfx->setCursor(296, CONTENT_Y + 3); gfx->print("V");

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

  char buf[72];
  gfx->setTextColor(col);
  gfx->setTextSize(2);
  gfx->setCursor(4, py + 4);
  gfx->print(f.callsign);

  // Airline name + aircraft type
  gfx->setTextColor(COL_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(4, py + 24);
  {
    const char *airline  = airlineLookup(f.callsign);
    bool        hasType  = f.ac_type[0]  != '\0';
    bool        hasMaker = f.ac_maker[0] != '\0';
    buf[0] = '\0';
    if (airline && hasType)
      snprintf(buf, sizeof(buf), "%s  %s", airline, f.ac_type);  // drop maker — keeps line short
    else if (airline)
      strncpy(buf, airline, sizeof(buf) - 1);
    else if (hasType && hasMaker)
      snprintf(buf, sizeof(buf), "%s %s", f.ac_maker, f.ac_type);
    else if (hasType)
      strncpy(buf, f.ac_type, sizeof(buf) - 1);
    if (buf[0]) gfx->print(buf);
  }

  // Altitude + speed + vertical trend
  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(4, py + 36);
  if (f.on_ground) {
    gfx->print("ON GROUND");
  } else if (!isnan(f.alt_m)) {
    snprintf(buf, sizeof(buf), "%.0fft  %.0fkn", mToFt(f.alt_m), msToKts(f.vel_ms));
    gfx->print(buf);
    if (!isnan(f.vert_ms)) {
      if (f.vert_ms >= 2.0f)
        drawArrowVert(gfx->getCursorX() + 6, py + 39, true,  RGB565_WHITE);
      else if (f.vert_ms <= -2.0f)
        drawArrowVert(gfx->getCursorX() + 6, py + 39, false, RGB565_WHITE);
    }
  }

  // Distance + heading
  gfx->setCursor(4, py + 48);
  snprintf(buf, sizeof(buf), "%.0f%s  %s  hdg %.0f%c",
           fc_use_miles ? f.dist_km * 0.621371f : f.dist_km,
           fc_use_miles ? "mi" : "km",
           fc_compass(f.bearing), f.track, 248);
  gfx->print(buf);

  // Dismiss hint
  gfx->setTextColor(0x4208);
  gfx->setCursor(4, py + 62);
  gfx->print("tap to dismiss");
}

// ---------------------------------------------------------------------------
// Full redraw of current mode
// ---------------------------------------------------------------------------
static void redraw() {
  drawHeader();
  if      (fc_mode == MODE_RADAR) drawRadar();
  else if (fc_mode == MODE_STATS) drawStats();
  else                            drawList();
  drawFooter();
  if (fc_mode == MODE_LIST && fc_detail_idx >= 0) drawDetail(fc_detail_idx);
}

// ---------------------------------------------------------------------------
// Fetch — runs on core 0 so loop() on core 1 stays responsive
// ---------------------------------------------------------------------------
static void fetchTaskFn(void *) {
  float uLat = atof(fc_lat);
  float uLon = atof(fc_lon);
  fc_fetch_ok = openSkyFetch(uLat, uLon, (float)fc_radius_km, fc_client_id, fc_client_secret);
  if (fc_hide_ground) {
    int n = 0;
    for (int i = 0; i < fc_flight_count; i++)
      if (!fc_flights[i].on_ground) fc_flights[n++] = fc_flights[i];
    fc_flight_count = n;
  }
  fc_fetch_done = true;
  fc_fetching   = false;
  vTaskDelete(NULL);
}

// ---------------------------------------------------------------------------
// Touch handler
// ---------------------------------------------------------------------------
static void handleTouch(int tx, int ty) {
  int footerY      = 240 - FOOTER_H;
  int footerTouchY = footerY - 16;  // extend tap zone upward without moving the visual footer

  // Footer: switch modes
  if (ty >= footerTouchY) {
    int newMode = (tx < 107) ? MODE_RADAR : (tx < 214) ? MODE_STATS : MODE_LIST;
    if (newMode != fc_mode) {
      fc_mode       = newMode;
      fc_detail_idx = -1;
      if (fc_mode == MODE_STATS) stats_fetching_types = true;
      redraw();
    }
    return;
  }

  // LIST mode: tap inside the overlay to dismiss it
  if (fc_mode == MODE_LIST && fc_detail_idx >= 0 && ty >= CONTENT_Y + CONTENT_H - 76) {
    fc_detail_idx = -1;
    drawList();
    drawFooter();
    return;
  }

  // LIST mode: tap a row to show/dismiss detail
  if (fc_mode == MODE_LIST && ty >= CONTENT_Y + 14) {
    int rowIdx = (ty - (CONTENT_Y + 14)) / LIST_ROW_H;
    if (rowIdx >= 0 && rowIdx < min(fc_flight_count, LIST_MAX_ROWS)) {
      fc_detail_idx = (fc_detail_idx == rowIdx) ? -1 : rowIdx;
      drawList();
      drawFooter();
      if (fc_detail_idx >= 0) {
        drawDetail(fc_detail_idx);
        bool updated = adsbdbFetchType(fc_flights[fc_detail_idx]);
        if (updated) drawDetail(fc_detail_idx);
      }
    }
  }
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("FlightRadarCYD - Live Aircraft Radar");

  // Load saved settings before display init so inversion is known
  fcLoadSettings();

  // Display init
  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->invertDisplay(fc_invert_display);
  gfx->fillScreen(RGB565_BLACK);
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);

  // BOOT button (GPIO 0, active LOW)
  pinMode(0, INPUT_PULLUP);

  // Touch init
  touchSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSPI);
  ts.setRotation(1);

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
    gfx->invertDisplay(fc_invert_display);  // apply if changed during portal
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
  showStatus("Loading airline data...");
  airlinesLoad();

  // NTP — gmtOffset/daylightOffset left 0; POSIX TZ string drives local time
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", fc_tz_posix, 1);
  tzset();
  {
    struct tm t;
    int tries = 0;
    while (!getLocalTime(&t, 0) && tries++ < 20) delay(500);  // up to 10s for NTP
  }
  LittleFS.begin(true);
  loadStats();

  // Draw initial shell so display isn't blank while waiting
  drawHeader();
  if      (fc_mode == MODE_RADAR) drawRadar();
  else if (fc_mode == MODE_STATS) drawStats();
  else                            drawList();
  drawFooter();
}

// ---------------------------------------------------------------------------
// loop()
// ---------------------------------------------------------------------------
void loop() {

  // BOOT button: cycle RADAR → STATS → LIST → RADAR
  if (digitalRead(0) == LOW) {
    delay(50);
    if (digitalRead(0) == LOW) {
      fc_mode = (fc_mode + 1) % 3;
      fc_detail_idx = -1;
      if (fc_mode == MODE_STATS) stats_fetching_types = true;
      redraw();
      while (digitalRead(0) == LOW) delay(10);
    }
  }

  // Post-fetch: runs on core 1 (display-safe) — must check before starting a new fetch
  if (fc_fetch_done) {
    fc_fetch_done = false;
    updateStats();
    fc_last_fetch = millis();
    fc_detail_idx = -1;
    redraw();
    if (fc_fetch_ok && fc_client_id[0] != '\0') updateTrails();
  }

  // Kick off background fetch when due
  unsigned long fetchInterval = (fc_client_id[0] != '\0') ? OPENSKY_INTERVAL_AUTH : OPENSKY_INTERVAL_ANON;
  if (!fc_fetching && (fc_last_fetch == 0 || (millis() - fc_last_fetch) > fetchInterval)) {
    fc_fetching   = true;
    fc_fetch_done = false;
    drawHeader();
    xTaskCreatePinnedToCore(fetchTaskFn, "fetch", 8192, NULL, 1, NULL, 0);
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

  // Background type lookup — start task when needed, redraw as types arrive.
  // Task runs on core 0; loop() on core 1 is never blocked.
  if (fc_mode == MODE_STATS && stats_fetching_types) {
    startTypesFetch();
    if (stats_type_arrived) {
      stats_type_arrived = false;
      drawStats();
      drawFooter();
    }
  }

  delay(10);
}
