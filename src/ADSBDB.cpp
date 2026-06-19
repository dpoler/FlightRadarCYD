#include "ADSBDB.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

bool adsbdbFetchType(FlightData &f, int timeoutMs) {
  if (f.type_fetched) return f.ac_type[0] != '\0';
  f.type_fetched = true;

  char url[80];
  snprintf(url, sizeof(url), "https://api.adsbdb.com/v0/aircraft/%s", f.icao);
  unsigned long t0 = millis();
  Serial.printf("[ADSBDB] GET %s\n", url);

  // Local WiFiClientSecure: destructor guarantees TLS context is freed on
  // every exit path — no stale state carried across calls.
  WiFiClientSecure client;
  client.setInsecure();

  DynamicJsonDocument doc(2048);
  {
    HTTPClient https;
    https.begin(client, url);
    https.setTimeout(timeoutMs);
    https.useHTTP10(true);  // clean connection close after response
    int code = https.GET();
    Serial.printf("[ADSBDB] HTTP %d in %lums\n", code, millis() - t0);
    if (code == HTTP_CODE_OK)
      deserializeJson(doc, https.getStream());
    https.end();
  }  // client destructor runs here → stop() + mbedTLS free

  JsonVariant ac = doc["response"]["aircraft"];
  if (ac.isNull()) return false;

  const char *t = ac["icao_type"]    | "";
  const char *m = ac["manufacturer"] | "";

  strncpy(f.ac_type,  t, sizeof(f.ac_type)  - 1); f.ac_type[sizeof(f.ac_type)   - 1] = '\0';
  strncpy(f.ac_maker, m, sizeof(f.ac_maker) - 1); f.ac_maker[sizeof(f.ac_maker) - 1] = '\0';

  Serial.printf("[ADSBDB] %s: %s %s\n", f.icao, m, t);
  return f.ac_type[0] != '\0';
}
