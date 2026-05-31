#pragma once
#include <math.h>

#define OPENSKY_INTERVAL_ANON (4UL * 60UL * 1000UL)
#define OPENSKY_INTERVAL_AUTH (30UL * 1000UL)
#define MAX_FLIGHTS 30

struct FlightData {
  char  icao[8];
  char  callsign[10];
  char  country[20];
  float lat, lon;
  float alt_m;
  float vel_ms;
  float track;
  bool  on_ground;
  float vert_ms;
  float dist_km;
  float bearing;
  char  ac_type[12];
  char  ac_maker[24];
  bool  type_fetched;
};

extern FlightData fc_flights[MAX_FLIGHTS];
extern int        fc_flight_count;
extern int        fc_total_in_bbox;

const char *fc_compass(float b);
bool openSkyFetch(float userLat, float userLon, float radiusKm,
                  const char *clientId = "", const char *clientSecret = "");
