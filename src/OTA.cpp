#include "OTA.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <Arduino.h>

volatile OtaStatus ota_status  = OTA_IDLE;
char     ota_latest_tag[16]    = "";
volatile int ota_progress      = 0;

static void checkTask(void *) {
    // Network objects in a nested scope so their destructors run before vTaskDelete.
    // vTaskDelete() terminates the task immediately, skipping any C++ destructors
    // still on the stack — which leaks the mbedTLS heap those objects hold internally.
    {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client,
            "https://api.github.com/repos/" GITHUB_OWNER "/" GITHUB_REPO "/releases/latest");
        http.addHeader("User-Agent", "FlightRadarCYD");
        http.addHeader("Accept", "application/vnd.github+json");

        int code = http.GET();
        if (code == 200) {
            StaticJsonDocument<32> filter;
            filter["tag_name"] = true;
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, http.getStream(),
                    DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
                const char *tag = doc["tag_name"];
                if (tag) {
                    strncpy(ota_latest_tag, tag, sizeof(ota_latest_tag) - 1);
                    ota_latest_tag[sizeof(ota_latest_tag) - 1] = '\0';
                    ota_status = (strcmp(ota_latest_tag, FIRMWARE_VERSION_STR) == 0)
                                 ? OTA_UP_TO_DATE : OTA_AVAILABLE;
                } else {
                    ota_status = OTA_ERROR;
                }
            } else {
                ota_status = OTA_ERROR;
            }
        } else {
            ota_status = OTA_ERROR;
        }
        http.end();
    }  // WiFiClientSecure and HTTPClient destroyed here — mbedTLS heap released
    vTaskDelete(NULL);
}

void otaCheck() {
    if (ota_status == OTA_CHECKING || ota_status == OTA_DOWNLOADING) return;
    ota_status = OTA_CHECKING;
    xTaskCreatePinnedToCore(checkTask, "ota_chk", 8192, NULL, 1, NULL, 0);
}

static void updateTask(void *) {
    ota_status   = OTA_DOWNLOADING;
    ota_progress = 0;

    char url[128];
    snprintf(url, sizeof(url),
        "https://github.com/" GITHUB_OWNER "/" GITHUB_REPO
        "/releases/download/%s/firmware.bin", ota_latest_tag);

    {
        WiFiClientSecure client;
        client.setInsecure();
        HTTPClient http;
        http.begin(client, url);
        http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        http.setTimeout(30000);

        int code = http.GET();
        if (code == 200) {
            int contentLen = http.getSize();
            Stream &stream = http.getStream();

            if (Update.begin(contentLen > 0 ? contentLen : UPDATE_SIZE_UNKNOWN)) {
                uint8_t buf[512];
                int written = 0;
                unsigned long idleStart = millis();

                while (http.connected() && (contentLen < 0 || written < contentLen)) {
                    size_t avail = stream.available();
                    if (avail) {
                        idleStart = millis();
                        int r = stream.readBytes(buf, min(avail, sizeof(buf)));
                        if (Update.write(buf, r) != r) break;
                        written += r;
                        if (contentLen > 0) ota_progress = written * 100 / contentLen;
                    } else {
                        if (millis() - idleStart > 10000) break;
                        delay(1);
                    }
                }

                ota_status = (Update.end(true) && written == contentLen)
                             ? OTA_DONE : OTA_ERROR;
            } else {
                ota_status = OTA_ERROR;
            }
        } else {
            ota_status = OTA_ERROR;
        }
        http.end();
    }  // WiFiClientSecure and HTTPClient destroyed here
    vTaskDelete(NULL);
}

void otaUpdate() {
    if (ota_status != OTA_AVAILABLE) return;
    xTaskCreatePinnedToCore(updateTask, "ota_upd", 16384, NULL, 1, NULL, 0);
}
