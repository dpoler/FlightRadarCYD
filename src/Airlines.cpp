#include "Airlines.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

#define AIRLINES_URL \
  "https://raw.githubusercontent.com/dpoler/FlightRadarCYD/main/airlines.csv"

static AirlineEntry g_airlines[AIRLINES_MAX];
static int          g_airline_count = 0;

bool airlinesLoad() {
  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();

  unsigned long t0 = millis();
  String body;
  {
    HTTPClient https;
    https.begin(*client, AIRLINES_URL);
    https.setTimeout(15000);
    int code = https.GET();
    Serial.printf("[Airlines] HTTP %d in %lums\n", code, millis() - t0);
    if (code == HTTP_CODE_OK) body = https.getString();
    https.end();
  }
  delete client;

  if (body.isEmpty()) return false;

  g_airline_count = 0;
  int pos = 0;
  int len = (int)body.length();

  while (pos < len && g_airline_count < AIRLINES_MAX) {
    char c = body[pos];
    if (c == '#' || c == '\r' || c == '\n') {
      while (pos < len && body[pos] != '\n') pos++;
      pos++;
      continue;
    }
    int comma1 = body.indexOf(',', pos);
    if (comma1 < 0) break;
    int eol = body.indexOf('\n', comma1);
    if (eol < 0) eol = len;

    // Optional third field: ICAO,Name,Callsign
    int comma2 = body.indexOf(',', comma1 + 1);
    bool hasCallsign = (comma2 >= 0 && comma2 < eol);

    String icao     = body.substring(pos, comma1);
    String name     = body.substring(comma1 + 1, hasCallsign ? comma2 : eol);
    String cs       = hasCallsign ? body.substring(comma2 + 1, eol) : String();
    icao.trim(); name.trim(); cs.trim();

    int ilen = (int)icao.length();
    if (ilen >= 2 && ilen <= 3 && name.length() > 0) {
      strncpy(g_airlines[g_airline_count].code, icao.c_str(), 3);
      g_airlines[g_airline_count].code[3] = '\0';
      strncpy(g_airlines[g_airline_count].name, name.c_str(), 25);
      g_airlines[g_airline_count].name[25] = '\0';
      strncpy(g_airlines[g_airline_count].callsign, cs.c_str(), 15);
      g_airlines[g_airline_count].callsign[15] = '\0';
      g_airline_count++;
    }
    pos = eol + 1;
  }

  Serial.printf("[Airlines] Loaded %d entries\n", g_airline_count);
  return g_airline_count > 0;
}

const AirlineEntry *airlineLookup(const char *callsign) {
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
      return &g_airlines[i];
  return nullptr;
}
