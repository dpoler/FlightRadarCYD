#pragma once
#include "OpenSky.h"
#include <time.h>

struct StatRecord {
  char   cs[10];
  char   icao[8];
  char   ac_type[12];
  char   hhmm[6];
  float  bearing;
  bool   type_attempted;
  time_t ts;
};

extern int        stats_unique_count;
extern int        stats_fetch_count;
extern int        stats_peak_count;
extern float      stats_closest_dist;
extern StatRecord stats_closest;
extern float      stats_highest_alt;
extern StatRecord stats_highest;
extern float      stats_fastest_spd;
extern StatRecord stats_fastest;
extern float      stats_climb_rate;
extern StatRecord stats_climb;
extern float      stats_desc_rate;
extern StatRecord stats_desc;
extern uint8_t    stats_hourly_unique[24];
extern int        stats_current_hour;
extern bool       stats_fetching_types;
extern volatile bool stats_type_arrived;

void saveStats();
void loadStats();
void updateStats();
void expireOldRecords();
void startTypesFetch();
