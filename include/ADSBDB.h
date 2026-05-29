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
bool adsbdbFetchType(FlightData &f, int timeoutMs = 10000) {
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
    https.setTimeout(timeoutMs);
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
