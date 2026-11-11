#include "device_presence.h"

#include "build_options.h"
#include "utils.h"

#include <WiFi.h>
#include <atomic>

#ifdef ESP_IDF5_MIGRATION_BUILD
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <mbedtls/sha256.h>

extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_data_cert_x509_crt_bundle_bin_end");

namespace {

constexpr const char* PRESENCE_ENDPOINT = "https://woister.de/p.gif";
constexpr uint32_t INITIAL_DELAY_MS = 180000;
constexpr uint32_t PRESENCE_INTERVAL_MS = 900000;
constexpr uint32_t RETRY_INTERVAL_MS = 300000;
constexpr uint32_t HTTP_TIMEOUT_MS = 12000;
constexpr uint32_t TASK_STACK_BYTES = 6144;

std::atomic<uint32_t> s_node_num{0};
TaskHandle_t s_task = nullptr;
char s_device_id[33] = {0};

void buildPseudonymousDeviceId() {
    static constexpr char domain[] = "meshgate-presence-v1";
    const uint64_t efuseId = ESP.getEfuseMac();
    uint8_t material[sizeof(domain) - 1 + sizeof(efuseId)] = {0};
    memcpy(material, domain, sizeof(domain) - 1);
    for (size_t i = 0; i < sizeof(efuseId); ++i) {
        material[sizeof(domain) - 1 + i] = static_cast<uint8_t>(efuseId >> (56 - (i * 8)));
    }

    uint8_t digest[32] = {0};
    if (mbedtls_sha256(material, sizeof(material), digest, 0) != 0) {
        log_e("[PRESENCE] Unable to derive the pseudonymous installation ID.");
        return;
    }

    static constexpr char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; ++i) {
        s_device_id[i * 2] = hex[digest[i] >> 4];
        s_device_id[i * 2 + 1] = hex[digest[i] & 0x0f];
    }
    s_device_id[32] = '\0';
}

bool sendPresence(const char* reason) {
    if (WiFi.status() != WL_CONNECTED || s_device_id[0] == '\0') {
        return false;
    }

    char url[320] = {0};
    const uint32_t nodeNum = s_node_num.load(std::memory_order_relaxed);
    if (nodeNum != 0) {
        snprintf(
            url,
            sizeof(url),
            "%s?id=%s&v=%s&b=%s&n=%08lx&up=%lu&r=%s",
            PRESENCE_ENDPOINT,
            s_device_id,
            FIRMWARE_VERSION,
            HARDWARE_BOARD_NAME,
            static_cast<unsigned long>(nodeNum),
            static_cast<unsigned long>(millis() / 1000UL),
            reason
        );
    } else {
        snprintf(
            url,
            sizeof(url),
            "%s?id=%s&v=%s&b=%s&up=%lu&r=%s",
            PRESENCE_ENDPOINT,
            s_device_id,
            FIRMWARE_VERSION,
            HARDWARE_BOARD_NAME,
            static_cast<unsigned long>(millis() / 1000UL),
            reason
        );
    }

    NetworkClientSecure client;
    client.setCACertBundle(
        rootca_crt_bundle_start,
        rootca_crt_bundle_end - rootca_crt_bundle_start
    );

    HTTPClient http;
    http.setConnectTimeout(HTTP_TIMEOUT_MS);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.setUserAgent("MeshGate/" FIRMWARE_VERSION);
    if (!http.begin(client, url)) {
        log_w("[PRESENCE] Unable to initialize the presence request.");
        return false;
    }

    const int status = http.GET();
    http.end();
    if (status != HTTP_CODE_OK) {
        log_w("[PRESENCE] Request failed with HTTP status %d.", status);
        return false;
    }

    log_i("[PRESENCE] Installation presence accepted.");
    return true;
}

void presenceTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(INITIAL_DELAY_MS));
    bool firstUpdate = true;

    for (;;) {
        const bool sent = sendPresence(firstUpdate ? "boot" : "periodic");
        if (sent) {
            firstUpdate = false;
        }
        const uint32_t waitMs = sent ? PRESENCE_INTERVAL_MS : RETRY_INTERVAL_MS;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(waitMs));
    }
}

}  // namespace
#endif

void device_presence_init() {
#ifdef ESP_IDF5_MIGRATION_BUILD
    buildPseudonymousDeviceId();
#endif
}

void device_presence_start() {
#ifdef ESP_IDF5_MIGRATION_BUILD
    if (s_task != nullptr || s_device_id[0] == '\0') {
        return;
    }
    if (xTaskCreatePinnedToCore(
            presenceTask,
            "device_presence",
            TASK_STACK_BYTES,
            nullptr,
            1,
            &s_task,
            0
        ) != pdPASS) {
        s_task = nullptr;
        log_w("[PRESENCE] Unable to start the presence task.");
    }
#endif
}

void device_presence_set_meshtastic_node(uint32_t nodeNum) {
#ifdef ESP_IDF5_MIGRATION_BUILD
    if (nodeNum != 0) {
        s_node_num.store(nodeNum, std::memory_order_relaxed);
    }
#else
    (void)nodeNum;
#endif
}

