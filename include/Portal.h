#pragma once
#include <stdint.h>

#define MAX_LOCATIONS 8

struct LocationEntry {
    char name[9];
    char lat[16];
    char lon[16];
    int  elevation_ft;
};

extern char          fc_wifi_ssid[64];
extern char          fc_wifi_pass[64];
extern char          fc_lat[16];
extern char          fc_lon[16];
extern int           fc_radius_km;
extern bool          fc_use_miles;
extern int           fc_elevation_ft;
extern char          fc_client_id[80];
extern char          fc_client_secret[64];
#define FILTER_GND   0x01   // on ground
#define FILTER_CLB   0x02   // climbing  (vert_ms >= +2 m/s, ≤10 000 ft AGL)
#define FILTER_DSC   0x04   // descending (vert_ms <= -2 m/s, ≤10 000 ft AGL)
#define FILTER_HI    0x08   // high altitude (>10 000 ft AGL)
#define FILTER_LO    0x10   // low altitude (≤10 000 ft AGL), level
#define FILTER_ALL   0x1F   // all categories shown
extern uint8_t       fc_filter_mask;
extern bool          fc_show_labels;
extern bool          fc_invert_display;
extern char          fc_tz_posix[64];
extern bool          fc_has_settings;
extern bool          portalDone;
extern LocationEntry fc_locations[MAX_LOCATIONS];
extern int           fc_loc_count;
extern int           fc_active_loc;

void fcLoadSettings();
void fcSaveSettings(const char *ssid, const char *pass,
                    const char *lat, const char *lon, int radius, bool use_miles,
                    const char *client_id, const char *client_sec,
                    int elevation_ft, const char *tz_posix,
                    bool invert_display);
void fcLoadLocations();
void fcSaveLocations();
void fcApplyLocation(int idx);
void fcInitPortal();
void fcRunPortal();
void fcClosePortal();
