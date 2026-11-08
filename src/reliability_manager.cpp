#include "reliability_manager.h"
#include "web_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include "bridge.h"
#include "wireguard_net.h"
#include "esp_task_wdt.h"

namespace {

constexpr uint32_t CHECK_INTERVAL_MS = 10000;
constexpr uint32_t WIFI_FAILURE_RESTART_SECONDS = 180;
constexpr uint32_t BLE_FAILURE_RESTART_SECONDS = 900;
constexpr uint32_t WIREGUARD_FAILURE_RESTART_SECONDS = 900;
constexpr uint32_t SCHEDULE_BOOT_GRACE_SECONDS = 180;
constexpr uint32_t CLOCK_VALID_AFTER = 1700000000UL;
constexpr uint32_t FALLBACK_REBOOT_SECONDS = 6 * 60 * 60;

bool s_wireguardExpected = false;

void controlledRestart(const char* reason) {
    log_e("[Reliability] Restarting: %s", reason);
    delay(500);
    ESP.restart();
}
void reliabilityTask(void*) {
    // Poland and Germany currently share the same EU CET/CEST transition rules.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");

    esp_task_wdt_init(30, true);
    esp_task_wdt_add(nullptr);

    uint32_t wifiDownSince = 0;
    uint32_t bleDownSince = 0;
    uint32_t wireguardDownSince = 0;
    uint32_t lastFallbackRebootMark = 0;

    for (;;) {
        esp_task_wdt_reset();
        uint32_t uptimeSeconds = millis() / 1000UL;

        bool wifiOk = WiFi.status() == WL_CONNECTED;
        bool bleOk = bridge_is_ble_connected();
        bool wireguardOk = !s_wireguardExpected || wireguard_net_is_connected();

        if (wifiOk) wifiDownSince = 0;
        else if (wifiDownSince == 0) wifiDownSince = uptimeSeconds;

        if (bleOk) bleDownSince = 0;
        else if (bleDownSince == 0) bleDownSince = uptimeSeconds;

        if (wireguardOk) wireguardDownSince = 0;
        else if (wireguardDownSince == 0) wireguardDownSince = uptimeSeconds;

        if (wifiDownSince != 0 && uptimeSeconds - wifiDownSince >= WIFI_FAILURE_RESTART_SECONDS) {
            controlledRestart("WiFi disconnected for 3 minutes");
        }
        if (bleDownSince != 0 && uptimeSeconds - bleDownSince >= BLE_FAILURE_RESTART_SECONDS) {
            controlledRestart("Meshtastic BLE link unavailable for 15 minutes");
        }
        if (wireguardDownSince != 0 && uptimeSeconds - wireguardDownSince >= WIREGUARD_FAILURE_RESTART_SECONDS) {
            controlledRestart("WireGuard tunnel unavailable for 15 minutes");
        }

        time_t now = time(nullptr);
        if (now >= static_cast<time_t>(CLOCK_VALID_AFTER)) {
            struct tm localTime = {};
            localtime_r(&now, &localTime);
            if (uptimeSeconds >= SCHEDULE_BOOT_GRACE_SECONDS &&
                localTime.tm_min == 0 && localTime.tm_sec < 20 &&
                (localTime.tm_hour == 0 || localTime.tm_hour == 6 ||
                 localTime.tm_hour == 12 || localTime.tm_hour == 18)) {
                controlledRestart("scheduled 00/06/12/18 maintenance reboot");
            }
        } else if (uptimeSeconds - lastFallbackRebootMark >= FALLBACK_REBOOT_SECONDS) {
            // If NTP is unavailable for six hours, still perform maintenance so
            // a broken network stack gets a clean recovery opportunity.
            controlledRestart("six-hour fallback reboot (clock not synchronized)");
        }

        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

} // namespace

void reliability_manager_start(const BridgeConfig& cfg) {
    s_wireguardExpected = cfg.wireguard_enabled;
    xTaskCreatePinnedToCore(reliabilityTask, "reliability_task", 4096, nullptr, 1, nullptr, 0);
}

