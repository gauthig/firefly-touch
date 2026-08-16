/*
 * ota_update — Wi-Fi OTA orchestration for a PANEL_HAS_OTA panel.
 *
 * Owns one FreeRTOS task that, on request, temporarily joins the Wi-Fi
 * network from Kconfig (CONFIG_FIREFLY_OTA_WIFI_SSID/PASSWORD), fetches the
 * JSON manifest at CONFIG_FIREFLY_OTA_MANIFEST_URL (see ota_manifest.h),
 * and — if the manifest version is newer than the running image — downloads
 * and flashes it via esp_https_ota(), then reboots.
 *
 * A panel that also runs ESP-NOW (components/espnow_link) shares the same
 * radio: joining the OTA Wi-Fi network changes the channel away from the
 * fixed ESP-NOW channel for the duration of the check. On any outcome that
 * doesn't end in a reboot (no update / failure / timeout), the channel is
 * restored to CONFIG_FIREFLY_ESPNOW_CHANNEL before returning to idle so the
 * ESP-NOW link recovers on its own.
 *
 * No UI trigger yet — see GitHub issue #17 for the long-press action that
 * calls ota_update_request_check(). Until then, call it manually (e.g. from
 * a debug build) or enable CONFIG_FIREFLY_OTA_CHECK_ON_BOOT.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OTA_STATUS_IDLE = 0,
    OTA_STATUS_CHECKING,
    OTA_STATUS_UPDATING,
    OTA_STATUS_UP_TO_DATE,
    OTA_STATUS_FAILED,
} ota_status_t;

/*
 * Creates the ota_update task. Safe to call once per boot, after nvs/board
 * init (mirrors espnow_link_init()). If CONFIG_FIREFLY_OTA_CHECK_ON_BOOT is
 * enabled, also queues an immediate check.
 */
void ota_update_init(void);

/*
 * Requests an update check. Returns false without doing anything if a
 * check/update is already in progress (OTA_STATUS_CHECKING/UPDATING) or if
 * ota_update_init() has not been called yet. Non-blocking either way — the
 * actual work happens on the ota_update task.
 */
bool ota_update_request_check(void);

/* Current state, for a status label/icon in the UI (see GitHub issue #17). */
ota_status_t ota_update_get_status(void);

#ifdef __cplusplus
}
#endif
