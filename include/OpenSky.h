#pragma once

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>

// Anonymous:     400 req/day → 240 s interval → 360 req/day (safe)
// Authenticated: 4000 req/day →  30 s interval → 2880 req/day (safe)
#define OPENSKY_INTERVAL_ANON (4UL * 60UL * 1000UL)
#define OPENSKY_INTERVAL_AUTH (30UL * 1000UL)

static char          fc_access_token[2048] = "";
static unsigned long fc_token_expiry_ms    = 0;

#define MAX_FLIGHTS 30

struct FlightData {
  char  icao[8];       // ICAO24 hex address
  char  callsign[10];  // flight callsign (trailing spaces trimmed)
  char  country[20];   // origin country
  float lat, lon;      // last position (degrees)
  float alt_m;         // baro altitude in metres (NAN = unknown)
  float vel_ms;        // ground speed in m/s
  float track;         // true track in degrees (0 = N)
  bool  on_ground;
  float vert_ms;       // vertical rate m/s (+ climb, - descend, NAN = unknown)
  float dist_km;       // haversine distance from user (computed)
  float bearing;       // bearing from user (degrees, 0 = N, computed)
  char  ac_type[12];   // ICAO type code e.g. "B738" (from adsbdb, on-tap)
  char  ac_maker[24];  // manufacturer e.g. "Boeing"
  bool  type_fetched;  // true once adsbdb lookup has been attempted
};

FlightData fc_flights[MAX_FLIGHTS];
int  fc_flight_count  = 0;  // aircraft currently in fc_flights[]
int  fc_total_in_bbox = 0;  // raw count before radius filter + cap

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
static float fc_haversine(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371.0f;
  float dLat = (lat2 - lat1) * (float)M_PI / 180.0f;
  float dLon = (lon2 - lon1) * (float)M_PI / 180.0f;
  float a = sinf(dLat / 2) * sinf(dLat / 2)
          + cosf(lat1 * (float)M_PI / 180.0f) * cosf(lat2 * (float)M_PI / 180.0f)
          * sinf(dLon / 2) * sinf(dLon / 2);
  return R * 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
}

static float fc_bearing(float lat1, float lon1, float lat2, float lon2) {
  float l1 = lat1 * (float)M_PI / 180.0f;
  float l2 = lat2 * (float)M_PI / 180.0f;
  float dl = (lon2 - lon1) * (float)M_PI / 180.0f;
  float x  = sinf(dl) * cosf(l2);
  float y  = cosf(l1) * sinf(l2) - sinf(l1) * cosf(l2) * cosf(dl);
  return fmodf(atan2f(x, y) * 180.0f / (float)M_PI + 360.0f, 360.0f);
}

static const char *fc_compass(float b) {
  const char *d[] = { "N","NE","E","SE","S","SW","W","NW","N" };
  return d[(int)((b + 22.5f) / 45.0f) % 8];
}

// ---------------------------------------------------------------------------
// Sorted insert into fc_flights[] — keeps the MAX_FLIGHTS closest aircraft
// ---------------------------------------------------------------------------
static void fc_insert_sorted(const FlightData &f) {
  if (fc_flight_count < MAX_FLIGHTS) {
    // Find insertion point (sorted ascending by dist_km)
    int ins = fc_flight_count;
    for (int i = 0; i < fc_flight_count; i++) {
      if (f.dist_km < fc_flights[i].dist_km) { ins = i; break; }
    }
    for (int i = fc_flight_count; i > ins; i--) fc_flights[i] = fc_flights[i - 1];
    fc_flights[ins] = f;
    fc_flight_count++;
  } else if (f.dist_km < fc_flights[MAX_FLIGHTS - 1].dist_km) {
    // Replace farthest entry
    int ins = MAX_FLIGHTS - 1;
    for (int i = 0; i < MAX_FLIGHTS; i++) {
      if (f.dist_km < fc_flights[i].dist_km) { ins = i; break; }
    }
    for (int i = MAX_FLIGHTS - 1; i > ins; i--) fc_flights[i] = fc_flights[i - 1];
    fc_flights[ins] = f;
  }
}

// ---------------------------------------------------------------------------
// Fetch OAuth2 Bearer token using client credentials grant.
// ---------------------------------------------------------------------------
static bool fc_fetch_token(const char *clientId, const char *clientSecret) {
  const char *tokenUrl =
    "https://auth.opensky-network.org/auth/realms/opensky-network"
    "/protocol/openid-connect/token";

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();

  String body;
  {
    HTTPClient https;
    https.begin(*client, tokenUrl);
    https.setTimeout(15000);
    https.addHeader("Content-Type", "application/x-www-form-urlencoded");
    String payload = "grant_type=client_credentials&client_id=";
    payload += clientId;
    payload += "&client_secret=";
    payload += clientSecret;
    int code = https.POST(payload);
    if (code == HTTP_CODE_OK) {
      body = https.getString();
    } else {
      Serial.printf("[OpenSky] Token HTTP %d\n", code);
    }
    https.end();
  }
  delete client;

  if (body.isEmpty()) return false;

  DynamicJsonDocument doc(4096);
  if (deserializeJson(doc, body)) {
    Serial.println("[OpenSky] Token parse failed");
    return false;
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
// Fetch aircraft from OpenSky Network and populate fc_flights[].
// Returns true if the request succeeded (even if zero aircraft found).
// ---------------------------------------------------------------------------
bool openSkyFetch(float userLat, float userLon, float radiusKm,
                  const char *clientId = "", const char *clientSecret = "") {
  // Build bounding box URL
  float degLat = radiusKm / 111.0f;
  float degLon = radiusKm / (111.0f * cosf(userLat * (float)M_PI / 180.0f));
  char url[256];
  snprintf(url, sizeof(url),
    "https://opensky-network.org/api/states/all"
    "?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
    userLat - degLat, userLon - degLon,
    userLat + degLat, userLon + degLon);

  Serial.printf("[OpenSky] GET %s\n", url);

  bool hasAuth = clientId && clientId[0] != '\0';
  if (hasAuth && (fc_access_token[0] == '\0' || millis() >= fc_token_expiry_ms))
    hasAuth = fc_fetch_token(clientId, clientSecret);

  WiFiClientSecure *client = new WiFiClientSecure;
  if (!client) return false;
  client->setInsecure();
  String body;
  {
    HTTPClient https;
    https.begin(*client, url);
    https.setTimeout(20000);
    https.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    if (hasAuth) https.addHeader("Authorization", String("Bearer ") + fc_access_token);
    int code = https.GET();
    if (code == HTTP_CODE_OK) {
      body = https.getString();
    } else {
      Serial.printf("[OpenSky] HTTP %d\n", code);
    }
    https.end();
  }
  delete client;

  if (body.isEmpty()) return false;

  DynamicJsonDocument doc(32768);
  if (deserializeJson(doc, body)) {
    Serial.println("[OpenSky] JSON parse failed");
    return false;
  }

  JsonArray states = doc["states"];
  fc_flight_count  = 0;
  fc_total_in_bbox = 0;

  if (states.isNull()) {
    // API returned null states — no aircraft in bounding box
    Serial.println("[OpenSky] No aircraft in bounding box");
    return true;
  }

  fc_total_in_bbox = states.size();

  for (JsonArray state : states) {
    // Latitude and longitude can be null (no recent position fix)
    if (state[6].isNull() || state[5].isNull()) continue;
    float lat = state[6].as<float>();
    float lon = state[5].as<float>();
    float dist = fc_haversine(userLat, userLon, lat, lon);
    if (dist > radiusKm) continue;  // outside circle (bbox is square)

    FlightData f;
    // ICAO24
    const char *icao = state[0] | "??????";
    strncpy(f.icao, icao, sizeof(f.icao) - 1);
    f.icao[sizeof(f.icao) - 1] = '\0';

    // Callsign — trim whitespace; fall back to ICAO if blank
    String cs = String(state[1] | "");
    cs.trim();
    if (cs.isEmpty()) cs = String(f.icao);
    strncpy(f.callsign, cs.c_str(), sizeof(f.callsign) - 1);
    f.callsign[sizeof(f.callsign) - 1] = '\0';

    // Origin country
    const char *ctry = state[2] | "";
    strncpy(f.country, ctry, sizeof(f.country) - 1);
    f.country[sizeof(f.country) - 1] = '\0';

    f.lat       = lat;
    f.lon       = lon;
    f.alt_m     = state[7].isNull()  ? NAN  : state[7].as<float>();
    f.on_ground = state[8]           | false;
    f.vel_ms    = state[9].isNull()  ? 0.0f : state[9].as<float>();
    f.track     = state[10].isNull() ? 0.0f : state[10].as<float>();
    f.vert_ms   = state[11].isNull() ? NAN  : state[11].as<float>();
    f.dist_km      = dist;
    f.bearing      = fc_bearing(userLat, userLon, lat, lon);
    f.ac_type[0]   = '\0';
    f.ac_maker[0]  = '\0';
    f.type_fetched = false;

    fc_insert_sorted(f);
  }

  Serial.printf("[OpenSky] bbox=%d  circle=%d  shown=%d\n",
                fc_total_in_bbox, fc_flight_count, fc_flight_count);
  return true;
}
