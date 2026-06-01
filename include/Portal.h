#pragma once

extern char fc_wifi_ssid[64];
extern char fc_wifi_pass[64];
extern char fc_lat[16];
extern char fc_lon[16];
extern int  fc_radius_km;
extern bool fc_use_miles;
extern int  fc_elevation_ft;
extern char fc_client_id[80];
extern char fc_client_secret[64];
extern bool fc_hide_ground;
extern bool fc_invert_display;
extern char fc_tz_posix[64];
extern bool fc_has_settings;
extern bool portalDone;

void fcLoadSettings();
void fcSaveSettings(const char *ssid, const char *pass,
                    const char *lat, const char *lon, int radius, bool use_miles,
                    const char *client_id, const char *client_sec,
                    bool hide_ground, int elevation_ft, const char *tz_posix,
                    bool invert_display);
void fcInitPortal();
void fcRunPortal();
void fcClosePortal();
