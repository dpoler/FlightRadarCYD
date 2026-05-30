// FlightRadarCYD - Live Aircraft Radar for CYD (Cheap Yellow Display)
// Data: OpenSky Network (free, no key) — refreshes every 4 minutes
// RADAR mode: mini radar showing aircraft positions relative to your location
// LIST  mode: closest aircraft sorted by distance with detail overlay on tap
// BOOT button: toggle RADAR ↔ LIST  |  Footer touch zones: same
// Setup: first boot opens FlightRadarCYD_Setup AP — enter WiFi + lat/lon + radius

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <XPT2046_Touchscreen.h>
#include "Portal.h"
#include "OpenSky.h"
#include "ADSBDB.h"
#include "Airlines.h"

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
#define MODE_RADAR 0
#define MODE_STATS 1
#define MODE_LIST  2
#define MAX_SEEN   500

static int  fc_mode         = MODE_RADAR;
static int  fc_detail_idx   = -1;          // -1 = no detail overlay
static unsigned long fc_last_fetch = 0;
static bool fc_fetch_ok = false;

// Per-record bundle: callsign, ICAO (for type lookup), cached type, time seen
struct StatRecord {
  char   cs[10];
  char   icao[8];
  char   ac_type[12];
  char   hhmm[6];          // "HH:MM\0"
  float  bearing;
  bool   type_attempted;
  time_t ts;               // epoch seconds when record was set; 0 = empty
};

// Rolling 24h stats
static int        stats_unique_count = 0;
static int        stats_fetch_count  = 0;
static int        stats_peak_count   = 0;
static char       stats_peak_hhmm[6] = {};
static time_t     stats_peak_ts      = 0;
static float      stats_closest_dist = 1e9f;
static StatRecord stats_closest      = {};
static float      stats_highest_alt  = -1e9f;
static StatRecord stats_highest      = {};
static float      stats_fastest_spd  = -1e9f;
static StatRecord stats_fastest      = {};
static float      stats_climb_rate   = -1e9f;   // strongest positive vert_ms
static StatRecord stats_climb        = {};
static float      stats_desc_rate    =  1e9f;   // most negative vert_ms
static StatRecord stats_desc         = {};
static bool         stats_types_pending  = false;
static bool         stats_fetching_types = false;
static volatile bool stats_type_arrived  = false;
static char     stats_seen_icao[MAX_SEEN][7] = {};
static uint32_t stats_seen_ts[MAX_SEEN]      = {};  // epoch when first seen (24h expiry)
static int      stats_seen_count             = 0;
static uint8_t  stats_hourly_unique[24]      = {};  // rolling: unique ICAOs per clock hour
static char     stats_hour_seen_icao[30][7]  = {};  // per-hour ICAO set (temp, cleared on transition)
static int      stats_hour_seen_cnt          = 0;
static int          stats_current_hour   = -1;
static TaskHandle_t hTypeTask            = NULL;

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

// Capture current local time as "HH:MM" into a 6-byte buffer
static void captureTime(char *hhmm6) {
  struct tm t;
  if (getLocalTime(&t, 0)) snprintf(hhmm6, 6, "%02d:%02d", t.tm_hour, t.tm_min);
  else hhmm6[0] = '\0';
}

// New aircraft takes a record — clear type cache so it gets looked up
static void setRecord(StatRecord &r, const FlightData &f) {
  const char *cs = f.callsign[0] ? f.callsign : f.icao;
  strncpy(r.cs,   cs,     sizeof(r.cs)   - 1); r.cs[sizeof(r.cs)-1]     = '\0';
  strncpy(r.icao, f.icao, sizeof(r.icao) - 1); r.icao[sizeof(r.icao)-1] = '\0';
  r.ac_type[0]     = '\0';
  r.bearing        = f.bearing;
  r.type_attempted = false;
  r.ts             = time(nullptr);
  captureTime(r.hhmm);
  stats_types_pending  = true;
  stats_fetching_types = true;  // fetch immediately if on stats, else on next entry
}

// Same aircraft extends its own record — preserve the cached type
static void refreshRecord(StatRecord &r, const FlightData &f) {
  const char *cs = f.callsign[0] ? f.callsign : f.icao;
  strncpy(r.cs, cs, sizeof(r.cs) - 1); r.cs[sizeof(r.cs)-1] = '\0';
  r.bearing = f.bearing;
  r.ts      = time(nullptr);
  captureTime(r.hhmm);
  // icao, ac_type, type_attempted intentionally unchanged
}

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
// Stats persistence — NVS namespace "stats"
// ---------------------------------------------------------------------------
static void expireOldRecords() {
  time_t now = time(nullptr);
  if (now <= 0) return;

  // Expire StatRecord records
  auto expire = [&](StatRecord &r, float &val, float resetVal) {
    if (r.ts > 0 && (now - r.ts) > 86400) { r = {}; val = resetVal; }
  };
  expire(stats_closest, stats_closest_dist, 1e9f);
  expire(stats_highest, stats_highest_alt, -1e9f);
  expire(stats_fastest, stats_fastest_spd, -1e9f);
  expire(stats_climb,   stats_climb_rate,  -1e9f);
  expire(stats_desc,    stats_desc_rate,    1e9f);

  // Expire peak-at-once
  if (stats_peak_ts > 0 && (now - stats_peak_ts) > 86400) {
    stats_peak_count = 0;
    stats_peak_ts    = 0;
    stats_peak_hhmm[0] = '\0';
  }

  // Expire seen ICAOs older than 24h; recompute unique count
  int n = 0;
  for (int i = 0; i < stats_seen_count; i++) {
    if ((now - (time_t)stats_seen_ts[i]) <= 86400) {
      if (i != n) {
        memcpy(stats_seen_icao[n], stats_seen_icao[i], 7);
        stats_seen_ts[n] = stats_seen_ts[i];
      }
      n++;
    }
  }
  stats_seen_count  = n;
  stats_unique_count = n;
}

static void saveStats() {
  Preferences prefs;
  prefs.begin("stats", false);
  prefs.putInt("ver",       3);
  prefs.putInt("cur_hour",  stats_current_hour);
  prefs.putInt("unique",    stats_unique_count);
  prefs.putInt("fetch",     stats_fetch_count);
  prefs.putInt("peak_cnt",  stats_peak_count);
  prefs.putUInt("peak_ts",  (uint32_t)stats_peak_ts);
  prefs.putString("peak_hhmm", stats_peak_hhmm);
  prefs.putFloat("cl_dist", stats_closest_dist);
  prefs.putBytes("cl_rec",  &stats_closest, sizeof(StatRecord));
  prefs.putFloat("hi_alt",  stats_highest_alt);
  prefs.putBytes("hi_rec",  &stats_highest, sizeof(StatRecord));
  prefs.putFloat("fa_spd",  stats_fastest_spd);
  prefs.putBytes("fa_rec",  &stats_fastest, sizeof(StatRecord));
  prefs.putFloat("cb_rate", stats_climb_rate);
  prefs.putBytes("cb_rec",  &stats_climb, sizeof(StatRecord));
  prefs.putFloat("dc_rate", stats_desc_rate);
  prefs.putBytes("dc_rec",  &stats_desc, sizeof(StatRecord));
  prefs.putInt("seen_cnt",  stats_seen_count);
  if (stats_seen_count > 0) {
    prefs.putBytes("seen",    stats_seen_icao, stats_seen_count * 7);
    prefs.putBytes("seen_ts", stats_seen_ts,   stats_seen_count * sizeof(uint32_t));
  }
  prefs.putBytes("hourly_u", stats_hourly_unique, sizeof(stats_hourly_unique));
  prefs.end();
}

static void loadStats() {
  Preferences prefs;
  prefs.begin("stats", true);
  if (prefs.getInt("ver", 0) < 3) { prefs.end(); return; }  // old/missing format — fresh start

  stats_current_hour   = prefs.getInt("cur_hour", -1);
  stats_fetch_count    = prefs.getInt("fetch", 0);
  stats_peak_count     = prefs.getInt("peak_cnt", 0);
  stats_peak_ts        = (time_t)prefs.getUInt("peak_ts", 0);
  String phhmm         = prefs.getString("peak_hhmm", "");
  strncpy(stats_peak_hhmm, phhmm.c_str(), sizeof(stats_peak_hhmm) - 1);
  stats_closest_dist   = prefs.getFloat("cl_dist", 1e9f);
  prefs.getBytes("cl_rec",  &stats_closest, sizeof(StatRecord));
  stats_highest_alt    = prefs.getFloat("hi_alt", -1e9f);
  prefs.getBytes("hi_rec",  &stats_highest, sizeof(StatRecord));
  stats_fastest_spd    = prefs.getFloat("fa_spd", -1e9f);
  prefs.getBytes("fa_rec",  &stats_fastest, sizeof(StatRecord));
  stats_climb_rate     = prefs.getFloat("cb_rate", -1e9f);
  prefs.getBytes("cb_rec",  &stats_climb, sizeof(StatRecord));
  stats_desc_rate      = prefs.getFloat("dc_rate",  1e9f);
  prefs.getBytes("dc_rec",  &stats_desc, sizeof(StatRecord));
  stats_seen_count     = prefs.getInt("seen_cnt", 0);
  if (stats_seen_count > 0) {
    prefs.getBytes("seen",    stats_seen_icao, stats_seen_count * 7);
    prefs.getBytes("seen_ts", stats_seen_ts,   stats_seen_count * sizeof(uint32_t));
  }
  prefs.getBytes("hourly_u", stats_hourly_unique, sizeof(stats_hourly_unique));
  prefs.end();

  expireOldRecords();  // drops anything older than 24h, recomputes stats_unique_count

  Serial.printf("[Stats] Loaded: hour=%d unique=%d fetches=%d\n",
                stats_current_hour, stats_unique_count, stats_fetch_count);
}

// ---------------------------------------------------------------------------
// Stats update
// ---------------------------------------------------------------------------

static void updateStats() {
  expireOldRecords();
  stats_fetch_count++;

  // Peak at once
  if (fc_flight_count > stats_peak_count) {
    stats_peak_count = fc_flight_count;
    stats_peak_ts    = time(nullptr);
    captureTime(stats_peak_hhmm);
  }

  // Hourly unique tracking — detect hour transitions
  struct tm t;
  if (getLocalTime(&t, 0)) {
    if (t.tm_hour != stats_current_hour) {
      stats_hourly_unique[t.tm_hour] = 0;  // clear slot from 24h ago
      stats_hour_seen_cnt = 0;
      stats_current_hour  = t.tm_hour;
    }
  }

  for (int i = 0; i < fc_flight_count; i++) {
    const FlightData &f = fc_flights[i];

    // 24h rolling unique ICAO tracking
    bool seen = false;
    for (int j = 0; j < stats_seen_count; j++)
      if (strcmp(stats_seen_icao[j], f.icao) == 0) { seen = true; break; }
    if (!seen && stats_seen_count < MAX_SEEN) {
      strncpy(stats_seen_icao[stats_seen_count], f.icao, 6);
      stats_seen_icao[stats_seen_count][6] = '\0';
      stats_seen_ts[stats_seen_count] = (uint32_t)time(nullptr);
      stats_seen_count++;
      stats_unique_count++;
    }

    // Per-hour unique ICAO tracking
    if (stats_current_hour >= 0) {
      bool seenHr = false;
      for (int j = 0; j < stats_hour_seen_cnt; j++)
        if (strcmp(stats_hour_seen_icao[j], f.icao) == 0) { seenHr = true; break; }
      if (!seenHr && stats_hour_seen_cnt < 30) {
        strncpy(stats_hour_seen_icao[stats_hour_seen_cnt], f.icao, 6);
        stats_hour_seen_icao[stats_hour_seen_cnt][6] = '\0';
        stats_hour_seen_cnt++;
        if (stats_hourly_unique[stats_current_hour] < 255)
          stats_hourly_unique[stats_current_hour]++;
      }
    }

    // Records — use refreshRecord when same aircraft extends its own record
    #define UPDATE_RECORD(cmp, val, rec, field) \
      if (cmp) { field = val; \
        if (strcmp(rec.icao, f.icao) == 0) refreshRecord(rec, f); \
        else setRecord(rec, f); }

    UPDATE_RECORD(f.dist_km < stats_closest_dist,
                  f.dist_km, stats_closest, stats_closest_dist)
    if (!f.on_ground && !isnan(f.alt_m))
      UPDATE_RECORD(f.alt_m > stats_highest_alt,
                    f.alt_m, stats_highest, stats_highest_alt)
    if (!f.on_ground && !isnan(f.vel_ms))
      UPDATE_RECORD(f.vel_ms > stats_fastest_spd,
                    f.vel_ms, stats_fastest, stats_fastest_spd)
    if (!f.on_ground && !isnan(f.vert_ms)) {
      if (f.vert_ms > 0)
        UPDATE_RECORD(f.vert_ms > stats_climb_rate,
                      f.vert_ms, stats_climb, stats_climb_rate)
      if (f.vert_ms < 0)
        UPDATE_RECORD(f.vert_ms < stats_desc_rate,
                      f.vert_ms, stats_desc,  stats_desc_rate)
    }
    #undef UPDATE_RECORD
  }
  saveStats();
}

// ---------------------------------------------------------------------------
// Background type lookup — runs on core 0 so loop() on core 1 stays responsive
// ---------------------------------------------------------------------------

static void typesFetchTaskFn(void *) {
  StatRecord *recs[] = { &stats_closest, &stats_highest, &stats_fastest,
                         &stats_climb,   &stats_desc };
  for (auto *r : recs) {
    if (!stats_types_pending) break;
    if (!r->icao[0] || r->type_attempted) continue;
    r->type_attempted = true;

    // If another record already fetched the type for this ICAO, copy it
    bool copied = false;
    for (auto *other : recs) {
      if (other != r && strcmp(other->icao, r->icao) == 0 && other->ac_type[0]) {
        strncpy(r->ac_type, other->ac_type, sizeof(r->ac_type) - 1);
        copied = true;
        break;
      }
    }
    if (!copied) {
      FlightData tmp = {};
      strncpy(tmp.icao, r->icao, sizeof(tmp.icao) - 1);
      adsbdbFetchType(tmp, 3000);
      strncpy(r->ac_type, tmp.ac_type, sizeof(r->ac_type) - 1);
    }
    stats_type_arrived = true;
  }
  stats_types_pending  = false;
  stats_fetching_types = false;
  stats_type_arrived   = true;
  hTypeTask = NULL;
  vTaskDelete(NULL);
}

static void startTypesFetch() {
  if (hTypeTask || !stats_types_pending) return;
  xTaskCreatePinnedToCore(typesFetchTaskFn, "typeFetch", 8192, NULL, 1, &hTypeTask, 0);
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
//   +6  "Stats (last 24h)" title  |  +14 divider  |  +8px gap
//   DIV_X=88: left panel = Unique/Updates; right panel = bar chart (3px bars, 9px step)
//   +23..+106: left: counts at +36/+50  |  right: legend +24, bars +32..+94, ticks +96
//   +106 divider  |  +112..+182 six record rows at 14px pitch
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

  // Left panel: Unique / Updates counts
  {
    auto statRow = [&](int y, const char *label, int value) {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(4, y);
      gfx->print(label);
      snprintf(buf, sizeof(buf), " %d", value);
      gfx->setTextColor(COL_TITLE);
      gfx->print(buf);
    };
    statRow(CONTENT_Y + 36, "Unique:", stats_unique_count);
    statRow(CONTENT_Y + 50, "Updates:", stats_fetch_count);
  }

  // Right panel: bar chart + legend
  {
    const int SPARK_X   = DIV_X + 3;  // x=91
    const int SPARK_BOT = CONTENT_Y + 94;
    const int SPARK_H   = 62;
    const int BAR_STEP  = 9;   // 24 × 9 = 216 px
    const int BAR_W     = 3;

    gfx->setTextColor(COL_DIM);
    gfx->setCursor(SPARK_X, CONTENT_Y + 24);
    gfx->print("unique AC/hr");

    int cur_h = stats_current_hour >= 0 ? stats_current_hour : 0;

    uint8_t maxH = 1;
    for (int bar = 0; bar < 24; bar++) {
      int hour_idx = (cur_h + 1 + bar) % 24;
      if (stats_hourly_unique[hour_idx] > maxH) maxH = stats_hourly_unique[hour_idx];
    }

    gfx->drawFastHLine(SPARK_X, SPARK_BOT, 24 * BAR_STEP, COL_GRID);

    for (int bar = 0; bar < 24; bar++) {
      int hour_idx = (cur_h + 1 + bar) % 24;
      if (stats_hourly_unique[hour_idx] == 0) continue;
      int bx   = SPARK_X + bar * BAR_STEP + 3;  // 3px left gap within step
      int barH = max(1, (int)((long)SPARK_H * stats_hourly_unique[hour_idx] / maxH));
      uint16_t col = (stats_hourly_unique[hour_idx] == maxH) ? COL_TITLE : COL_ACCENT;
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

  // Peak traffic
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(4, CONTENT_Y + 182);
  gfx->print("Peak Visible:");
  if (stats_peak_count > 0) {
    snprintf(buf, sizeof(buf), "%d ac", stats_peak_count);
    gfx->setTextColor(COL_TITLE);
    gfx->setCursor(276 - (int)strlen(buf) * 6, CONTENT_Y + 182);
    gfx->print(buf);
    if (stats_peak_hhmm[0]) {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(286, CONTENT_Y + 182);
      gfx->print(stats_peak_hhmm);
    }
  } else {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(60, CONTENT_Y + 182);
    gfx->print("--");
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
// Fetch + update last-time string
// ---------------------------------------------------------------------------
static void doFetch() {
  showStatus("Fetching aircraft...");
  float uLat = atof(fc_lat);
  float uLon = atof(fc_lon);
  fc_fetch_ok = openSkyFetch(uLat, uLon, (float)fc_radius_km, fc_client_id, fc_client_secret);

  if (fc_hide_ground) {
    int n = 0;
    for (int i = 0; i < fc_flight_count; i++)
      if (!fc_flights[i].on_ground) fc_flights[n++] = fc_flights[i];
    fc_flight_count = n;
  }


  updateStats();
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

  // Display init
  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
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

  // Auto-fetch on interval
  unsigned long fetchInterval = (fc_client_id[0] != '\0') ? OPENSKY_INTERVAL_AUTH : OPENSKY_INTERVAL_ANON;
  if (fc_last_fetch == 0 || (millis() - fc_last_fetch) > fetchInterval) {
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
