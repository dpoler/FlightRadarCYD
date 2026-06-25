#pragma once
#include "version.h"

#define GITHUB_OWNER "dpoler"
#define GITHUB_REPO  "FlightRadarCYD"

enum OtaStatus {
    OTA_IDLE,
    OTA_CHECKING,
    OTA_UP_TO_DATE,
    OTA_AVAILABLE,
    OTA_DOWNLOADING,
    OTA_DONE,
    OTA_ERROR
};

extern volatile OtaStatus ota_status;
extern char     ota_latest_tag[16];
extern volatile int ota_progress;   // 0-100 during download

void otaCheck();   // start background version-check task
void otaUpdate();  // start background download+flash task (only when OTA_AVAILABLE)
