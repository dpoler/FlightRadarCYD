#include "OpenSky.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

FlightData fc_flights[MAX_FLIGHTS];
int        fc_flight_count  = 0;
int        fc_total_in_bbox = 0;
int        fc_hidden_ground = 0;

static char              fc_access_token[2048] = "";
static unsigned long     fc_token_expiry_ms    = 0;
static WiFiClientSecure *s_token_client  = nullptr;
static WiFiClientSecure *s_states_client = nullptr;

static float fc_haversine(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  float dLat = (lat2 - lat1) * (float)M_PI / 180.0f;
  float dLon = (lon2 - lon1) * (float)M_PI / 180.0f;
  float a = sinf(dLat / 2) * sinf(dLat / 2)
          + cosf(lat1 * (float)M_PI / 180.0f) * cosf(lat2 * (float)M_PI / 180.0f)
          * sinf(dLon / 2) * sinf(dLon / 2);
  return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

static float fc_bearing_calc(float lat1, float lon1, float lat2, float lon2) {
  float l1 = lat1 * (float)M_PI / 180.0f;
  float l2 = lat2 * (float)M_PI / 180.0f;
  float dl = (lon2 - lon1) * (float)M_PI / 180.0f;
  float x  = sinf(dl) * cosf(l2);
  float y  = cosf(l1) * sinf(l2) - sinf(l1) * cosf(l2) * cosf(dl);
  return fmodf(atan2f(x, y) * 180.0f / (float)M_PI + 360.0f, 360.0f);
}

const char *fc_compass(float b) {
  const char *d[] = { "N","NE","E","SE","S","SW","W","NW","N" };
  return d[(int)((b + 22.5f) / 45.0f) % 8];
}

static void fc_insert_sorted(const FlightData &f) {
  if (fc_flight_count < MAX_FLIGHTS) {
    int ins = fc_flight_count;
    for (int i = 0; i < fc_flight_count; i++) {
      if (f.dist_km < fc_flights[i].dist_km) { ins = i; break; }
    }
    for (int i = fc_flight_count; i > ins; i--) fc_flights[i] = fc_flights[i - 1];
    fc_flights[ins] = f;
    fc_flight_count++;
  } else if (f.dist_km < fc_flights[MAX_FLIGHTS - 1].dist_km) {
    int ins = MAX_FLIGHTS - 1;
    for (int i = 0; i < MAX_FLIGHTS; i++) {
      if (f.dist_km < fc_flights[i].dist_km) { ins = i; break; }
    }
    for (int i = MAX_FLIGHTS - 1; i > ins; i--) fc_flights[i] = fc_flights[i - 1];
    fc_flights[ins] = f;
  }
}

// ---------------------------------------------------------------------------
// Token fetch (ArduinoJson fine here — response is ~500 bytes)
// ---------------------------------------------------------------------------
static bool fc_fetch_token(const char *clientId, const char *clientSecret) {
  const char *tokenUrl =
    "https://auth.opensky-network.org/auth/realms/opensky-network"
    "/protocol/openid-connect/token";

  if (!s_token_client) { s_token_client = new WiFiClientSecure; s_token_client->setInsecure(); }

  unsigned long t0 = millis();
  DynamicJsonDocument doc(4096);
  {
    HTTPClient https;
    https.begin(*s_token_client, tokenUrl);
    https.setTimeout(15000);
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String payload = "grant_type=client_credentials&client_id=";
    payload += clientId;
    payload += "&client_secret=";
    payload += clientSecret;
    int code = https.POST(payload);
    Serial.printf("[OpenSky] Token HTTP %d in %lums\n", code, millis() - t0);
    if (code != HTTP_CODE_OK) { https.end(); s_token_client->stop(); return false; }
    if (deserializeJson(doc, https.getStream())) {
      Serial.println("[OpenSky] Token parse failed");
      https.end();
      s_token_client->stop();
      return false;
    }
    https.end();
    s_token_client->stop();
  }

  const char *token = doc["access_token"] | "";
  int expires_in    = doc["expires_in"]   | 300;

  if (token[0] == '\0') {
    Serial.println("[OpenSky] No access_token in response");
    return false;
  }

  strncpy(fc_access_token, token, sizeof(fc_access_token) - 1);
  fc_access_token[sizeof(fc_access_token) - 1] = '\0';
  fc_token_expiry_ms = millis() + (unsigned long)(expires_in - 60) * 1000UL;
  Serial.printf("[OpenSky] Token acquired, expires in %ds\n", expires_in);
  return true;
}

// ---------------------------------------------------------------------------
// Streaming JSON parser for /api/states/all
//
// ArduinoJson's array filter uses filter[0] as a uniform template for every
// element — it cannot filter specific indices within a nested array.  So with
// the old approach all 17 fields were stored per aircraft, including the
// "sensors" nested array (15-20 ints in dense urban coverage) which alone
// blew the heap.  This streaming parser reads exactly the 12 fields we need
// and skips everything else, using near-zero heap.
// ---------------------------------------------------------------------------

struct StreamReader {
  Stream&       s;
  int           lookahead;      // -2=empty, -1=EOF/timeout, >=0=char
  unsigned long deadline_ms;

  StreamReader(Stream& stream, uint32_t timeout_ms)
    : s(stream), lookahead(-2), deadline_ms(millis() + timeout_ms) {}

  int read() {
    if (lookahead != -2) { int c = lookahead; lookahead = -2; return c; }
    while (!s.available()) {
      if (millis() > deadline_ms) return -1;
      delay(1);
    }
    return s.read();
  }

  int peek() {
    if (lookahead == -2) lookahead = read();
    return lookahead;
  }

  void unread(int c) { lookahead = c; }
};

static void sr_skipWS(StreamReader &r) {
  int c;
  while ((c = r.peek()) == ' ' || c == '\t' || c == '\n' || c == '\r')
    r.read();
}

// Read JSON string after opening quote was consumed; returns false on error.
static bool sr_readStr(StreamReader &r, char *buf, int max) {
  int i = 0, c;
  while ((c = r.read()) >= 0 && c != '"') {
    if (c == '\\') { c = r.read(); if (c < 0) return false; }
    if (i < max - 1) buf[i++] = (char)c;
  }
  buf[i] = '\0';
  return c == '"';
}

// Read a scalar token (number / bool / null) into buf until a JSON delimiter.
// The terminating delimiter is put back via unread().
static bool sr_readToken(StreamReader &r, char *buf, int max) {
  sr_skipWS(r);
  int i = 0, c;
  while ((c = r.read()) >= 0) {
    if (c == ',' || c == ']' || c == '}' || c == ' ' ||
        c == '\t' || c == '\n' || c == '\r') {
      r.unread(c);
      break;
    }
    if (i < max - 1) buf[i++] = (char)c;
  }
  buf[i] = '\0';
  return i > 0;
}

// Skip any JSON value (scalar, string, array, or object).
static bool sr_skipValue(StreamReader &r) {
  sr_skipWS(r);
  int c = r.read();
  if (c < 0) return false;
  if (c == '"') {
    while ((c = r.read()) >= 0 && c != '"')
      if (c == '\\') r.read();
    return c == '"';
  }
  if (c == '[' || c == '{') {
    int depth = 1;
    bool inStr = false;
    while ((c = r.read()) >= 0) {
      if (inStr) {
        if (c == '\\') r.read();
        else if (c == '"') inStr = false;
      } else {
        if      (c == '"')   inStr = true;
        else if (c == '[' || c == '{') depth++;
        else if (c == ']' || c == '}') { if (--depth == 0) return true; }
      }
    }
    return false;
  }
  // Scalar: consume until delimiter.
  while ((c = r.peek()) >= 0 && c != ',' && c != ']' && c != '}' &&
         c != ' ' && c != '\t' && c != '\n' && c != '\r')
    r.read();
  return true;
}

// Expect a specific character (after skipping whitespace); return false if mismatch.
static bool sr_expect(StreamReader &r, char expected) {
  sr_skipWS(r);
  return r.read() == expected;
}

// Parse one aircraft state array: [...] at current stream position.
// Fields: 0=icao24 1=callsign 2=country 3=t_pos 4=t_contact
//         5=lon 6=lat 7=baro_alt 8=on_ground 9=velocity 10=track 11=vert_rate
//         12=sensors(array) 13=geo_alt 14=squawk 15=spi 16=pos_src
// Returns false only on stream/parse error (not on filtered-out aircraft).
static bool parseOneState(StreamReader &r, float userLat, float userLon, float radiusKm, bool hideGround) {
  if (!sr_expect(r, '[')) return false;

  char tmp[32];
  FlightData f = {};
  bool hasPos = true;

  // 0: icao24
  sr_skipWS(r);
  if (r.peek() == '"') { r.read(); sr_readStr(r, f.icao, sizeof(f.icao)); }
  else { sr_skipValue(r); hasPos = false; }

  // 1: callsign
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  if (r.peek() == '"') {
    r.read();
    char cs[12] = {};
    sr_readStr(r, cs, sizeof(cs));
    int len = strlen(cs);
    while (len > 0 && cs[len - 1] == ' ') len--;
    cs[len] = '\0';
    strncpy(f.callsign, cs[0] ? cs : f.icao, sizeof(f.callsign) - 1);
  } else {
    sr_skipValue(r);
    strncpy(f.callsign, f.icao, sizeof(f.callsign) - 1);
  }

  // 2: origin_country
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  if (r.peek() == '"') { r.read(); sr_readStr(r, f.country, sizeof(f.country)); }
  else sr_skipValue(r);

  // 3,4: time_position, last_contact (skip)
  for (int i = 0; i < 2; i++) {
    if (!sr_expect(r, ',')) return false;
    sr_skipValue(r);
  }

  // 5: longitude
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  if (strcmp(tmp, "null") == 0) hasPos = false;
  else f.lon = atof(tmp);

  // 6: latitude
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  if (strcmp(tmp, "null") == 0) hasPos = false;
  else f.lat = atof(tmp);

  // 7: baro_altitude
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  f.alt_m = (strcmp(tmp, "null") == 0) ? NAN : atof(tmp);

  // 8: on_ground
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  f.on_ground = (strcmp(tmp, "true") == 0);

  // 9: velocity
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  f.vel_ms = (strcmp(tmp, "null") == 0) ? 0.0f : atof(tmp);

  // 10: true_track
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  f.track = (strcmp(tmp, "null") == 0) ? 0.0f : atof(tmp);

  // 11: vertical_rate
  if (!sr_expect(r, ',')) return false;
  sr_skipWS(r);
  sr_readToken(r, tmp, sizeof(tmp));
  f.vert_ms = (strcmp(tmp, "null") == 0) ? NAN : atof(tmp);

  // Skip remaining fields (12=sensors array, 13=geo_alt, 14=squawk, 15=spi, 16=pos_src)
  // Track depth so the sensors nested array is consumed correctly.
  {
    int depth = 1;   // we are inside the state array '[' we opened above
    bool inStr = false;
    int c;
    while ((c = r.read()) >= 0 && depth > 0) {
      if (inStr) {
        if (c == '\\') r.read();
        else if (c == '"') inStr = false;
      } else {
        if      (c == '"')  inStr = true;
        else if (c == '[')  depth++;
        else if (c == ']') { if (--depth == 0) break; }
      }
    }
  }

  // Count total aircraft the API returned (all with any position data).
  if (hasPos) fc_total_in_bbox++;

  if (!hasPos) return true;

  // Skip ground aircraft during insert so airborne ones get the limited slots.
  if (hideGround && f.on_ground) { fc_hidden_ground++; return true; }

  float dist = fc_haversine(userLat, userLon, f.lat, f.lon);
  if (dist <= radiusKm) {
    f.dist_km      = dist;
    f.bearing      = fc_bearing_calc(userLat, userLon, f.lat, f.lon);
    f.ac_type[0]   = '\0';
    f.ac_maker[0]  = '\0';
    f.type_fetched = false;
    fc_insert_sorted(f);
  }
  return true;
}

// Navigate stream to the "states" array and parse every element.
static bool parseOpenSkyStream(Stream &s, float userLat, float userLon, float radiusKm, bool hideGround) {
  StreamReader r(s, 25000);

  // Consume the opening '{' of the top-level JSON object.
  if (!sr_expect(r, '{')) return false;

  // Find the "states" key in the top-level object.
  while (true) {
    if (!sr_expect(r, '"')) return false;   // find next key's opening quote
    char key[16];
    sr_readStr(r, key, sizeof(key));
    if (!sr_expect(r, ':')) return false;
    if (strcmp(key, "states") == 0) break;
    sr_skipValue(r);               // skip value of irrelevant key
    sr_skipWS(r);
    int c = r.peek();
    if (c == ',') r.read();
    else if (c == '}' || c < 0) return false;  // end of object, no "states"
  }

  sr_skipWS(r);
  if (r.peek() != '[') {
    // "states": null — no aircraft
    return true;
  }
  r.read();  // consume '['

  sr_skipWS(r);
  if (r.peek() == ']') { r.read(); return true; }  // empty array

  while (true) {
    sr_skipWS(r);
    int c = r.peek();
    if (c < 0)  return false;   // timeout
    if (c == ']') { r.read(); break; }  // end of states array

    if (!parseOneState(r, userLat, userLon, radiusKm, hideGround)) return false;

    sr_skipWS(r);
    c = r.peek();
    if (c == ',') r.read();
    else if (c != ']') return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
bool openSkyFetch(float userLat, float userLon, float radiusKm,
                  const char *clientId, const char *clientSecret, bool hideGround) {
  float degLat = radiusKm / 111.0f;
  float degLon = radiusKm / (111.0f * cosf(userLat * (float)M_PI / 180.0f));
  char url[256];
  snprintf(url, sizeof(url),
    "https://opensky-network.org/api/states/all"
    "?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
    userLat - degLat, userLon - degLon,
    userLat + degLat, userLon + degLon);

  unsigned long t0 = millis();
  Serial.printf("[OpenSky] GET %s\n", url);

  bool hasAuth = clientId && clientId[0] != '\0';
  if (hasAuth && (fc_access_token[0] == '\0' || millis() >= fc_token_expiry_ms))
    hasAuth = fc_fetch_token(clientId, clientSecret);

  if (!s_states_client) { s_states_client = new WiFiClientSecure; s_states_client->setInsecure(); }
  Serial.printf("[Heap] pre-fetch free=%u min=%u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());

  fc_flight_count  = 0;
  fc_total_in_bbox = 0;
  fc_hidden_ground = 0;

  bool ok = false;
  {
    HTTPClient https;
    https.begin(*s_states_client, url);
    https.setTimeout(20000);
    https.useHTTP10(true);
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (hasAuth) https.addHeader("Authorization", String("Bearer ") + fc_access_token);
    int code = https.GET();
    Serial.printf("[OpenSky] HTTP %d in %lums\n", code, millis() - t0);
    if (code == HTTP_CODE_OK) {
      ok = parseOpenSkyStream(https.getStream(), userLat, userLon, radiusKm, hideGround);
      if (!ok) Serial.println("[OpenSky] stream parse error");
    }
    https.end();
    s_states_client->stop();
  }

  if (!ok) return false;

  if (fc_total_in_bbox == 0) Serial.println("[OpenSky] No aircraft in bounding box");
  Serial.printf("[OpenSky] bbox=%d  circle=%d  gnd_hidden=%d  total=%lums\n",
                fc_total_in_bbox, fc_flight_count, fc_hidden_ground, millis() - t0);
  Serial.printf("[Heap] post-fetch free=%u min=%u\n", ESP.getFreeHeap(), ESP.getMinFreeHeap());
  return true;
}
