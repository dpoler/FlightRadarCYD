#include "Stats.h"
#include "ADSBDB.h"
#include <Arduino.h>
#include <Preferences.h>
#include <LittleFS.h>

#define MAX_SEEN      4500
#define MAX_HOUR_SEEN 500

// Public state
int        stats_unique_count     = 0;
int        stats_fetch_count      = 0;
int        stats_fetch_fail_count = 0;
int        stats_peak_count    = 0;
float      stats_closest_dist  = 1e9f;
StatRecord stats_closest       = {};
float      stats_highest_alt   = -1e9f;
StatRecord stats_highest       = {};
float      stats_fastest_spd   = -1e9f;
StatRecord stats_fastest       = {};
float      stats_climb_rate    = -1e9f;
StatRecord stats_climb         = {};
float      stats_desc_rate     =  1e9f;
StatRecord stats_desc          = {};
uint8_t    stats_hourly_unique[24] = {};
int        stats_current_hour  = -1;
bool       stats_fetching_types = false;
volatile bool stats_type_arrived = false;
bool       stats_seen_capped   = false;

// Private state
static char       stats_ns[10]                     = "stats0";
static char       stats_seen_file[16]              = "/seen0.bin";
static char       stats_hrseen_file[20]            = "/hr_seen0.bin";
static char       stats_peak_hhmm[6]               = {};
static time_t     stats_peak_ts                    = 0;
static bool       stats_types_pending              = false;
static char       stats_seen_icao[MAX_SEEN][7]     = {};
static uint32_t   stats_seen_ts[MAX_SEEN]          = {};
static int        stats_seen_count                 = 0;
static char       stats_hour_seen_icao[MAX_HOUR_SEEN][7] = {};
static int        stats_hour_seen_cnt              = 0;
static time_t     stats_save_ts                    = 0;
static TaskHandle_t hTypeTask                      = NULL;

void setStatsLocation(int idx) {
    snprintf(stats_ns,         sizeof(stats_ns),         "stats%d", idx);
    snprintf(stats_seen_file,  sizeof(stats_seen_file),  "/seen%d.bin", idx);
    snprintf(stats_hrseen_file,sizeof(stats_hrseen_file),"/hr_seen%d.bin", idx);
}

static void captureTime(char *hhmm6) {
  struct tm t;
  if (getLocalTime(&t, 0)) snprintf(hhmm6, 6, "%02d:%02d", t.tm_hour, t.tm_min);
  else hhmm6[0] = '\0';
}

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
  stats_fetching_types = true;
}

static void refreshRecord(StatRecord &r, const FlightData &f) {
  const char *cs = f.callsign[0] ? f.callsign : f.icao;
  strncpy(r.cs, cs, sizeof(r.cs) - 1); r.cs[sizeof(r.cs)-1] = '\0';
  r.bearing = f.bearing;
  r.ts      = time(nullptr);
  captureTime(r.hhmm);
}

void expireOldRecords() {
  time_t now = time(nullptr);
  if (now <= 0) return;

  auto expire = [&](StatRecord &r, float &val, float resetVal) {
    if (r.ts > 0 && (now - r.ts) > 86400) { r = {}; val = resetVal; }
  };
  expire(stats_closest, stats_closest_dist, 1e9f);
  expire(stats_highest, stats_highest_alt, -1e9f);
  expire(stats_fastest, stats_fastest_spd, -1e9f);
  expire(stats_climb,   stats_climb_rate,  -1e9f);
  expire(stats_desc,    stats_desc_rate,    1e9f);

  if (stats_peak_ts > 0 && (now - stats_peak_ts) > 86400) {
    stats_peak_count    = 0;
    stats_peak_ts       = 0;
    stats_peak_hhmm[0]  = '\0';
  }

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
  stats_seen_count   = n;
  stats_unique_count = n;
  if (stats_seen_count < MAX_SEEN) stats_seen_capped = false;
}

void resetStats() {
  stats_unique_count      = 0;
  stats_fetch_count       = 0;
  stats_fetch_fail_count  = 0;
  stats_peak_count    = 0;
  stats_peak_ts       = 0;
  stats_peak_hhmm[0]  = '\0';
  stats_closest_dist  = 1e9f;  stats_closest = {};
  stats_highest_alt   = -1e9f; stats_highest = {};
  stats_fastest_spd   = -1e9f; stats_fastest = {};
  stats_climb_rate    = -1e9f; stats_climb   = {};
  stats_desc_rate     =  1e9f; stats_desc    = {};
  memset(stats_hourly_unique, 0, sizeof(stats_hourly_unique));
  stats_current_hour  = -1;
  stats_hour_seen_cnt = 0;
  stats_seen_count    = 0;
  stats_seen_capped   = false;
  // Deferred seen-file clear: set flag so loadStats() wipes it after LittleFS is ready
  Preferences prefs;
  prefs.begin(stats_ns, false);
  prefs.putBool("loc_reset", true);
  prefs.end();
}

void saveStats() {
  Preferences prefs;
  prefs.begin(stats_ns, false);
  prefs.putInt("ver",       4);
  prefs.putUInt("save_ts",  (uint32_t)time(nullptr));
  prefs.putInt("unique",    stats_unique_count);
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
  prefs.putBytes("hourly_u", stats_hourly_unique, sizeof(stats_hourly_unique));
  prefs.end();

  // hr_seen in LittleFS (too large for NVS — up to MAX_HOUR_SEEN*7 bytes)
  {
    File hf = LittleFS.open(stats_hrseen_file, "w");
    if (hf) {
      uint16_t cnt = (uint16_t)stats_hour_seen_cnt;
      hf.write((uint8_t*)&cnt, sizeof(cnt));
      if (cnt > 0) hf.write((uint8_t*)stats_hour_seen_icao, cnt * 7);
      hf.close();
    }
  }

  File sf = LittleFS.open(stats_seen_file, "w");
  if (sf) {
    uint16_t cnt = (uint16_t)stats_seen_count;
    sf.write((uint8_t*)&cnt, sizeof(cnt));
    if (cnt > 0) {
      sf.write((uint8_t*)stats_seen_icao, cnt * 7);
      sf.write((uint8_t*)stats_seen_ts,   cnt * sizeof(uint32_t));
    }
    sf.close();
  }
}

void loadStats() {
  {
    Preferences prefs;
    prefs.begin(stats_ns, false);
    if (prefs.getBool("loc_reset", false)) {
      prefs.remove("loc_reset");
      prefs.end();
      LittleFS.remove(stats_seen_file);
      LittleFS.remove(stats_hrseen_file);
      Serial.printf("[Stats] Location reset — %s cleared\n", stats_seen_file);
      return;
    }
    prefs.end();
  }

  Preferences prefs;
  prefs.begin(stats_ns, true);
  if (prefs.getInt("ver", 0) < 4) { prefs.end(); return; }

  stats_save_ts        = (time_t)prefs.getUInt("save_ts", 0);
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
  prefs.getBytes("hourly_u", stats_hourly_unique, sizeof(stats_hourly_unique));
  // Migrate: remove old NVS hr_seen blobs (now stored in LittleFS)
  if (prefs.isKey("hr_seen"))     prefs.remove("hr_seen");
  if (prefs.isKey("hr_seen_cnt")) prefs.remove("hr_seen_cnt");
  prefs.end();

  // Load hr_seen from LittleFS
  {
    File hf = LittleFS.open(stats_hrseen_file, "r");
    if (hf) {
      uint16_t cnt = 0;
      hf.read((uint8_t*)&cnt, sizeof(cnt));
      if (cnt > MAX_HOUR_SEEN) cnt = MAX_HOUR_SEEN;
      stats_hour_seen_cnt = (int)cnt;
      if (cnt > 0) hf.read((uint8_t*)stats_hour_seen_icao, cnt * 7);
      hf.close();
    }
  }

  {
    File sf = LittleFS.open(stats_seen_file, "r");
    if (sf) {
      uint16_t cnt = 0;
      sf.read((uint8_t*)&cnt, sizeof(cnt));
      if (cnt > MAX_SEEN) cnt = MAX_SEEN;
      stats_seen_count = (int)cnt;
      if (cnt > 0) {
        sf.read((uint8_t*)stats_seen_icao, cnt * 7);
        sf.read((uint8_t*)stats_seen_ts,   cnt * sizeof(uint32_t));
      }
      sf.close();
    }
  }

  {
    time_t now_ts = time(nullptr);
    int64_t elapsed = (int64_t)now_ts - (int64_t)stats_save_ts;

    if (now_ts <= 0) {
      stats_current_hour = -1;
    } else if (stats_save_ts <= 0 || elapsed >= 86400) {
      memset(stats_hourly_unique, 0, sizeof(stats_hourly_unique));
      stats_current_hour = -1;
    } else {
      struct tm save_tm, now_tm;
      localtime_r(&stats_save_ts, &save_tm);
      getLocalTime(&now_tm, 0);
      stats_current_hour = save_tm.tm_hour;

      if (save_tm.tm_hour != now_tm.tm_hour) {
        int h = (save_tm.tm_hour + 1) % 24;
        while (h != now_tm.tm_hour) {
          stats_hourly_unique[h] = 0;
          h = (h + 1) % 24;
        }
        stats_hourly_unique[now_tm.tm_hour] = 0;
        stats_hour_seen_cnt = 0;
      }
      // else: stats_hour_seen_cnt already loaded from LittleFS
    }
  }

  expireOldRecords();

  Serial.printf("[Stats] Loaded ns=%s: save_ts=%lu unique=%d\n",
                stats_ns, (unsigned long)stats_save_ts, stats_unique_count);
}

void updateStats(bool fetchOk) {
  expireOldRecords();
  if (fetchOk) stats_fetch_count++;
  else         stats_fetch_fail_count++;

  if (fc_flight_count > stats_peak_count) {
    stats_peak_count = fc_flight_count;
    stats_peak_ts    = time(nullptr);
    captureTime(stats_peak_hhmm);
  }

  struct tm t;
  if (getLocalTime(&t, 0)) {
    if (t.tm_hour != stats_current_hour) {
      stats_hourly_unique[t.tm_hour] = 0;
      stats_hour_seen_cnt = 0;
      stats_current_hour  = t.tm_hour;
    }
  }

  for (int i = 0; i < fc_flight_count; i++) {
    const FlightData &f = fc_flights[i];

    bool seen = false;
    for (int j = 0; j < stats_seen_count; j++)
      if (strcmp(stats_seen_icao[j], f.icao) == 0) { seen = true; break; }

    if (!seen) {
      if (stats_seen_count < MAX_SEEN) {
        strncpy(stats_seen_icao[stats_seen_count], f.icao, 6);
        stats_seen_icao[stats_seen_count][6] = '\0';
        stats_seen_ts[stats_seen_count] = (uint32_t)time(nullptr);
        stats_seen_count++;
        stats_unique_count++;
      } else {
        stats_seen_capped = true;
        stats_unique_count++;
      }
    }

    if (stats_current_hour >= 0) {
      bool seenHr = false;
      for (int j = 0; j < stats_hour_seen_cnt; j++)
        if (strcmp(stats_hour_seen_icao[j], f.icao) == 0) { seenHr = true; break; }
      if (!seenHr && stats_hour_seen_cnt < MAX_HOUR_SEEN) {
        strncpy(stats_hour_seen_icao[stats_hour_seen_cnt], f.icao, 6);
        stats_hour_seen_icao[stats_hour_seen_cnt][6] = '\0';
        stats_hour_seen_cnt++;
        if (stats_hourly_unique[stats_current_hour] < 255)
          stats_hourly_unique[stats_current_hour]++;
      }
    }

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

static void typesFetchTaskFn(void *) {
  StatRecord *recs[] = { &stats_closest, &stats_highest, &stats_fastest,
                         &stats_climb,   &stats_desc };
  for (auto *r : recs) {
    if (!stats_types_pending) break;
    if (!r->icao[0] || r->type_attempted) continue;
    r->type_attempted = true;

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

void startTypesFetch() {
  if (hTypeTask || !stats_types_pending) return;
#ifdef CONFIG_IDF_TARGET_ESP32P4
  const BaseType_t taskCore = 1;
#else
  const BaseType_t taskCore = 0;
#endif
  xTaskCreatePinnedToCore(typesFetchTaskFn, "typeFetch", 8192, NULL, 1, &hTypeTask, taskCore);
}

bool statsTypesFetchBusy() {
  return hTypeTask != NULL;
}
