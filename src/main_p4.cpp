// FlightRadarCYD — JC1060P470C (ESP32-P4 + JD9165 1024x600 MIPI DSI + GT911 touch)

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <time.h>
#include <Arduino_GFX_Library.h>
#include <TAMC_GT911.h>
#include "Portal.h"
#include "OpenSky.h"
#include "ADSBDB.h"
#include "Airlines.h"
#include "Stats.h"

// ---------------------------------------------------------------------------
// Display — JD9165 1024x600 MIPI DSI 2-lane
// Pins: RST=27, Backlight=23
// DSI timing from vendor reference design
// ---------------------------------------------------------------------------
#define LCD_W   1024
#define LCD_H    600
#define LCD_RST   27
#define LCD_BL    23

Arduino_ESP32DSIPanel *dsiPanel = new Arduino_ESP32DSIPanel(
    40 /*hsync_pw*/, 160 /*hsync_bp*/, 160 /*hsync_fp*/,
    10 /*vsync_pw*/,  23 /*vsync_bp*/,  12 /*vsync_fp*/,
    GFX_NOT_DEFINED /*prefer_speed*/, 550 /*lane_bit_rate Mbps*/);

Arduino_GFX *gfx = new Arduino_DSI_Display(
    LCD_W, LCD_H, dsiPanel,
    0 /*rotation*/, true /*auto_flush*/,
    LCD_RST,
    jd9165_init_operations, sizeof(jd9165_init_operations) / sizeof(jd9165_init_operations[0]));

// ---------------------------------------------------------------------------
// Touch — GT911 capacitive, I2C on GPIO7/8, RST=22, INT=21
// GT911 returns coordinates directly in screen space — no calibration needed
// ---------------------------------------------------------------------------
#define TP_SDA  7
#define TP_SCL  8
#define TP_RST 22
#define TP_INT 21
#define TOUCH_DEBOUNCE 350

// BOOT button — GPIO35 on ESP32-P4 (active LOW), used for mode cycling
#define BOOT_BTN 35

// ESP32-C6 WiFi coprocessor reset pin
#define WIFI_C6_RST 54

TAMC_GT911 ts(TP_SDA, TP_SCL, TP_INT, TP_RST, LCD_W, LCD_H);
static unsigned long lastTouchTime = 0;

// ---------------------------------------------------------------------------
// Layout — scaled for 1024x600
// Header and footer are taller than CYD to stay readable on a 7" screen.
// ---------------------------------------------------------------------------
#define HEADER_H    36
#define FOOTER_H    36
#define CONTENT_Y   HEADER_H
#define CONTENT_H   (LCD_H - HEADER_H - FOOTER_H)   // 528
#define CONTENT_CX  (LCD_W / 2)                      // 512
#define CONTENT_CY  (CONTENT_Y + CONTENT_H / 2)      // 300

#define RADAR_R     240   // radar circle radius (fits in 528px tall content area)

// ---------------------------------------------------------------------------
// Colours (same palette as CYD build)
// ---------------------------------------------------------------------------
#define COL_HEADER_BG  0x0841
#define COL_FOOTER_BG  0x0841
#define COL_RADAR_BG   0x0020
#define COL_RING       0x0200
#define COL_RING_LABEL 0x0380
#define COL_GRID       0x0100
#define COL_USER       0xFFFF
#define COL_AC_HIGH    0x07FF
#define COL_AC_LOW     0xFFE0
#define COL_AC_GND     0x39E7
#define COL_SELECTED   0x001F
#define COL_TITLE      0x07FF
#define COL_DIM        0x7BEF
#define COL_ACCENT     0xFD20
#define COL_GREEN      0x07E0

// ---------------------------------------------------------------------------
// App state
// ---------------------------------------------------------------------------
#define MODE_RADAR 0
#define MODE_STATS 1
#define MODE_LIST  2

static int  fc_mode           = MODE_RADAR;
static int  fc_detail_idx     = -1;
static unsigned long fc_last_fetch   = 0;
static unsigned long fc_last_attempt = 0;
static bool fc_fetch_ok        = false;
static bool fc_wifi_connected  = true;
static int  fc_wifi_fail_count = 0;
static volatile bool fc_fetching   = false;
static volatile bool fc_fetch_done = false;

#define TRAIL_LEN 5
struct TrailEntry { char icao[8]; float bng[TRAIL_LEN]; float dist[TRAIL_LEN]; uint8_t n; uint8_t age; };
static TrailEntry g_trails[MAX_FLIGHTS];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static void showStatus(const char *msg) {
  gfx->fillRect(0, 0, LCD_W, HEADER_H, COL_HEADER_BG);
  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(8, 10);
  gfx->print(msg);
  Serial.println(msg);
}

static uint16_t acColor(const FlightData &f) {
  if (f.on_ground) return COL_AC_GND;
  float threshold_m = 3048.0f + fc_elevation_ft * 0.3048f;
  if (!isnan(f.alt_m) && f.alt_m <= threshold_m) return COL_AC_LOW;
  return COL_AC_HIGH;
}

static float msToKts(float ms) { return ms * 1.94384f; }
static float mToFt(float m)    { return m  * 3.28084f; }
static float msToFpm(float ms) { return ms * 196.85f; }

static void drawArrowVert(int cx, int cy, bool up, uint16_t color) {
  if (up) {
    gfx->fillTriangle(cx, cy-8, cx-5, cy, cx+5, cy, color);
    gfx->drawFastVLine(cx, cy, 8, color);
  } else {
    gfx->fillTriangle(cx, cy+8, cx+5, cy, cx-5, cy, color);
    gfx->drawFastVLine(cx, cy-8, 8, color);
  }
}

static uint16_t lerpColor(uint16_t c1, uint16_t c2, int num, int den) {
  int r = ((c1>>11)&0x1F) + (((int)((c2>>11)&0x1F) - (int)((c1>>11)&0x1F)) * num / den);
  int g = ((c1>>5 )&0x3F) + (((int)((c2>>5 )&0x3F) - (int)((c1>>5 )&0x3F)) * num / den);
  int b = ( c1     &0x1F) + (((int)( c2     &0x1F) - (int)( c1     &0x1F)) * num / den);
  return ((uint16_t)r<<11)|((uint16_t)g<<5)|(uint16_t)b;
}

// ---------------------------------------------------------------------------
// Trail tracking (identical logic to CYD build)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------
static void drawHeader() {
  gfx->fillRect(0, 0, LCD_W, HEADER_H, COL_HEADER_BG);

  uint16_t symColor = fc_fetching        ? COL_ACCENT :
                      !fc_wifi_connected ? RGB565_RED  :
                      (fc_fetch_ok || fc_last_fetch == 0) ? COL_TITLE : RGB565_RED;
  gfx->setTextSize(2);
  gfx->setCursor(8, 10);
  gfx->setTextColor(symColor);
  gfx->print("\x1e");
  gfx->setTextColor(COL_TITLE);
  gfx->print(" FlightRadarCYD");

  // Dual clocks
  {
    time_t now = time(nullptr);
    struct tm utc, loc;
    gmtime_r(&now, &utc);
    getLocalTime(&loc, 0);
    char tz[8];
    strftime(tz, sizeof(tz), "%Z", &loc);
    char zulu[8], local[12];
    snprintf(zulu,  sizeof(zulu),  "%02d%02dZ",   utc.tm_hour, utc.tm_min);
    snprintf(local, sizeof(local), "%02d%02d %s", loc.tm_hour, loc.tm_min, tz[0] ? tz : "LT");
    int w  = (5 + 3 + (int)strlen(local)) * 12;  // textSize 2 = 12px wide chars
    int cx = 200 + (400 - w) / 2;
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(cx, 10);
    gfx->print(zulu);
    gfx->print(" / ");
    gfx->print(local);
  }

  // Last update
  char buf[32];
  gfx->setTextColor(COL_DIM);
  if (fc_last_fetch == 0) {
    snprintf(buf, sizeof(buf), "upd --");
  } else {
    int mins = (int)((millis() - fc_last_fetch) / 60000UL);
    if (mins == 0) snprintf(buf, sizeof(buf), "upd <1m ago");
    else           snprintf(buf, sizeof(buf), "upd %dm ago", mins);
  }
  int tw = strlen(buf) * 12;
  gfx->setCursor(LCD_W - tw - 8, 10);
  gfx->print(buf);
}

// ---------------------------------------------------------------------------
// Footer — three equal-width tap zones
// ---------------------------------------------------------------------------
static void drawFooter() {
  int fy = LCD_H - FOOTER_H;
  gfx->fillRect(0, fy, LCD_W, FOOTER_H, COL_FOOTER_BG);
  gfx->drawFastHLine(0, fy, LCD_W, 0x2104);

  int third = LCD_W / 3;   // 341
  gfx->drawFastVLine(third,     fy, FOOTER_H, 0x2104);
  gfx->drawFastVLine(third * 2, fy, FOOTER_H, 0x2104);

  gfx->setTextSize(2);

  gfx->setTextColor(fc_mode == MODE_RADAR ? COL_TITLE : COL_DIM);
  gfx->setCursor(third/2 - 54, fy + 10);
  gfx->print(fc_mode == MODE_RADAR ? "[O] RADAR" : " O  RADAR");

  gfx->setTextColor(fc_mode == MODE_STATS ? COL_TITLE : COL_DIM);
  gfx->setCursor(third + third/2 - 54, fy + 10);
  gfx->print(fc_mode == MODE_STATS ? "[#] STATS" : " #  STATS");

  gfx->setTextColor(fc_mode == MODE_LIST  ? COL_TITLE : COL_DIM);
  gfx->setCursor(third*2 + third/2 - 48, fy + 10);
  gfx->print(fc_mode == MODE_LIST  ? "[=] LIST"  : " =  LIST");
}

// ---------------------------------------------------------------------------
// RADAR display
// ---------------------------------------------------------------------------
static void drawRadar() {
  gfx->fillRect(0, CONTENT_Y, LCD_W, CONTENT_H, COL_RADAR_BG);

  float scale = (float)RADAR_R / (float)fc_radius_km;
  int cx = CONTENT_CX;
  int cy = CONTENT_CY;

  gfx->drawFastHLine(cx - RADAR_R, cy, RADAR_R * 2, COL_GRID);
  gfx->drawFastVLine(cx, cy - RADAR_R, RADAR_R * 2, COL_GRID);

  for (int ri = 1; ri <= 3; ri++) {
    int rr = (RADAR_R * ri) / 3;
    gfx->drawCircle(cx, cy, rr, ri == 3 ? COL_RING : COL_GRID);
    char rlbl[12];
    if (fc_use_miles)
      snprintf(rlbl, sizeof(rlbl), "%.0fmi", (fc_radius_km * ri) / 3.0f * 0.621371f);
    else
      snprintf(rlbl, sizeof(rlbl), "%dkm", (fc_radius_km * ri) / 3);
    gfx->setTextColor(COL_RING_LABEL);
    gfx->setTextSize(2);
    gfx->setCursor(cx + rr + 4, cy - 8);
    gfx->print(rlbl);
  }

  gfx->setTextColor(COL_RING);
  gfx->setTextSize(2);
  gfx->setCursor(cx - 6, cy - RADAR_R - 20);
  gfx->print("N");

  gfx->drawFastHLine(cx - 8, cy, 17, COL_USER);
  gfx->drawFastVLine(cx, cy - 8, 17, COL_USER);

  // Trails (authenticated mode only)
  if (fc_client_id[0] != '\0') {
    for (int i = 0; i < fc_flight_count; i++) {
      const FlightData &f = fc_flights[i];
      TrailEntry *e = nullptr;
      for (int j = 0; j < MAX_FLIGHTS; j++)
        if (strcmp(g_trails[j].icao, f.icao) == 0) { e = &g_trails[j]; break; }
      if (!e || e->n == 0) continue;

      uint16_t col = acColor(f);
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
          if (px < 0 || px >= LCD_W || py < CONTENT_Y || py >= CONTENT_Y + CONTENT_H) continue;
          gfx->drawPixel(px, py, dotCol);
        }
      }
    }
  }

  // Aircraft
  for (int i = 0; i < fc_flight_count; i++) {
    const FlightData &f = fc_flights[i];
    float track_rad = f.track   * (float)M_PI / 180.0f;
    float bng_rad   = f.bearing * (float)M_PI / 180.0f;

    int sx = cx + (int)(sinf(bng_rad) * f.dist_km * scale);
    int sy = cy - (int)(cosf(bng_rad) * f.dist_km * scale);

    if (sx < 2 || sx > LCD_W - 2 || sy < CONTENT_Y + 2 || sy > CONTENT_Y + CONTENT_H - 2) continue;

    uint16_t col = acColor(f);
    gfx->fillCircle(sx, sy, 4, col);

    if (!f.on_ground) {
      int hx = sx + (int)(sinf(track_rad) * 14);
      int hy = sy - (int)(cosf(track_rad) * 14);
      gfx->drawLine(sx, sy, hx, hy, col);
    }

    if (f.dist_km <= fc_radius_km * 0.5f) {
      gfx->setTextColor(col);
      gfx->setTextSize(1);
      gfx->setCursor(sx + 6, sy - 4);
      gfx->print(f.callsign);
    }
  }

  if (fc_flight_count == 0 && fc_fetch_ok) {
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(2);
    gfx->setCursor(cx - 130, cy - 8);
    gfx->print("No aircraft found");
  }
}

// ---------------------------------------------------------------------------
// STATS display
// ---------------------------------------------------------------------------
static void drawStats() {
  gfx->fillRect(0, CONTENT_Y, LCD_W, CONTENT_H, 0x0000);
  gfx->setTextSize(2);
  char buf[32];

  gfx->setTextColor(COL_ACCENT);
  gfx->setCursor(8, CONTENT_Y + 10);
  gfx->print("Stats (last 24h)");
  gfx->drawFastHLine(0, CONTENT_Y + 30, LCD_W, COL_GRID);

  const int DIV_X = 220;
  gfx->drawFastVLine(DIV_X, CONTENT_Y + 31, 220, 0x2104);

  // Left panel: counts
  {
    auto statRow = [&](int y, const char *label, int value) {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(8, y);
      gfx->print(label);
      snprintf(buf, sizeof(buf), " %d", value);
      gfx->setTextColor(COL_TITLE);
      gfx->print(buf);
    };
    statRow(CONTENT_Y + 50,  "Current:", fc_flight_count);
    statRow(CONTENT_Y + 80,  "Max:",     stats_peak_count);
    statRow(CONTENT_Y + 110, "Unique:",  stats_unique_count);
    statRow(CONTENT_Y + 140, "Updates:", stats_fetch_count);
    if (stats_fetch_fail_count > 0)
      statRow(CONTENT_Y + 170, "Errors:", stats_fetch_fail_count);
  }

  // Right panel: bar chart
  {
    const int SPARK_X   = DIV_X + 8;
    const int SPARK_BOT = CONTENT_Y + 230;
    const int SPARK_H   = 160;
    const int BAR_STEP  = (LCD_W - SPARK_X - 8) / 24;  // ~33px per hour
    const int BAR_W     = BAR_STEP - 2;

    int cur_h = stats_current_hour >= 0 ? stats_current_hour : 0;
    uint8_t trueMax = 0;
    for (int bar = 0; bar < 24; bar++) {
      int h = (cur_h + 1 + bar) % 24;
      if (stats_hourly_unique[h] > trueMax) trueMax = stats_hourly_unique[h];
    }
    uint8_t maxH = trueMax > 0 ? trueMax : 1;

    gfx->setCursor(SPARK_X, CONTENT_Y + 36);
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
      int h = (cur_h + 1 + bar) % 24;
      if (stats_hourly_unique[h] == 0) continue;
      int bx   = SPARK_X + bar * BAR_STEP + 2;
      int barH = max(1, (int)((long)SPARK_H * stats_hourly_unique[h] / maxH));
      int val  = stats_hourly_unique[h];
      int r = (31 * (maxH - val)) / maxH;
      int g = 41 + (22 * val) / maxH;
      int b = (31 * val) / maxH;
      uint16_t col = ((uint16_t)r << 11) | ((uint16_t)g << 5) | (uint16_t)b;
      gfx->fillRect(bx, SPARK_BOT - barH, BAR_W, barH, col);
    }

    const struct { int bar; const char *lbl; } ticks[] = { {0,"-24h"}, {11,"-12h"}, {23,"now"} };
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    for (int i = 0; i < 3; i++) {
      int tcx = SPARK_X + ticks[i].bar * BAR_STEP + BAR_STEP / 2;
      gfx->drawFastVLine(tcx, SPARK_BOT, 3, COL_DIM);
      int lblW = (int)strlen(ticks[i].lbl) * 6;
      int tx = (i == 0) ? tcx - 2 : (i == 2) ? tcx - lblW + 2 : tcx - lblW / 2;
      gfx->setCursor(tx, SPARK_BOT + 4);
      gfx->print(ticks[i].lbl);
    }
  }

  gfx->drawFastHLine(0, CONTENT_Y + 256, LCD_W, COL_GRID);

  // Records section — two columns to use the wide screen
  // Left column: Closest, Highest, Fastest   Right column: Climb, Descent
  auto printRecord = [&](int x, int y, const char *label, const StatRecord &r, const char *value) {
    gfx->setTextSize(2);
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(x, y);
    gfx->print(label);
    if (r.cs[0]) {
      gfx->setTextColor(COL_AC_HIGH);
      gfx->setCursor(x + 132, y);
      gfx->print(r.cs);
      if (r.ac_type[0]) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(x + 264, y);
        gfx->print(r.ac_type);
      }
      gfx->setTextColor(COL_TITLE);
      int vw = (int)strlen(value) * 12;
      gfx->setCursor(x + 480 - vw, y);
      gfx->print(value);
      if (r.hhmm[0]) {
        gfx->setTextColor(COL_DIM);
        gfx->setCursor(x + 492, y);
        gfx->print(r.hhmm);
      }
    } else {
      gfx->setTextColor(COL_DIM);
      gfx->setCursor(x + 132, y);
      gfx->print("--");
    }
  };

  int ry = CONTENT_Y + 270;
  int col2x = LCD_W / 2 + 4;

  if (stats_closest.cs[0]) {
    float d = fc_use_miles ? stats_closest_dist * 0.621371f : stats_closest_dist;
    snprintf(buf, sizeof(buf), "%.1f%s %s", d, fc_use_miles ? "mi" : "km", fc_compass(stats_closest.bearing));
  }
  printRecord(8, ry,       "Close:",  stats_closest, buf);

  if (stats_highest.cs[0])
    snprintf(buf, sizeof(buf), "%dft %s", (int)mToFt(stats_highest_alt), fc_compass(stats_highest.bearing));
  printRecord(8, ry + 36,  "High:",   stats_highest, buf);

  if (stats_fastest.cs[0])
    snprintf(buf, sizeof(buf), "%dkts %s", (int)msToKts(stats_fastest_spd), fc_compass(stats_fastest.bearing));
  printRecord(8, ry + 72,  "Fast:",   stats_fastest, buf);

  if (stats_climb.cs[0])
    snprintf(buf, sizeof(buf), "%dfpm %s", (int)msToFpm(stats_climb_rate), fc_compass(stats_climb.bearing));
  printRecord(col2x, ry,  "Climb:",  stats_climb, buf);

  if (stats_desc.cs[0])
    snprintf(buf, sizeof(buf), "%dfpm %s", (int)msToFpm(-stats_desc_rate), fc_compass(stats_desc.bearing));
  printRecord(col2x, ry + 36, "Desc:", stats_desc, buf);
}

// ---------------------------------------------------------------------------
// LIST display — up to 20 rows (more than CYD's 10, screen is taller)
// ---------------------------------------------------------------------------
#define LIST_ROW_H    26
#define LIST_MAX_ROWS 18

static void drawListRow(int rowIdx, int dataIdx, bool selected) {
  if (dataIdx >= fc_flight_count) return;
  const FlightData &f = fc_flights[dataIdx];
  int ry = CONTENT_Y + 28 + rowIdx * LIST_ROW_H;

  uint16_t bg  = selected ? COL_SELECTED : (rowIdx % 2 == 0 ? 0x0000 : 0x0841);
  uint16_t col = acColor(f);
  gfx->fillRect(0, ry, LCD_W, LIST_ROW_H, bg);
  gfx->setTextColor(col);
  gfx->setTextSize(2);

  // Callsign
  gfx->setCursor(8, ry + 5);
  gfx->print(f.callsign);

  // Altitude
  char buf[48];
  if (f.on_ground) {
    gfx->setTextColor(COL_DIM);
    gfx->setCursor(180, ry + 5);
    gfx->print("GND");
  } else if (!isnan(f.alt_m)) {
    snprintf(buf, sizeof(buf), "%5.0fft", mToFt(f.alt_m));
    gfx->setCursor(156, ry + 5);
    gfx->print(buf);
  }

  // Speed
  if (f.vel_ms > 1.0f) {
    snprintf(buf, sizeof(buf), "%3.0fkn", msToKts(f.vel_ms));
    gfx->setTextColor(col);
    gfx->setCursor(360, ry + 5);
    gfx->print(buf);
  }

  // Distance
  if (fc_use_miles)
    snprintf(buf, sizeof(buf), "%4.0fmi", f.dist_km * 0.621371f);
  else
    snprintf(buf, sizeof(buf), "%4.0fkm", f.dist_km);
  gfx->setTextColor(col);
  gfx->setCursor(516, ry + 5);
  gfx->print(buf);

  // Direction
  gfx->setTextColor(COL_DIM);
  gfx->setCursor(672, ry + 5);
  gfx->print(fc_compass(f.bearing));

  // Heading dot + line
  static const int8_t hdg_dx[8] = { 0,  8,  10,  8,  0, -8,-10, -8};
  static const int8_t hdg_dy[8] = {-10,-8,   0,  8, 10,  8,  0, -8};
  int dir8 = (int)((f.track + 22.5f) / 45.0f) % 8;
  gfx->fillCircle(780, ry + 13, 3, col);
  gfx->drawLine(780, ry + 13, 780 + hdg_dx[dir8], ry + 13 + hdg_dy[dir8], col);

  // Vertical trend
  if (!f.on_ground && !isnan(f.vert_ms)) {
    if (f.vert_ms >= 2.0f)       drawArrowVert(840, ry + 13, true,  col);
    else if (f.vert_ms <= -2.0f) drawArrowVert(840, ry + 13, false, col);
  }
}

static void drawList() {
  gfx->fillRect(0, CONTENT_Y, LCD_W, CONTENT_H, RGB565_BLACK);

  // Column header bar
  gfx->fillRect(0, CONTENT_Y, LCD_W, 28, COL_HEADER_BG);
  gfx->setTextColor(COL_DIM);
  gfx->setTextSize(1);
  gfx->setCursor(8,   CONTENT_Y + 8); gfx->print("CALLSIGN");
  gfx->setCursor(156, CONTENT_Y + 8); gfx->print("    ALT");
  gfx->setCursor(360, CONTENT_Y + 8); gfx->print(" SPD");
  gfx->setCursor(516, CONTENT_Y + 8); gfx->print(" DIST");
  gfx->setCursor(672, CONTENT_Y + 8); gfx->print("DIR");
  gfx->setCursor(756, CONTENT_Y + 8); gfx->print("HDG");
  gfx->setCursor(820, CONTENT_Y + 8); gfx->print("V");

  if (fc_flight_count == 0) {
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(2);
    gfx->setCursor(200, CONTENT_Y + CONTENT_H / 2 - 8);
    gfx->print(fc_fetch_ok ? "No aircraft in range" : "Waiting for data...");
    return;
  }

  int rows = min(fc_flight_count, LIST_MAX_ROWS);
  for (int i = 0; i < rows; i++)
    drawListRow(i, i, (i == fc_detail_idx));

  if (fc_flight_count > LIST_MAX_ROWS) {
    gfx->setTextColor(COL_DIM);
    gfx->setTextSize(1);
    int ry = CONTENT_Y + 28 + LIST_MAX_ROWS * LIST_ROW_H;
    char buf[48];
    snprintf(buf, sizeof(buf), "  + %d more outside view", fc_flight_count - LIST_MAX_ROWS);
    gfx->setCursor(8, ry + 4);
    gfx->print(buf);
  }
}

// ---------------------------------------------------------------------------
// Detail overlay
// ---------------------------------------------------------------------------
static void drawDetail(int idx) {
  if (idx < 0 || idx >= fc_flight_count) return;
  const FlightData &f = fc_flights[idx];
  uint16_t col = acColor(f);

  int py = CONTENT_Y + CONTENT_H - 140;
  gfx->fillRect(0, py, LCD_W, 140, 0x000A);
  gfx->drawFastHLine(0, py, LCD_W, col);

  char buf[72];
  gfx->setTextColor(col);
  gfx->setTextSize(3);
  gfx->setCursor(8, py + 8);
  gfx->print(f.callsign);

  gfx->setTextColor(COL_DIM);
  gfx->setTextSize(2);
  gfx->setCursor(8, py + 48);
  {
    const AirlineEntry *airline = airlineLookup(f.callsign);
    bool hasType  = f.ac_type[0]  != '\0';
    bool hasMaker = f.ac_maker[0] != '\0';
    buf[0] = '\0';
    if (airline) {
      bool hasCs = airline->callsign[0] != '\0';
      if (hasCs && hasType)
        snprintf(buf, sizeof(buf), "%s (%s) %s", airline->name, airline->callsign, f.ac_type);
      else if (hasCs)
        snprintf(buf, sizeof(buf), "%s (%s)", airline->name, airline->callsign);
      else if (hasType)
        snprintf(buf, sizeof(buf), "%s  %s", airline->name, f.ac_type);
      else
        strncpy(buf, airline->name, sizeof(buf) - 1);
    } else if (hasType && hasMaker)
      snprintf(buf, sizeof(buf), "%s %s", f.ac_maker, f.ac_type);
    else if (hasType)
      strncpy(buf, f.ac_type, sizeof(buf) - 1);
    if (buf[0]) gfx->print(buf);
  }

  gfx->setTextColor(RGB565_WHITE);
  gfx->setCursor(8, py + 76);
  if (f.on_ground) {
    gfx->print("ON GROUND");
  } else if (!isnan(f.alt_m)) {
    snprintf(buf, sizeof(buf), "%.0fft  %.0fkn", mToFt(f.alt_m), msToKts(f.vel_ms));
    gfx->print(buf);
    if (!isnan(f.vert_ms)) {
      if (f.vert_ms >= 2.0f)       drawArrowVert(gfx->getCursorX() + 10, py + 84, true,  RGB565_WHITE);
      else if (f.vert_ms <= -2.0f) drawArrowVert(gfx->getCursorX() + 10, py + 84, false, RGB565_WHITE);
    }
  }

  gfx->setCursor(8, py + 106);
  snprintf(buf, sizeof(buf), "%.0f%s  %s  hdg %.0f\xf8",
           fc_use_miles ? f.dist_km * 0.621371f : f.dist_km,
           fc_use_miles ? "mi" : "km",
           fc_compass(f.bearing), f.track);
  gfx->print(buf);

  gfx->setTextColor(0x4208);
  gfx->setTextSize(1);
  gfx->setCursor(8, py + 128);
  gfx->print("tap to dismiss");
}

// ---------------------------------------------------------------------------
// Full redraw
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
// Fetch task (core 0)
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
// Footer zones use LCD_W/3 boundaries, mirroring drawFooter()
// ---------------------------------------------------------------------------
static void handleTouch(int tx, int ty) {
  int third = LCD_W / 3;
  int footerTouchY = LCD_H - FOOTER_H - 24;

  if (ty >= footerTouchY) {
    int newMode = (tx < third) ? MODE_RADAR : (tx < third * 2) ? MODE_STATS : MODE_LIST;
    if (newMode != fc_mode) {
      fc_mode       = newMode;
      fc_detail_idx = -1;
      if (fc_mode == MODE_STATS) stats_fetching_types = true;
      redraw();
    }
    return;
  }

  if (fc_mode == MODE_LIST && fc_detail_idx >= 0 && ty >= CONTENT_Y + CONTENT_H - 140) {
    fc_detail_idx = -1;
    drawList();
    drawFooter();
    return;
  }

  if (fc_mode == MODE_LIST && ty >= CONTENT_Y + 28) {
    int rowIdx = (ty - (CONTENT_Y + 28)) / LIST_ROW_H;
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

// Hard-reset the ESP32-C6 WiFi coprocessor and wait for the SDIO link.
// Must be called before WiFi.mode() — ESP.restart() only resets the P4,
// leaving the C6 in whatever state it was in.
static void reset_wifi_c6() {
  Serial.println("[WiFi] Resetting C6...");
  pinMode(WIFI_C6_RST, OUTPUT);
  digitalWrite(WIFI_C6_RST, LOW);
  delay(100);
  digitalWrite(WIFI_C6_RST, HIGH);
  unsigned long t0 = millis();
  while (WiFi.status() == WL_NO_SHIELD && millis() - t0 < 10000)
    delay(200);
  delay(1000);
  Serial.printf("[WiFi] C6 ready (%lums)\n", millis() - t0);
}

// ---------------------------------------------------------------------------
// setup()
// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("FlightRadarCYD P4 - Live Aircraft Radar");

  fcLoadSettings();

  // Display
  if (!gfx->begin()) Serial.println("gfx->begin() failed!");
  gfx->invertDisplay(fc_invert_display);
  gfx->fillScreen(RGB565_BLACK);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, HIGH);

  // Touch (GT911 defaults to landscape — no rotation needed)
  ts.begin();

  // BOOT button
  pinMode(BOOT_BTN, INPUT_PULLUP);

  bool showPortal = !fc_has_settings;

  if (!showPortal) {
    showStatus("Hold BOOT to change settings...");
    for (int i = 0; i < 30 && !showPortal; i++) {
      if (digitalRead(BOOT_BTN) == LOW) showPortal = true;
      delay(100);
    }
  }

  if (showPortal) {
    fcInitPortal();
    while (!portalDone) { fcRunPortal(); delay(5); }
    fcClosePortal();
  }

  // WiFi via ESP32-C6 coprocessor.
  // Reset C6 on every boot — ESP.restart() only resets the P4.
  gfx->fillScreen(RGB565_BLACK);
  showStatus("Resetting WiFi module...");
  reset_wifi_c6();
  int dots = 0;
  for (int attempt = 0; ; attempt++) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(fc_wifi_ssid, fc_wifi_pass);
    unsigned long wStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wStart < 30000) {
      delay(500);
      char msg[48];
      snprintf(msg, sizeof(msg), "Connecting to WiFi%.*s", (dots % 4) + 1, "....");
      showStatus(msg);
      dots++;
    }
    if (WiFi.status() == WL_CONNECTED) break;
    char msg[56];
    snprintf(msg, sizeof(msg), "WiFi failed, resetting C6 (#%d)...", attempt + 1);
    showStatus(msg);
    reset_wifi_c6();
  }
  // WL_CONNECTED fires before DHCP assigns an address — wait for lease.
  { unsigned long d0 = millis(); while (WiFi.localIP() == IPAddress(0,0,0,0) && millis()-d0 < 5000) delay(200); }
  Serial.printf("[WiFi] Connected, IP: %s\n", WiFi.localIP().toString().c_str());
  showStatus("WiFi connected!");
  showStatus("Loading airline data...");
  airlinesLoad();

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  setenv("TZ", fc_tz_posix, 1);
  tzset();
  {
    struct tm t;
    int tries = 0;
    unsigned long ntpStart = millis();
    while (!getLocalTime(&t, 0) && tries++ < 20) delay(500);
    if (getLocalTime(&t, 0)) Serial.printf("[NTP] Synced in %lums\n", millis() - ntpStart);
    else                     Serial.printf("[NTP] Failed after %lums\n", millis() - ntpStart);
  }
  LittleFS.begin(true);
  loadStats();

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
  // BOOT button (GPIO35): cycle RADAR → STATS → LIST → RADAR
  if (digitalRead(BOOT_BTN) == LOW) {
    delay(50);
    if (digitalRead(BOOT_BTN) == LOW) {
      fc_mode = (fc_mode + 1) % 3;
      fc_detail_idx = -1;
      if (fc_mode == MODE_STATS) stats_fetching_types = true;
      redraw();
      while (digitalRead(BOOT_BTN) == LOW) delay(10);
    }
  }

  if (fc_fetch_done) {
    fc_fetch_done = false;
    updateStats(fc_fetch_ok);
    fc_last_attempt = millis();
    if (fc_fetch_ok) fc_last_fetch = millis();
    fc_detail_idx = -1;
    redraw();
    if (fc_fetch_ok && fc_client_id[0] != '\0') updateTrails();
  }

  unsigned long fetchInterval = (fc_client_id[0] != '\0') ? OPENSKY_INTERVAL_AUTH : OPENSKY_INTERVAL_ANON;
  // Don't start while type fetch is running: both tasks pin to core 0, concurrent TLS starves both.
  if (!fc_fetching && !statsTypesFetchBusy() && (fc_last_attempt == 0 || (millis() - fc_last_attempt) > fetchInterval)) {
    if (WiFi.status() != WL_CONNECTED) {
      if (fc_wifi_connected) {
        fc_wifi_connected = false;
        stats_fetch_fail_count++;
        redraw();
      }
      fc_wifi_fail_count++;
      if (fc_wifi_fail_count >= 5) {
        // C6 is not recovering on its own — full restart so C6 gets hard-reset in setup().
        Serial.printf("[WiFi] %d consecutive failures, restarting\n", fc_wifi_fail_count);
        ESP.restart();
      }
      WiFi.reconnect();
      fc_last_attempt = millis();
      return;
    }
    fc_wifi_fail_count = 0;
    if (!fc_wifi_connected) fc_wifi_connected = true;
    fc_fetching   = true;
    fc_fetch_done = false;
    drawHeader();
    xTaskCreatePinnedToCore(fetchTaskFn, "fetch", 8192, NULL, 1, NULL, 0);
  }

  // Touch
  ts.read();
  if (ts.isTouched) {
    unsigned long now = millis();
    if (now - lastTouchTime > TOUCH_DEBOUNCE) {
      lastTouchTime = now;
      int tx = ts.points[0].x;
      int ty = ts.points[0].y;
      tx = constrain(tx, 0, LCD_W - 1);
      ty = constrain(ty, 0, LCD_H - 1);
      handleTouch(tx, ty);
    }
  }

  // Don't launch while fc_fetching: both tasks pin to core 0, concurrent TLS starves both.
  if (fc_mode == MODE_STATS && stats_fetching_types && !fc_fetching) {
    startTypesFetch();
    if (stats_type_arrived) {
      stats_type_arrived = false;
      drawStats();
      drawFooter();
    }
  }

  delay(10);
}
