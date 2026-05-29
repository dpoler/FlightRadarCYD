#pragma once

#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

extern Arduino_GFX *gfx;

// ---------------------------------------------------------------------------
// Persisted settings
// ---------------------------------------------------------------------------
static char fc_wifi_ssid[64]  = "";
static char fc_wifi_pass[64]  = "";
static char fc_lat[16]        = "";
static char fc_lon[16]        = "";
static int  fc_radius_km      = 150;   // default scan radius, always stored in km
static bool fc_use_miles      = false; // display distances in miles
static int  fc_elevation_ft   = 0;     // location elevation in feet (MSL) for altitude coloring
static char fc_client_id[80]     = ""; // optional OpenSky OAuth2 credentials
static char fc_client_secret[64] = ""; // enables 30-second refresh
static bool fc_hide_ground    = false; // filter out on-ground aircraft
static char fc_tz_posix[64]   = "UTC0"; // POSIX TZ string
static bool fc_has_settings   = false;

// ---------------------------------------------------------------------------
// Portal state
// ---------------------------------------------------------------------------
static WebServer *portalServer = nullptr;
static DNSServer *portalDNS    = nullptr;
static bool       portalDone   = false;

// ---------------------------------------------------------------------------
// NVS helpers
// ---------------------------------------------------------------------------
static void fcLoadSettings() {
  Preferences prefs;
  prefs.begin("flightcyd", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("pass", "");
  String lat  = prefs.getString("lat",  "");
  String lon  = prefs.getString("lon",  "");
  fc_radius_km    = prefs.getInt ("radius",   150);
  fc_use_miles    = prefs.getBool("miles",    false);
  fc_hide_ground  = prefs.getBool("hide_gnd", false);
  fc_elevation_ft = prefs.getInt ("elev_ft",  0);
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

static void fcSaveSettings(const char *ssid, const char *pass,
                           const char *lat, const char *lon, int radius, bool use_miles,
                           const char *client_id, const char *client_sec,
                           bool hide_ground, int elevation_ft, const char *tz_posix) {
  Preferences prefs;
  prefs.begin("flightcyd", false);
  prefs.putString("ssid",       ssid);
  prefs.putString("pass",       pass);
  prefs.putString("lat",        lat);
  prefs.putString("lon",        lon);
  prefs.putInt   ("radius",     radius);
  prefs.putBool  ("miles",      use_miles);
  prefs.putBool  ("hide_gnd",   hide_ground);
  prefs.putInt   ("elev_ft",    elevation_ft);
  prefs.putString("client_id",  client_id);
  prefs.putString("client_sec", client_sec);
  prefs.putString("tz_posix",   tz_posix);
  prefs.end();

  strncpy(fc_wifi_ssid,     ssid,       sizeof(fc_wifi_ssid)     - 1);
  strncpy(fc_wifi_pass,     pass,       sizeof(fc_wifi_pass)     - 1);
  strncpy(fc_lat,           lat,        sizeof(fc_lat)           - 1);
  strncpy(fc_lon,           lon,        sizeof(fc_lon)           - 1);
  strncpy(fc_client_id,     client_id,  sizeof(fc_client_id)     - 1);
  strncpy(fc_client_secret, client_sec, sizeof(fc_client_secret) - 1);
  strncpy(fc_tz_posix,      tz_posix,   sizeof(fc_tz_posix)      - 1);
  fc_radius_km    = radius;
  fc_use_miles    = use_miles;
  fc_hide_ground  = hide_ground;
  fc_elevation_ft = elevation_ft;
  fc_has_settings = true;
  setenv("TZ", fc_tz_posix, 1);
  tzset();
}

// ---------------------------------------------------------------------------
// On-screen setup instructions
// ---------------------------------------------------------------------------
static void fcShowPortalScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(0x07FF);  // cyan
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
  gfx->print("3. Enter WiFi, location & radius,");
  gfx->setCursor(4, 130);
  gfx->print("   then tap  Save & Connect.");

  if (fc_has_settings) {
    gfx->setTextColor(0x07E0);
    gfx->setCursor(4, 152);
    gfx->print("Existing settings found. Tap");
    gfx->setCursor(4, 164);
    gfx->print("'No Changes' to keep them.");
  }
}

// ---------------------------------------------------------------------------
// Web handlers
// ---------------------------------------------------------------------------
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
    "input,select{width:100%;box-sizing:border-box;background:#001122;color:#00ccff;"
                 "border:2px solid #0066aa;border-radius:6px;padding:10px;font-size:1em;}"
    ".btn{display:block;width:100%;padding:14px;margin:10px 0;font-size:1.05em;"
         "border-radius:8px;border:none;cursor:pointer;font-weight:bold;}"
    ".btn-save{background:#004488;color:#00ffff;border:2px solid #0099dd;}"
    ".btn-save:hover{background:#0066bb;}"
    ".btn-skip{background:#1a1a2e;color:#667788;border:2px solid #334455;}"
    ".note{color:#445566;font-size:0.82em;margin-top:16px;}"
    "hr{border:1px solid #113355;margin:20px 0;}"
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
    "<label>Your Latitude:</label>"
    "<input type='text' name='lat' value='";
  html += String(fc_lat);
  html +=
    "' placeholder='e.g. 38.8894' maxlength='15'>"
    "<label>Your Longitude:</label>"
    "<input type='text' name='lon' value='";
  html += String(fc_lon);
  html +=
    "' placeholder='e.g. -77.0352' maxlength='15'>"
    "<label>Your Elevation (feet MSL):</label>"
    "<input type='number' name='elevation' min='-1500' max='20000' value='";
  html += String(fc_elevation_ft);
  html +=
    "' placeholder='e.g. 5430 for Denver, 0 for sea level'>"
    "<label>Time Zone:</label>"
    "<p style='text-align:left;color:#88aacc;font-size:0.85em;margin:2px 0 8px'>"
    "Used for the stats screen daily reset (midnight in your local time).</p>";
  {
    struct { const char *label; const char *posix; } zones[] = {
      {"UTC",                              "UTC0"},
      {"US Eastern (EST/EDT)",             "EST5EDT,M3.2.0,M11.1.0"},
      {"US Central (CST/CDT)",             "CST6CDT,M3.2.0,M11.1.0"},
      {"US Mountain (MST/MDT)",            "MST7MDT,M3.2.0,M11.1.0"},
      {"US Mountain, Arizona (MST)",       "MST7"},
      {"US Pacific (PST/PDT)",             "PST8PDT,M3.2.0,M11.1.0"},
      {"Alaska (AKST/AKDT)",              "AKST9AKDT,M3.2.0,M11.1.0"},
      {"Hawaii (HST)",                     "HST10"},
      {"UK & Ireland (GMT/BST)",           "GMT0BST,M3.5.0/1,M10.5.0"},
      {"Portugal (WET/WEST)",              "WET0WEST,M3.5.0/1,M10.5.0"},
      {"Central Europe (CET/CEST)",        "CET-1CEST,M3.5.0,M10.5.0/3"},
      {"Eastern Europe (EET/EEST)",        "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    };
    html += "<select name='tz_posix'>";
    for (int i = 0; i < (int)(sizeof(zones)/sizeof(zones[0])); i++) {
      html += "<option value='";
      html += zones[i].posix;
      html += "'";
      if (strcmp(fc_tz_posix, zones[i].posix) == 0) html += " selected";
      html += ">";
      html += zones[i].label;
      html += "</option>";
    }
    html += "</select>";
  }
  html +=
    "<label>Distance Units:</label>"
    "<select name='units' id='units' onchange='swapRadii()'>"
    "<option value='km'";
  if (!fc_use_miles) html += " selected";
  html +=
    ">Kilometers (km)</option>"
    "<option value='miles'";
  if (fc_use_miles) html += " selected";
  html +=
    ">Miles (mi)</option>"
    "</select>"
    "<label>Scan Radius:</label>"
    "<p style='text-align:left;color:#88aacc;font-size:0.85em;margin:2px 0 8px'>"
    "Inner rings appear at 1/3 and 2/3 of this distance. "
    "Large values make the display cluttered. "
    "OpenSky API maximum: 500 km / 310 mi.</p>"
    "<select name='radius' id='radius_sel' onchange='checkCustom()'>";
  // Determine whether current saved value matches a preset
  {
    int kmRadii[] = { 25, 50, 100 };
    bool kmMatch = false;
    for (int i = 0; i < 3; i++) if (kmRadii[i] == fc_radius_km) kmMatch = true;

    int miRadii[] = { 15, 30, 60 };
    int curMiRound = (int)(fc_radius_km * 0.621371f + 0.5f);
    int miMatchIdx = -1;
    for (int i = 0; i < 3; i++) {
      int d = curMiRound - miRadii[i]; if (d < 0) d = -d;
      if (d <= 2) { miMatchIdx = i; break; }
    }

    bool showCustom = fc_use_miles ? (miMatchIdx < 0) : !kmMatch;
    int  customVal  = fc_use_miles ? curMiRound : fc_radius_km;

    if (!fc_use_miles) {
      for (int i = 0; i < 3; i++) {
        html += "<option value='" + String(kmRadii[i]) + "'";
        if (kmRadii[i] == fc_radius_km) html += " selected";
        html += ">" + String(kmRadii[i]) + " km</option>";
      }
    } else {
      for (int i = 0; i < 3; i++) {
        html += "<option value='" + String(miRadii[i]) + "'";
        if (i == miMatchIdx) html += " selected";
        html += ">" + String(miRadii[i]) + " mi</option>";
      }
    }
    html += "<option value='0'";
    if (showCustom) html += " selected";
    html += ">Custom...</option>"
            "</select>"
            "<div id='custom_div' style='margin-top:8px;display:";
    html += showCustom ? "block" : "none";
    html += "'>"
            "<input type='number' id='radius_custom'"
            " min='"; html += fc_use_miles ? "15" : "20";
    html += "' max='"; html += fc_use_miles ? "310" : "500";
    html += "' value='";
    if (showCustom) html += String(customVal);
    html += "' placeholder='Enter value'></div>";
  }
  html +=
    "<script>"
    "var kO='<option value=25>25 km</option><option value=50>50 km</option>"
    "<option value=100>100 km</option><option value=0>Custom...</option>';"
    "var mO='<option value=15>15 mi</option><option value=30>30 mi</option>"
    "<option value=60>60 mi</option><option value=0>Custom...</option>';"
    "function swapRadii(){"
    "var miles=document.getElementById('units').value==='miles';"
    "document.getElementById('radius_sel').innerHTML=miles?mO:kO;"
    "var ci=document.getElementById('radius_custom');"
    "ci.min=miles?15:20;ci.max=miles?310:500;ci.value='';"
    "checkCustom();}"
    "function checkCustom(){"
    "var show=document.getElementById('radius_sel').value==='0';"
    "document.getElementById('custom_div').style.display=show?'block':'none';"
    "if(show)document.getElementById('radius_custom').focus();}"
    "document.forms[0].addEventListener('submit',function(e){"
    "var sel=document.getElementById('radius_sel');"
    "if(sel.value==='0'){"
    "var v=parseInt(document.getElementById('radius_custom').value);"
    "if(!v||v<1){alert('Please enter a custom radius.');e.preventDefault();return;}"
    "sel.value=v;}});"
    "</script>"
    "<label>Filter Options:</label>"
    "<label style='font-weight:normal;display:flex;align-items:center;gap:8px'>"
    "<input type='checkbox' name='hide_ground' value='1' style='width:auto'";
  if (fc_hide_ground) html += " checked";
  html +=
    "> Hide aircraft on ground</label>"
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
    "<p class='note'>&#9888; ESP32 supports 2.4 GHz WiFi only. "
    "OpenSky: anonymous 400 req/day (4-min refresh) or account 4,000 req/day (30-sec refresh).</p>"
    "</body></html>";
  portalServer->send(200, "text/html", html);
}

static void fcHandleSave() {
  String ssid   = portalServer->hasArg("ssid")   ? portalServer->arg("ssid")           : "";
  String pass   = portalServer->hasArg("pass")   ? portalServer->arg("pass")           : "";
  String lat    = portalServer->hasArg("lat")    ? portalServer->arg("lat")            : "";
  String lon    = portalServer->hasArg("lon")    ? portalServer->arg("lon")            : "";
  bool   use_miles  = portalServer->hasArg("units") && portalServer->arg("units") == "miles";
  int    radius_raw = portalServer->hasArg("radius") ? portalServer->arg("radius").toInt() : 150;
  int    radius     = use_miles ? (int)(radius_raw * 1.60934f + 0.5f) : radius_raw;
  radius = constrain(radius, 20, 500);
  String client_id  = portalServer->hasArg("client_id")     ? portalServer->arg("client_id")     : "";
  String client_sec = portalServer->hasArg("client_secret") ? portalServer->arg("client_secret") : "";
  bool   hide_ground   = portalServer->hasArg("hide_ground");
  int    elevation_ft  = portalServer->hasArg("elevation") ? portalServer->arg("elevation").toInt() : 0;
  elevation_ft = constrain(elevation_ft, -1500, 20000);
  String tz_posix = portalServer->hasArg("tz_posix") ? portalServer->arg("tz_posix") : "UTC0";
  if (tz_posix.length() == 0 || tz_posix.length() >= 64) tz_posix = "UTC0";

  if (ssid.length() == 0) {
    portalServer->send(400, "text/html",
      "<html><body style='background:#000d1a;color:#ff5555;font-family:Arial;"
      "text-align:center;padding:40px'>"
      "<h2>&#10060; SSID cannot be empty!</h2>"
      "<a href='/' style='color:#00ccff'>&#8592; Go Back</a></body></html>");
    return;
  }

  fcSaveSettings(ssid.c_str(), pass.c_str(), lat.c_str(), lon.c_str(), radius, use_miles,
                 client_id.c_str(), client_sec.c_str(), hide_ground, elevation_ft, tz_posix.c_str());

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#000d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}p{color:#88aacc;}</style>"
    "</head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>Connecting to <b>" + ssid + "</b>...</p>"
    "<p>Scan radius: <b>" + String(radius_raw) + (use_miles ? " mi" : " km") + "</b></p>"
    "<p>Refresh: <b>" + (client_id.length() > 0 ? "30 sec (authenticated)" : "4 min (anonymous)") + "</b></p>"
    "<p>You can close this page and disconnect from <b>FlightRadarCYD_Setup</b>.</p>"
    "</body></html>");

  delay(1500);
  portalDone = true;
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

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void fcInitPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FlightRadarCYD_Setup", "");
  delay(500);

  portalDNS    = new DNSServer();
  portalServer = new WebServer(80);

  portalDNS->start(53, "*", WiFi.softAPIP());
  portalServer->on("/",         fcHandleRoot);
  portalServer->on("/save",     HTTP_POST, fcHandleSave);
  portalServer->on("/nochange", HTTP_POST, fcHandleNoChange);
  portalServer->onNotFound(fcHandleRoot);
  portalServer->begin();
  portalDone = false;

  fcShowPortalScreen();
  Serial.printf("[Portal] AP up — connect to FlightRadarCYD_Setup, open %s\n",
                WiFi.softAPIP().toString().c_str());
}

static void fcRunPortal() {
  portalDNS->processNextRequest();
  portalServer->handleClient();
}

static void fcClosePortal() {
  portalServer->stop();
  portalDNS->stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  delay(300);
  delete portalServer; portalServer = nullptr;
  delete portalDNS;    portalDNS    = nullptr;
}
