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
static int  fc_radius_km      = 150;   // default scan radius
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
  fc_radius_km = prefs.getInt("radius", 150);
  prefs.end();

  fc_radius_km = constrain(fc_radius_km, 50, 500);
  ssid.toCharArray(fc_wifi_ssid, sizeof(fc_wifi_ssid));
  pass.toCharArray(fc_wifi_pass, sizeof(fc_wifi_pass));
  lat.toCharArray(fc_lat, sizeof(fc_lat));
  lon.toCharArray(fc_lon, sizeof(fc_lon));
  fc_has_settings = (ssid.length() > 0);
}

static void fcSaveSettings(const char *ssid, const char *pass,
                           const char *lat, const char *lon, int radius) {
  Preferences prefs;
  prefs.begin("flightcyd", false);
  prefs.putString("ssid",   ssid);
  prefs.putString("pass",   pass);
  prefs.putString("lat",    lat);
  prefs.putString("lon",    lon);
  prefs.putInt   ("radius", radius);
  prefs.end();

  strncpy(fc_wifi_ssid, ssid, sizeof(fc_wifi_ssid) - 1);
  strncpy(fc_wifi_pass, pass, sizeof(fc_wifi_pass) - 1);
  strncpy(fc_lat, lat,  sizeof(fc_lat) - 1);
  strncpy(fc_lon, lon,  sizeof(fc_lon) - 1);
  fc_radius_km    = radius;
  fc_has_settings = true;
}

// ---------------------------------------------------------------------------
// On-screen setup instructions
// ---------------------------------------------------------------------------
static void fcShowPortalScreen() {
  gfx->fillScreen(RGB565_BLACK);

  gfx->setTextColor(0x07FF);  // cyan
  gfx->setTextSize(2);
  gfx->setCursor(30, 5);
  gfx->print("FlightCYD Setup");

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
  gfx->print("FlightCYD_Setup");

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
    "<title>FlightCYD Setup</title>"
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
    "<h1>&#9992; FlightCYD Setup</h1>"
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
    "<label>Scan Radius:</label>"
    "<select name='radius'>";
  int radii[] = { 50, 100, 150, 200, 300 };
  for (int i = 0; i < 5; i++) {
    html += "<option value='" + String(radii[i]) + "'";
    if (radii[i] == fc_radius_km) html += " selected";
    html += ">" + String(radii[i]) + " km</option>";
  }
  html +=
    "</select>"
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
    "OpenSky anonymous access: ~400 requests/day. Radar refreshes every 4 min.</p>"
    "</body></html>";
  portalServer->send(200, "text/html", html);
}

static void fcHandleSave() {
  String ssid   = portalServer->hasArg("ssid")   ? portalServer->arg("ssid")           : "";
  String pass   = portalServer->hasArg("pass")   ? portalServer->arg("pass")           : "";
  String lat    = portalServer->hasArg("lat")    ? portalServer->arg("lat")            : "";
  String lon    = portalServer->hasArg("lon")    ? portalServer->arg("lon")            : "";
  int    radius = portalServer->hasArg("radius") ? portalServer->arg("radius").toInt() : 150;
  radius = constrain(radius, 50, 500);

  if (ssid.length() == 0) {
    portalServer->send(400, "text/html",
      "<html><body style='background:#000d1a;color:#ff5555;font-family:Arial;"
      "text-align:center;padding:40px'>"
      "<h2>&#10060; SSID cannot be empty!</h2>"
      "<a href='/' style='color:#00ccff'>&#8592; Go Back</a></body></html>");
    return;
  }

  fcSaveSettings(ssid.c_str(), pass.c_str(), lat.c_str(), lon.c_str(), radius);

  portalServer->send(200, "text/html",
    "<html><head><meta charset='UTF-8'>"
    "<style>body{background:#000d1a;color:#00ccff;font-family:Arial;"
    "text-align:center;padding:40px;}h2{color:#00ffff;}p{color:#88aacc;}</style>"
    "</head><body>"
    "<h2>&#9989; Settings Saved!</h2>"
    "<p>Connecting to <b>" + ssid + "</b>...</p>"
    "<p>Scan radius: <b>" + String(radius) + " km</b></p>"
    "<p>You can close this page and disconnect from <b>FlightCYD_Setup</b>.</p>"
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
    "<p>You can close this page and disconnect from <b>FlightCYD_Setup</b>.</p>"
    "</body></html>");
  delay(1500);
  portalDone = true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static void fcInitPortal() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("FlightCYD_Setup", "");
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
  Serial.printf("[Portal] AP up — connect to FlightCYD_Setup, open %s\n",
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
