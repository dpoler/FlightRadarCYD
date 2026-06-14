#pragma once

#define AIRLINES_MAX 250

struct AirlineEntry {
  char code[4];
  char name[26];
  char callsign[16];
};

bool               airlinesLoad();
const AirlineEntry *airlineLookup(const char *callsign);
