#pragma once
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <string.h>

#define AIRLINES_URL \
  "https://raw.githubusercontent.com/dpoler/FlightRadarCYD/main/airlines.csv"
#define AIRLINES_MAX 400

struct AirlineEntry { char code[4]; char name[26]; };

static AirlineEntry g_airlines[AIRLINES_MAX];
static int          g_airline_count = 0;

// Fetch airlines.csv from GitHub and parse into g_airlines[].
// Call once after WiFi connects. Degrades gracefully on failure (no names shown).
static bool airlinesLoad() {
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();

  String body;
  {
    HTTPClient https;
    https.begin(*client, AIRLINES_URL);
    https.setTimeout(15000);
    int code = https.GET();
    if (code == HTTP_CODE_OK) body = https.getString();
    else Serial.printf("[Airlines] HTTP %d\n", code);
    https.end();
  }
  delete client;

  if (body.isEmpty()) return false;

  g_airline_count = 0;
  int pos = 0;
  int len = (int)body.length();

  while (pos < len && g_airline_count < AIRLINES_MAX) {
    char c = body[pos];
    // Skip comment lines and blank lines
    if (c == '#' || c == '\r' || c == '\n') {
      while (pos < len && body[pos] != '\n') pos++;
      pos++;
      continue;
    }
    int comma = body.indexOf(',', pos);
    if (comma < 0) break;
    int eol = body.indexOf('\n', comma);
    if (eol < 0) eol = len;

    String icao = body.substring(pos, comma);
    String name = body.substring(comma + 1, eol);
    icao.trim();
    name.trim();

    int ilen = (int)icao.length();
    if (ilen >= 2 && ilen <= 3 && name.length() > 0) {
      strncpy(g_airlines[g_airline_count].code, icao.c_str(), 3);
      g_airlines[g_airline_count].code[3] = '\0';
      strncpy(g_airlines[g_airline_count].name, name.c_str(), 25);
      g_airlines[g_airline_count].name[25] = '\0';
      g_airline_count++;
    }
    pos = eol + 1;
  }

  Serial.printf("[Airlines] Loaded %d entries\n", g_airline_count);
  return g_airline_count > 0;
}

// Returns airline name for callsign prefix (e.g. "AAL123" -> "American Airlines"),
// or nullptr if not a recognised airline flight.
static const char *airlineLookup(const char *callsign) {
  char prefix[4] = {0, 0, 0, 0};
  int  plen = 0;
  while (plen < 3 && callsign[plen] >= 'A' && callsign[plen] <= 'Z') {
    prefix[plen] = callsign[plen];
    plen++;
  }
  if (plen < 2) return nullptr;
  bool hasNum = false;
  for (int j = plen; callsign[j]; j++)
    if (callsign[j] >= '0' && callsign[j] <= '9') { hasNum = true; break; }
  if (!hasNum) return nullptr;

  for (int i = 0; i < g_airline_count; i++)
    if (strcmp(prefix, g_airlines[i].code) == 0)
      return g_airlines[i].name;
  return nullptr;
}
