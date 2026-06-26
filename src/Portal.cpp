#include "Portal.h"
#include "Stats.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ArduinoJson.h>

extern Arduino_GFX *gfx;

// Settings globals (definitions)
char fc_wifi_ssid[64]     = "";
char fc_wifi_pass[64]     = "";
char fc_lat[16]           = "";
char fc_lon[16]           = "";
int  fc_radius_km         = 150;
bool fc_use_miles         = false;
int  fc_elevation_ft      = 0;
char fc_client_id[80]     = "";
char fc_client_secret[64] = "";
uint8_t fc_filter_mask    = FILTER_ALL;
bool    fc_show_labels    = true;
bool fc_invert_display    = false;
char fc_tz_posix[64]      = "UTC0";
bool fc_has_settings      = false;
bool portalDone           = false;

// Location globals
LocationEntry fc_locations[MAX_LOCATIONS] = {};
int           fc_loc_count  = 1;
int           fc_active_loc = 0;

// Portal state (private)
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;

void fcApplyLocation(int idx) {
  if (idx < 0 || idx >= fc_loc_count) return;
  strncpy(fc_lat, fc_locations[idx].lat,  sizeof(fc_lat)  - 1); fc_lat[sizeof(fc_lat)-1]  = '\0';
  strncpy(fc_lon, fc_locations[idx].lon,  sizeof(fc_lon)  - 1); fc_lon[sizeof(fc_lon)-1]  = '\0';
  fc_elevation_ft = fc_locations[idx].elevation_ft;
  const char *tz = (fc_locations[idx].tz_posix[0] != '\0')
                   ? fc_locations[idx].tz_posix : fc_tz_posix;
  setenv("TZ", tz, 1);
  tzset();
}

void fcSaveLocations() {
  DynamicJsonDocument doc(2048);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < fc_loc_count; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["name"] = fc_locations[i].name;
    obj["lat"]  = fc_locations[i].lat;
    obj["lon"]  = fc_locations[i].lon;
    obj["elev"] = fc_locations[i].elevation_ft;
    if (fc_locations[i].tz_posix[0] != '\0')
      obj["tz"] = fc_locations[i].tz_posix;
  }
  File f = LittleFS.open("/locations.json", "w");
  if (f) { serializeJson(doc, f); f.close(); }

  Preferences prefs;
  prefs.begin("flightcyd", false);
  prefs.putInt("loc_idx", fc_active_loc);
  prefs.end();
}

void fcLoadLocations() {
  // Load active location index from NVS
  {
    Preferences prefs;
    prefs.begin("flightcyd", true);
    fc_active_loc = prefs.getInt("loc_idx", 0);
    prefs.end();
  }

  // Try loading locations.json from LittleFS
  File f = LittleFS.open("/locations.json", "r");
  if (f) {
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (!err && doc.is<JsonArray>()) {
      JsonArray arr = doc.as<JsonArray>();
      fc_loc_count = 0;
      for (JsonObject obj : arr) {
        if (fc_loc_count >= MAX_LOCATIONS) break;
        LocationEntry &e = fc_locations[fc_loc_count++];
        const char *n  = obj["name"]; strlcpy(e.name, n  ? n  : "", sizeof(e.name));
        const char *la = obj["lat"];  strlcpy(e.lat,  la ? la : "", sizeof(e.lat));
        const char *lo = obj["lon"];  strlcpy(e.lon,  lo ? lo : "", sizeof(e.lon));
        e.elevation_ft = obj["elev"] | 0;
        const char *tz = obj["tz"];   strlcpy(e.tz_posix, tz ? tz : "", sizeof(e.tz_posix));
      }
    }
  }

  if (fc_loc_count == 0) {
    // Migration: create location[0] from existing NVS lat/lon
    fc_loc_count = 1;
    fc_active_loc = 0;
    strlcpy(fc_locations[0].name, "HOME", sizeof(fc_locations[0].name));
    strlcpy(fc_locations[0].lat,  fc_lat, sizeof(fc_locations[0].lat));
    strlcpy(fc_locations[0].lon,  fc_lon, sizeof(fc_locations[0].lon));
    fc_locations[0].elevation_ft = fc_elevation_ft;
    fcSaveLocations();
    Serial.println("[Portal] Migrated lat/lon to locations.json");
  }

  fc_active_loc = constrain(fc_active_loc, 0, fc_loc_count - 1);
  fcApplyLocation(fc_active_loc);
  Serial.printf("[Portal] Active location %d: %s (%s, %s)\n",
                fc_active_loc, fc_locations[fc_active_loc].name,
                fc_lat, fc_lon);
}

void fcLoadSettings() {
  Preferences prefs;
  prefs.begin("flightcyd", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String lat  = prefs.getString("lat",  "");
  String lon  = prefs.getString("lon",  "");
  fc_radius_km      = prefs.getInt ("radius",     150);
  fc_use_miles      = prefs.getBool("miles",      false);
  fc_invert_display = prefs.getBool("invert_disp", false);
  // Migrate from legacy hide_gnd bool if new filter key is absent
  if (prefs.isKey("filter")) {
    fc_filter_mask = prefs.getUChar("filter", FILTER_ALL) & FILTER_ALL;
  } else {
    bool oldHide = prefs.getBool("hide_gnd", false);
    fc_filter_mask = oldHide ? (FILTER_ALL & ~FILTER_GND) : FILTER_ALL;
  }
  fc_show_labels = prefs.getBool("show_lbl", true);
  fc_elevation_ft    = prefs.getInt ("elev_ft",     0);
  String client_id  = prefs.getString("client_id",  "");
  String client_sec = prefs.getString("client_sec", "");
  String tz         = prefs.getString("tz_posix",   "UTC0");
  prefs.end();

  fc_radius_km = constrain(fc_radius_km, 20, 500);
  ssid.toCharArray(fc_wifi_ssid,     sizeof(fc_wifi_ssid));
  pass.toCharArray(fc_wifi_pass,     sizeof(fc_wifi_pass));
  lat.toCharArray(fc_lat,            sizeof(fc_lat));
  lon.toCharArray(fc_lon,            sizeof(fc_lon));
  client_id.toCharArray(fc_client_id,      sizeof(fc_client_id));
  client_sec.toCharArray(fc_client_secret, sizeof(fc_client_secret));
  tz.toCharArray(fc_tz_posix,              sizeof(fc_tz_posix));
  setenv("TZ", fc_tz_posix, 1);
  tzset();
  fc_has_settings = (ssid.length() > 0);
}

void fcSaveSettings(const char *ssid, const char *pass,
                    const char *lat, const char *lon, int radius, bool use_miles,
                    const char *client_id, const char *client_sec,
                    int elevation_ft, const char *tz_posix,
                    bool invert_display) {
  Preferences prefs;
  prefs.begin("flightcyd", false);
  prefs.putString("ssid",        ssid);
  prefs.putString("pass",        pass);
  prefs.putString("lat",         lat);
  prefs.putString("lon",         lon);
  prefs.putInt   ("radius",      radius);
  prefs.putBool  ("miles",       use_miles);
  prefs.putUChar ("filter",      fc_filter_mask);  // preserve current
  prefs.putBool  ("show_lbl",    fc_show_labels);  // preserve current
  prefs.putBool  ("invert_disp", invert_display);
  prefs.putInt   ("elev_ft",     elevation_ft);
  prefs.putString("client_id",   client_id);
  prefs.putString("client_sec",  client_sec);
  prefs.putString("tz_posix",    tz_posix);
  prefs.end();

  strncpy(fc_wifi_ssid,     ssid,       sizeof(fc_wifi_ssid)     - 1);
  strncpy(fc_wifi_pass,     pass,       sizeof(fc_wifi_pass)     - 1);
  strncpy(fc_lat,           lat,        sizeof(fc_lat)           - 1);
  strncpy(fc_lon,           lon,        sizeof(fc_lon)           - 1);
  strncpy(fc_client_id,     client_id,  sizeof(fc_client_id)     - 1);
  strncpy(fc_client_secret, client_sec, sizeof(fc_client_secret) - 1);
  strncpy(fc_tz_posix,      tz_posix,   sizeof(fc_tz_posix)      - 1);
  fc_radius_km      = radius;
  fc_use_miles      = use_miles;
  fc_invert_display = invert_display;
  fc_elevation_ft   = elevation_ft;
  fc_has_settings   = true;
  setenv("TZ", fc_tz_posix, 1);
  tzset();
}

static void fcShowPortalScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(30, 5);
  gfx->print("FlightRadarCYD Setup");

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(1);
  gfx->setCursor(62, 26);
  gfx->print("Live Aircraft Radar");

  gfx->setTextColor(0xFFE0);
  gfx->setCursor(4, 46);
  gfx->print("1. Connect your phone/PC to WiFi:");
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(22, 58);
  gfx->print("FlightRadarCYD_Setup");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 82);
  gfx->print("2. Open your browser and go to:");
  gfx->setTextColor(0x07FF);
  gfx->setTextSize(2);
  gfx->setCursor(50, 94);
  gfx->print("192.168.4.1");

  gfx->setTextColor(0xFFE0);
  gfx->setTextSize(1);
  gfx->setCursor(4, 118);
  gfx->print("3. Enter WiFi & locations,");
  gfx->setCursor(4, 130);
  gfx->print("   then tap Save & Connect.");

  if (fc_has_settings) {
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 152);
    gfx->print("Existing settings found. Tap");
    gfx->setCursor(4, 164);
    gfx->print("'No Changes' to keep them.");
  }
}

// Shared input style for table cells
static const char *CELL_STYLE =
  "width:100%;box-sizing:border-box;background:#001122;color:#00ccff;"
  "border:1px solid #0066aa;border-radius:4px;padding:5px;font-size:0.9em";

static void fcHandleRoot() {
  String html =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>FlightRadarCYD Setup</title>"
    "<style>"
    "body{background:#000d1a;color:#00ccff;font-family:Arial,sans-serif;"
         "text-align:center;padding:20px;max-width:480px;margin:auto;}"
    "h1{color:#00ffff;font-size:1.6em;margin-bottom:4px;}"
    "p{color:#88aacc;font-size:0.9em;}"
    "label{display:block;text-align:left;margin:14px 0 4px;color:#88ddff;font-weight:bold;}"
    "input,select,textarea{width:100%;box-sizing:border-box;background:#001122;color:#00ccff;"
                           "border:2px solid #0066aa;border-radius:6px;padding:10px;font-size:1em;}"
    ".btn{display:block;width:100%;padding:14px;margin:10px 0;font-size:1.05em;"
         "border-radius:8px;border:none;cursor:pointer;font-weight:bold;}"
    ".btn-save{background:#004488;color:#00ffff;border:2px solid #0099dd;}"
    ".btn-save:hover{background:#0066bb;}"
    ".btn-skip{background:#1a1a2e;color:#667788;border:2px solid #334455;}"
    ".note{color:#445566;font-size:0.82em;margin-top:16px;}"
    "hr{border:1px solid #113355;margin:20px 0;}"
    "th{color:#88ddff;text-align:left;padding:3px 4px;font-size:0.85em;font-weight:normal;}"
    "td{padding:2px 3px;}"
    "</style></head><body>"
    "<h1>&#9992; FlightRadarCYD Setup</h1>"
    "<p>Live aircraft radar for the CYD display.</p>"
    "<form method='post' action='/save'>"
    "<label>WiFi Network Name (SSID):</label>"
    "<input type='text' name='ssid' value='";
  html += String(fc_wifi_ssid);
  html +=
    "' placeholder='Your 2.4 GHz WiFi name' maxlength='63' required>"
    "<label>WiFi Password:</label>"
    "<input type='password' name='pass' value='";
  html += String(fc_wifi_pass);
  html +=
    "' placeholder='Leave blank if open network' maxlength='63'>"

    // ── Locations section ──
    "<label>Favorite Locations (up to 8):</label>"
    "<p style='text-align:left;color:#88aacc;font-size:0.85em;margin:2px 0 8px'>"
    "Name max 8 chars (e.g. HOME, KLAX). First location required. "
    "Leave Name and Lat blank to remove unused slots.</p>"
    "<div style='overflow-x:auto'>"
    "<table style='width:100%;border-collapse:collapse;min-width:480px'>"
    "<thead><tr><th>Name</th><th>Latitude</th><th>Longitude</th><th>Elev ft</th><th>Timezone</th></tr></thead>"
    "<tbody>";

  // Compact timezone labels for in-table dropdown
  struct { const char *label; const char *posix; } zones[] = {
    {"UTC",         "UTC0"},
    {"Eastern",     "EST5EDT,M3.2.0,M11.1.0"},
    {"Central",     "CST6CDT,M3.2.0,M11.1.0"},
    {"Mountain",    "MST7MDT,M3.2.0,M11.1.0"},
    {"AZ (MST)",    "MST7"},
    {"Pacific",     "PST8PDT,M3.2.0,M11.1.0"},
    {"Alaska",      "AKST9AKDT,M3.2.0,M11.1.0"},
    {"Hawaii",      "HST10"},
    {"UK/Ireland",  "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Portugal",    "WET0WEST,M3.5.0/1,M10.5.0"},
    {"C. Europe",   "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"E. Europe",   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  };
  int nZones = (int)(sizeof(zones)/sizeof(zones[0]));

  for (int i = 0; i < MAX_LOCATIONS; i++) {
    const char *name  = (i < fc_loc_count) ? fc_locations[i].name : "";
    const char *lat   = (i < fc_loc_count) ? fc_locations[i].lat  : "";
    const char *lon   = (i < fc_loc_count) ? fc_locations[i].lon  : "";
    int   elev        = (i < fc_loc_count) ? fc_locations[i].elevation_ft : 0;
    const char *locTz = (i < fc_loc_count) ? fc_locations[i].tz_posix : "";
    html += "<tr><td><input name='loc" + String(i) + "_name' maxlength='8' value='";
    html += String(name);
    html += "' style='" + String(CELL_STYLE) + "'></td>";
    html += "<td><input name='loc" + String(i) + "_lat' maxlength='15' value='";
    html += String(lat);
    html += "' placeholder='38.8894' style='" + String(CELL_STYLE) + "'></td>";
    html += "<td><input name='loc" + String(i) + "_lon' maxlength='15' value='";
    html += String(lon);
    html += "' placeholder='-77.035' style='" + String(CELL_STYLE) + "'></td>";
    html += "<td><input type='number' name='loc" + String(i) + "_elev' min='-1500' max='20000' value='";
    html += String(elev);
    html += "' style='" + String(CELL_STYLE) + "'></td>";
    html += "<td><select name='loc" + String(i) + "_tz' style='width:100%;background:#001a33;color:#00ccff;border:1px solid #0066aa;border-radius:4px;padding:2px'>";
    html += "<option value=''>— default —</option>";
    for (int z = 0; z < nZones; z++) {
      html += "<option value='"; html += zones[z].posix; html += "'";
      if (locTz[0] != '\0' && strcmp(locTz, zones[z].posix) == 0) html += " selected";
      html += ">"; html += zones[z].label; html += "</option>";
    }
    html += "</select></td></tr>";
  }
  html += "</tbody></table></div>";

  // ── Global / default timezone ──
  html +=
    "<label>Default Timezone:</label>"
    "<p style='text-align:left;color:#88aacc;font-size:0.85em;margin:2px 0 8px'>"
    "Used for locations set to \"&#8212; default &#8212;\" above, and for the stats daily reset.</p>"
    "<select name='tz_posix'>";
  for (int i = 0; i < nZones; i++) {
    html += "<option value='"; html += zones[i].posix; html += "'";
    if (strcmp(fc_tz_posix, zones[i].posix) == 0) html += " selected";
    html += ">"; html += zones[i].label; html += " &mdash; "; html += zones[i].posix; html += "</option>";
  }
  html += "</select>";
  html +=
    "<label>Distance Units:</label>"
    "<p style='text-align:left;color:#88aacc;font-size:0.85em;margin:2px 0 8px'>"
    "Scan radius and hide-ground are configured on the device (long-press BOOT).</p>"
    "<select name='units'>"
    "<option value='km'";
  if (!fc_use_miles) html += " selected";
  html +=
    ">Kilometers (km)</option>"
    "<option value='miles'";
  if (fc_use_miles) html += " selected";
  html +=
    ">Miles (mi)</option>"
    "</select>"
    "<label>Display Options:</label>"
    "<label style='font-weight:normal;display:flex;align-items:center;gap:8px'>"
    "<input type='checkbox' name='invert_display' value='1' style='width:auto'";
  if (fc_invert_display) html += " checked";
  html +=
    "> Invert display colors (some CYD hardware variants require this)</label>"
    "<hr>"
    "<p style='text-align:left;color:#88aacc;margin:0 0 8px'>"
    "&#128274; OpenSky OAuth2 credentials (optional) — enables 30-second refresh"
    " vs 4&nbsp;min anonymous. Paste your credentials.json or enter fields directly.</p>"
    "<label>Paste credentials.json:</label>"
    "<textarea id='cred_json' rows='3' oninput='parseCred()'"
    " style='width:100%;box-sizing:border-box;background:#001122;color:#00ccff;"
    "border:2px solid #0066aa;border-radius:6px;padding:10px;font-size:0.8em;"
    "font-family:monospace;resize:vertical'"
    " placeholder='{\"clientId\":\"...\",\"clientSecret\":\"...\"}'></textarea>"
    "<label>Client ID:</label>"
    "<input type='text' id='client_id' name='client_id' value='";
  html += String(fc_client_id);
  html +=
    "' placeholder='Leave blank for anonymous access' maxlength='79'>"
    "<label>Client Secret:</label>"
    "<input type='password' id='client_secret' name='client_secret' value='";
  html += String(fc_client_secret);
  html +=
    "' placeholder='Leave blank for anonymous access' maxlength='63'>"
    "<script>"
    "function parseCred(){"
    "try{"
    "var j=JSON.parse(document.getElementById('cred_json').value.trim());"
    "if(j.clientId)document.getElementById('client_id').value=j.clientId;"
    "if(j.clientSecret)document.getElementById('client_secret').value=j.clientSecret;"
    "}catch(e){}}"
    "</script>"
    "<br><button class='btn btn-save' type='submit'>&#128190; Save &amp; Connect</button>"
    "</form>";
  if (fc_has_settings) {
    html +=
      "<hr>"
      "<form method='post' action='/nochange'>"
      "<button class='btn btn-skip' type='submit'>&#10006; No Changes &mdash; Use Current Settings</button>"
      "</form>";
  }
  html +=
    "<hr>"
    "<form method='post' action='/reset'>"
    "<button type='submit'"
    " style='width:100%;padding:12px;background:#1a0000;color:#ff5555;"
    "border:2px solid #ff3333;border-radius:8px;font-size:1em;cursor:pointer;'>"
    "&#9888; Factory Reset &mdash; Erase All Settings &amp; Stats"
    "</button>"
    "</form>"
    "<p class='note'>&#9888; ESP32 supports 2.4 GHz WiFi only. "
    "OpenSky: anonymous 400 req/day (4-min refresh) or account 4,000 req/day (30-sec refresh).</p>"
    "</body></html>";
  portalServer->send(200, "text/html", html);
}

static void fcHandleSave() {
  String ssid       = portalServer->hasArg("ssid")   ? portalServer->arg("ssid")           : "";
  String pass       = portalServer->hasArg("pass")   ? portalServer->arg("pass")           : "";
  bool use_miles      = portalServer->hasArg("units") && portalServer->arg("units") == "miles";
  int  radius         = fc_radius_km;   // managed via on-device settings overlay
  String client_id    = portalServer->hasArg("client_id")     ? portalServer->arg("client_id")     : "";
  String client_sec   = portalServer->hasArg("client_secret") ? portalServer->arg("client_secret") : "";
  bool invert_display = portalServer->hasArg("invert_display");
  String tz_posix   = portalServer->hasArg("tz_posix") ? portalServer->arg("tz_posix") : "UTC0";
  if (tz_posix.length() == 0 || tz_posix.length() >= 64) tz_posix = "UTC0";

  if (ssid.length() == 0) {
    portalServer->send(400, "text/html",
      "<html><body style='background:#000d1a;color:#ff5555;font-family:Arial;"
      "text-align:center;padding:40px'>"
      "<h2>&#10060; SSID cannot be empty!</h2>"
      "<a href='/' style='color:#00ccff'>&#8592; Go Back</a></body></html>");
    return;
  }

  // Parse location entries from form
  LocationEntry newLoc[MAX_LOCATIONS] = {};
  int newLocCount = 0;
  for (int i = 0; i < MAX_LOCATIONS; i++) {
    String nameArg = "loc" + String(i) + "_name";
    if (!portalServer->hasArg(nameArg)) continue;
    String name = portalServer->arg(nameArg); name.trim();
    String lat  = portalServer->arg("loc" + String(i) + "_lat"); lat.trim();
    // Skip empty non-first slots
    if (i > 0 && name.length() == 0 && lat.length() == 0) continue;
    if (name.length() == 0) name = (i == 0) ? "HOME" : ("LOC" + String(i + 1));
    String lon  = portalServer->arg("loc" + String(i) + "_lon"); lon.trim();
    int elev    = portalServer->arg("loc" + String(i) + "_elev").toInt();
    elev = constrain(elev, -1500, 20000);
    strncpy(newLoc[newLocCount].name, name.c_str(), 8); newLoc[newLocCount].name[8] = '\0';
    strncpy(newLoc[newLocCount].lat,  lat.c_str(),  15); newLoc[newLocCount].lat[15] = '\0';
    strncpy(newLoc[newLocCount].lon,  lon.c_str(),  15); newLoc[newLocCount].lon[15] = '\0';
    newLoc[newLocCount].elevation_ft = elev;
    {
      String locTz = portalServer->arg("loc" + String(i) + "_tz"); locTz.trim();
      if (locTz.length() > 0 && locTz.length() < 32)
        strncpy(newLoc[newLocCount].tz_posix, locTz.c_str(), 31);
      else
        newLoc[newLocCount].tz_posix[0] = '\0';
    }
    newLocCount++;
  }
  if (newLocCount == 0) newLocCount = 1;

  // Detect active-location change (for stats reset)
  bool locationChanged = fc_has_settings &&
    (strcmp(newLoc[0].lat, fc_lat) != 0 ||
     strcmp(newLoc[0].lon, fc_lon) != 0 ||
     fc_active_loc != 0);

  // Update location globals and apply location[0]
  memcpy(fc_locations, newLoc, sizeof(LocationEntry) * newLocCount);
  for (int i = newLocCount; i < MAX_LOCATIONS; i++) fc_locations[i] = {};
  fc_loc_count  = newLocCount;
  fc_active_loc = 0;
  fcApplyLocation(0);

  // Save WiFi/radius/etc to NVS (lat/lon/elev saved from location[0])
  fcSaveSettings(ssid.c_str(), pass.c_str(), fc_lat, fc_lon, radius, use_miles,
                 client_id.c_str(), client_sec.c_str(),
                 fc_elevation_ft, tz_posix.c_str(), invert_display);

  // Save locations to LittleFS
  fcSaveLocations();

  if (locationChanged) {
    setStatsLocation(0);
    resetStats();
    Serial.printf("[Portal] Location changed to %s,%s — stats reset\n", fc_lat, fc_lon);
  }

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#000d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}p{color:#88aacc;}</style>"
    "</head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>Connecting to <b>" + ssid + "</b>...</p>"
    "<p>Distance units: <b>" + String(use_miles ? "miles" : "km") + "</b></p>"
    "<p>Refresh: <b>" + (client_id.length() > 0 ? "30 sec (authenticated)" : "4 min (anonymous)") + "</b></p>"
    "<p>You can close this page and disconnect from <b>FlightRadarCYD_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
}

static void fcHandleReset() {
  // Show server-side confirmation page — no JS required
  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#000d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#ff9955;}p{color:#88aacc;}"
    "a{color:#00ccff;}</style>"
    "</head><body>"
    "<h2>&#9888; Confirm Factory Reset</h2>"
    "<p>This will erase <b style='color:#ff5555'>ALL settings and stats</b>.<br>"
    "The device will restart and need to be reconfigured.</p>"
    "<form method='post' action='/do_reset'>"
    "<button type='submit'"
    " style='width:100%;padding:14px;background:#1a0000;color:#ff5555;"
    "border:2px solid #ff3333;border-radius:8px;font-size:1.1em;cursor:pointer;margin-bottom:16px'>"
    "&#9889; Yes, Erase Everything"
    "</button>"
    "</form>"
    "<a href='/'>&#8592; Cancel &mdash; Go Back</a>"
    "</body></html>");
}

static void fcHandleDoReset() {
  // Clear main settings namespace
  { Preferences p; p.begin("flightcyd", false); p.clear(); p.end(); }
  // Clear per-location stats namespaces
  for (int i = 0; i < MAX_LOCATIONS; i++) {
    char ns[10]; snprintf(ns, sizeof(ns), "stats%d", i);
    Preferences p; p.begin(ns, false); p.clear(); p.end();
  }
  // Remove LittleFS files
  LittleFS.remove("/locations.json");
  for (int i = 0; i < MAX_LOCATIONS; i++) {
    char path[20];
    snprintf(path, sizeof(path), "/seen%d.bin", i);    LittleFS.remove(path);
    snprintf(path, sizeof(path), "/hr_seen%d.bin", i); LittleFS.remove(path);
  }
  Serial.println("[Portal] Factory reset — all settings and stats cleared");
  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#000d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#ff5555;}p{color:#88aacc;}</style>"
    "</head><body>"
    "<h2>&#9889; Factory Reset Complete</h2>"
    "<p>All settings and stats have been cleared.</p>"
    "<p>Device is restarting &mdash; reconnect to <b>FlightRadarCYD_Setup</b> to reconfigure.</p>"
    "</body></html>");
  delay(2000);
  ESP.restart();
}

static void fcHandleNoChange() {
  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#000d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}p{color:#88aacc;}</style>"
    "</head><body>"
    "<h2>&#128077; No Changes</h2>"
    "<p>Using saved settings. Device is connecting now.</p>"
    "<p>You can close this page and disconnect from <b>FlightRadarCYD_Setup</b>.</p>"
    "</body></html>");
  delay(1500);
  portalDone = true;
}

void fcInitPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FlightRadarCYD_Setup", "");
  delay(500);

  portalDNS    = new DNSServer();
  portalServer = new WebServer(80);

  portalDNS->start(53, "*", WiFi.softAPIP());
  portalServer->on("/",         fcHandleRoot);
  portalServer->on("/save",     HTTP_POST, fcHandleSave);
  portalServer->on("/nochange", HTTP_POST, fcHandleNoChange);
  portalServer->on("/reset",    HTTP_POST, fcHandleReset);
  portalServer->on("/do_reset", HTTP_POST, fcHandleDoReset);
  portalServer->onNotFound(fcHandleRoot);
  portalServer->begin();
  portalDone = false;

  fcShowPortalScreen();
  Serial.printf("[Portal] AP up — connect to FlightRadarCYD_Setup, open %s\n",
                WiFi.softAPIP().toString().c_str());
}

void fcRunPortal() {
  portalDNS->processNextRequest();
  portalServer->handleClient();
}

void fcClosePortal() {
  portalServer->stop();
  portalDNS->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
  delete portalServer; portalServer = nullptr;
  delete portalDNS;    portalDNS    = nullptr;
}
