#pragma once

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "OpenSky.h"

// ---------------------------------------------------------------------------
// Look up aircraft type by ICAO hex from adsbdb.com.
// Called on first detail tap; result cached in FlightData until next radar refresh.
// Returns true if a type was found.
// ---------------------------------------------------------------------------
bool adsbdbFetchType(FlightData &f) {
  if (f.type_fetched) return f.ac_type[0] != '\0';
  f.type_fetched = true;

  char url[80];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", f.icao);
  Serial.printf("[ADSBDB] GET %s\n", url);

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();

  String body;
  {
    HTTPClient https;
    https.begin(*client, url);
    https.setTimeout(10000);
    int code = https.GET();
    if (code == HTTP_CODE_OK) body = https.getString();
    else Serial.printf("[ADSBDB] HTTP %d\n", code);
    https.end();
  }
  delete client;

  if (body.isEmpty()) return false;

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) return false;

  JsonVariant ac = doc["response"]["aircraft"];
  if (ac.isNull()) return false;

  const char *t = ac["icao_type"]    | "";
  const char *m = ac["manufacturer"] | "";

  strncpy(f.ac_type,  t, sizeof(f.ac_type)  - 1); f.ac_type[sizeof(f.ac_type)   - 1] = '\0';
  strncpy(f.ac_maker, m, sizeof(f.ac_maker) - 1); f.ac_maker[sizeof(f.ac_maker) - 1] = '\0';

  Serial.printf("[ADSBDB] %s: %s %s\n", f.icao, m, t);
  return f.ac_type[0] != '\0';
}

// ---------------------------------------------------------------------------
// Look up typical flight route by callsign from adsbdb.com.
// Called on first detail tap; result cached in FlightData until next radar refresh.
// Note: adsbdb stores scheduled/historical routes — may not reflect today's actual flight.
// Returns true if a route was found.
// ---------------------------------------------------------------------------
bool adsbdbFetchRoute(FlightData &f) {
  if (f.route_fetched) return f.origin_iata[0] != '\0';
  f.route_fetched = true;

  char url[96];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/callsign/%s", f.callsign);
  Serial.printf("[ADSBDB] GET %s\n", url);

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();

  String body;
  {
    HTTPClient https;
    https.begin(*client, url);
    https.setTimeout(10000);
    int code = https.GET();
    if (code == HTTP_CODE_OK) body = https.getString();
    else Serial.printf("[ADSBDB] HTTP %d\n", code);
    https.end();
  }
  delete client;

  if (body.isEmpty()) return false;

  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) return false;

  JsonVariant fr = doc["response"]["flightroute"];
  if (fr.isNull()) return false;

  const char *oi = fr["origin"]["iata_code"]        | "";
  const char *di = fr["destination"]["iata_code"]   | "";
  const char *oc = fr["origin"]["municipality"]      | "";
  const char *dc = fr["destination"]["municipality"] | "";

  strncpy(f.origin_iata, oi, sizeof(f.origin_iata) - 1); f.origin_iata[sizeof(f.origin_iata)-1] = '\0';
  strncpy(f.dest_iata,   di, sizeof(f.dest_iata)   - 1); f.dest_iata[sizeof(f.dest_iata)-1]     = '\0';
  strncpy(f.origin_city, oc, sizeof(f.origin_city) - 1); f.origin_city[sizeof(f.origin_city)-1] = '\0';
  strncpy(f.dest_city,   dc, sizeof(f.dest_city)   - 1); f.dest_city[sizeof(f.dest_city)-1]     = '\0';

  Serial.printf("[ADSBDB] %s: %s>%s\n", f.callsign, oi, di);
  return f.origin_iata[0] != '\0';
}
