#include "reliability_manager.h"
#include "web_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <time.h>

#include "bridge.h"
#include "ota_update.h"
#include "wireguard_net.h"
#include "tailscale_net.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_task_wdt.h"

namespace {

constexpr uint32_t CHECK_INTERVAL_MS = 10000;
constexpr uint32_t WIFI_FAILURE_RESTART_SECONDS = 180;
constexpr uint32_t BLE_FAILURE_RESTART_SECONDS = 900;
constexpr uint32_t WIREGUARD_FAILURE_RESTART_SECONDS = 900;
constexpr uint32_t SCHEDULE_BOOT_GRACE_SECONDS = 180;
constexpr uint32_t CLOCK_VALID_AFTER = 1700000000UL;
constexpr uint32_t FALLBACK_REBOOT_SECONDS = 6 * 60 * 60;
constexpr uint32_t LOW_MEMORY_RESTART_SECONDS = 5 * 60;
constexpr uint32_t LOW_FREE_HEAP_BYTES = 24 * 1024;
constexpr uint32_t LOW_LARGEST_BLOCK_BYTES = 8 * 1024;

bool s_wireguardExpected = false;
bool s_tailscaleExpected = false;
TaskHandle_t s_reliabilityTask = nullptr;
std::atomic<bool> s_restartRequested{false};
std::atomic<bool> s_restartAllowedDuringOta{false};
std::atomic<uint32_t> s_restartAtMs{0};

void controlledRestart(const char* reason) {
    log_e("[Reliability] Restarting: %s", reason);
    delay(500);
    ESP.restart();
}
void reliabilityTask(void*) {
    // The configured deployment timezone uses EU CET/CEST transition rules.
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");

#if ESP_IDF_VERSION_MAJOR >= 5
    const esp_task_wdt_config_t watchdogConfig = {
        .timeout_ms = 30000,
        .idle_core_mask = 0,
        .trigger_panic = true,
    };
    // Arduino Core 3.x normally initializes TWDT before setup(). Reconfigure
    // first to avoid the IDF error log emitted by a redundant init call; fall
    // back to init only when the framework has not created it.
    esp_err_t watchdogResult = esp_task_wdt_reconfigure(&watchdogConfig);
    if (watchdogResult == ESP_ERR_INVALID_STATE) {
        watchdogResult = esp_task_wdt_init(&watchdogConfig);
    }
    if (watchdogResult != ESP_OK) {
        log_w("[Reliability] Task watchdog configuration failed: %s", esp_err_to_name(watchdogResult));
    }
#else
    esp_task_wdt_init(30, true);
#endif
    esp_task_wdt_add(nullptr);

    uint32_t wifiDownSince = 0;
    uint32_t bleDownSince = 0;
    uint32_t wireguardDownSince = 0;
    uint32_t lowMemorySince = 0;
    uint32_t lastFallbackRebootMark = 0;

    for (;;) {
        esp_task_wdt_reset();
        const bool otaBusy = ota_update_is_busy();
        if (s_restartRequested.load(std::memory_order_acquire) &&
            (!otaBusy || s_restartAllowedDuringOta.load(std::memory_order_relaxed))) {
            const uint32_t restartAt = s_restartAtMs.load(std::memory_order_relaxed);
            if (static_cast<int32_t>(millis() - restartAt) >= 0) {
                controlledRestart("allocation-free remote restart request");
            }
        }
        uint32_t uptimeSeconds = millis() / 1000UL;

        bool wifiOk = WiFi.status() == WL_CONNECTED;
        bool bleOk = bridge_is_ble_connected();
        bool wireguardOk = !s_wireguardExpected || wireguard_net_is_connected();
        bool tailscaleOk = !s_tailscaleExpected || tailscale_net_is_connected();

        if (wifiOk) wifiDownSince = 0;
        else if (wifiDownSince == 0) wifiDownSince = uptimeSeconds;

        if (bleOk) bleDownSince = 0;
        else if (bleDownSince == 0) bleDownSince = uptimeSeconds;

        if (wireguardOk && tailscaleOk) wireguardDownSince = 0;
        else if (wireguardDownSince == 0) wireguardDownSince = uptimeSeconds;

        if (otaBusy) {
            // Do not carry outage time accumulated during the intentionally
            // disruptive OTA window into the first post-update check.
            wifiDownSince = 0;
            bleDownSince = 0;
            wireguardDownSince = 0;
            lowMemorySince = 0;
        } else {
            if (wifiDownSince != 0 && uptimeSeconds - wifiDownSince >= WIFI_FAILURE_RESTART_SECONDS) {
                controlledRestart("WiFi disconnected for 3 minutes");
            }
            if (bleDownSince != 0 && uptimeSeconds - bleDownSince >= BLE_FAILURE_RESTART_SECONDS) {
                controlledRestart("Meshtastic BLE link unavailable for 15 minutes");
            }
            if (wireguardDownSince != 0 && uptimeSeconds - wireguardDownSince >= WIREGUARD_FAILURE_RESTART_SECONDS) {
                controlledRestart(s_tailscaleExpected ? "Tailscale unavailable for 15 minutes" : "WireGuard tunnel unavailable for 15 minutes");
            }

            const uint32_t freeHeap = ESP.getFreeHeap();
            const uint32_t largestBlock = static_cast<uint32_t>(heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
            ));
            const bool memoryCritical = freeHeap < LOW_FREE_HEAP_BYTES &&
                                        largestBlock < LOW_LARGEST_BLOCK_BYTES;
            if (!memoryCritical) {
                lowMemorySince = 0;
            } else if (lowMemorySince == 0) {
                lowMemorySince = uptimeSeconds;
                log_w("[Reliability] Internal heap is critically fragmented (free=%u, largest=%u)",
                      freeHeap, largestBlock);
            } else if (uptimeSeconds - lowMemorySince >= LOW_MEMORY_RESTART_SECONDS) {
                controlledRestart("critical internal heap fragmentation persisted for 5 minutes");
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
        }

        uint32_t waitMs = CHECK_INTERVAL_MS;
        if (s_restartRequested.load(std::memory_order_acquire)) {
            const uint32_t nowMs = millis();
            const uint32_t restartAt = s_restartAtMs.load(std::memory_order_relaxed);
            if (static_cast<int32_t>(restartAt - nowMs) > 0) {
                waitMs = min(waitMs, restartAt - nowMs);
            } else {
                waitMs = 1;
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
    }
}

} // namespace

void reliability_manager_start(const BridgeConfig& cfg) {
    s_tailscaleExpected = cfg.tailscale_enabled;
    s_wireguardExpected = cfg.wireguard_enabled && !cfg.tailscale_enabled;
    // Keep the stack internal: the task participates in flash-backed logging
    // and must remain runnable while the external-memory cache is disabled.
    if (xTaskCreatePinnedToCore(
            reliabilityTask,
            "reliability_task",
            4096,
            nullptr,
            2,
            &s_reliabilityTask,
            1
        ) != pdPASS) {
        s_reliabilityTask = nullptr;
        log_e("[Reliability] Unable to start the supervisor task");
    }
}

bool reliability_manager_request_restart(uint32_t delayMs, bool allowDuringOta) {
    TaskHandle_t task = s_reliabilityTask;
    if (task == nullptr || (ota_update_is_busy() && !allowDuringOta)) {
        return false;
    }
    const uint32_t boundedDelay = delayMs < 250 ? 250 : delayMs;
    s_restartAtMs.store(millis() + boundedDelay, std::memory_order_relaxed);
    s_restartAllowedDuringOta.store(allowDuringOta, std::memory_order_relaxed);
    s_restartRequested.store(true, std::memory_order_release);
    xTaskNotifyGive(task);
    return true;
}

