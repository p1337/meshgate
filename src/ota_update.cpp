#include "ota_update.h"

#include <Update.h>
#include <atomic>
#include <esp_ota_ops.h>
#include <WiFi.h>

#ifdef ESP_IDF5_MIGRATION_BUILD
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>
#include <mbedtls/sha256.h>
#endif

#include "build_options.h"
#include "bounded_stream_response.h"
#include "mqtt_net.h"
#include "reliability_manager.h"
#include "utils.h"
#include "web_log.h"

namespace {
enum class RequestState : uintptr_t { None = 0, Unauthorized, Busy, Invalid, Uploading, Failed, Success };
enum class OfficialState : uint8_t { Idle = 0, Checking, Current, Available, Downloading, Verifying, Installing, Rebooting, Failed, Unsupported };

constexpr const char* STABLE_METADATA_URL = "https://p1337.github.io/meshgate/latest_ota.json";
constexpr const char* STABLE_FIRMWARE_URL = "https://p1337.github.io/meshgate/firmware/arduino_nano_esp32_idf5/firmware.bin";
constexpr const char* BETA_METADATA_URL = "https://p1337.github.io/meshgate/latest_beta_ota.json";
constexpr const char* BETA_FIRMWARE_URL = "https://p1337.github.io/meshgate/beta-firmware/arduino_nano_esp32_idf5/firmware.bin";
constexpr const char* OFFICIAL_CHANGELOG_URL = "https://github.com/p1337/meshgate/blob/main/CHANGELOG.md";
constexpr const char* OFFICIAL_BOARD = "arduino_nano_esp32_idf5";
constexpr uint32_t AUTO_INITIAL_DELAY_MS = 5U * 60U * 1000U;
constexpr uint32_t AUTO_INITIAL_JITTER_MS = 10U * 60U * 1000U;
constexpr uint32_t AUTO_INTERVAL_MS = 6U * 60U * 60U * 1000U;
constexpr uint32_t AUTO_RETRY_MS = 30U * 60U * 1000U;

String s_token;
AsyncWebServerRequest* s_activeRequest = nullptr;
std::atomic<bool> s_updateClaimed{false};
std::atomic<bool> s_betaEnabled{false};
std::atomic<bool> s_autoUpdateEnabled{true};
std::atomic<bool> s_autoInstallPending{false};
std::atomic<OfficialState> s_officialState{OfficialState::Idle};
std::atomic<uint32_t> s_downloadedBytes{0};
std::atomic<uint32_t> s_totalBytes{0};
portMUX_TYPE s_statusMux = portMUX_INITIALIZER_UNLOCKED;
char s_latestVersion[48] = "";
char s_officialError[160] = "";
std::atomic<bool> s_autoSchedulerStarted{false};
std::atomic<uint32_t> s_nextAutoCheckAt{0};

#ifdef ESP_IDF5_MIGRATION_BUILD
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_data_cert_x509_crt_bundle_bin_end");
#endif

RequestState getState(AsyncWebServerRequest* request) {
    return static_cast<RequestState>(reinterpret_cast<uintptr_t>(request->_tempObject));
}

void setState(AsyncWebServerRequest* request, RequestState state) {
    request->_tempObject = reinterpret_cast<void*>(static_cast<uintptr_t>(state));
}

bool constantTimeEquals(const String& lhs, const String& rhs) {
    if (lhs.length() != rhs.length()) return false;
    uint8_t difference = 0;
    for (size_t i = 0; i < lhs.length(); ++i) difference |= static_cast<uint8_t>(lhs[i] ^ rhs[i]);
    return difference == 0;
}

bool isAuthorized(AsyncWebServerRequest* request) {
    return !s_token.isEmpty() && request->hasHeader("X-OTA-Key") &&
           constantTimeEquals(request->header("X-OTA-Key"), s_token);
}

void setOfficialText(char* destination, size_t size, const String& value) {
    portENTER_CRITICAL(&s_statusMux);
    strlcpy(destination, value.c_str(), size);
    portEXIT_CRITICAL(&s_statusMux);
}

String getOfficialText(const char* source) {
    char copy[160];
    portENTER_CRITICAL(&s_statusMux);
    strlcpy(copy, source, sizeof(copy));
    portEXIT_CRITICAL(&s_statusMux);
    return String(copy);
}

const char* officialStateName(OfficialState state) {
    switch (state) {
        case OfficialState::Idle: return "idle";
        case OfficialState::Checking: return "checking";
        case OfficialState::Current: return "current";
        case OfficialState::Available: return "available";
        case OfficialState::Downloading: return "downloading";
        case OfficialState::Verifying: return "verifying";
        case OfficialState::Installing: return "installing";
        case OfficialState::Rebooting: return "rebooting";
        case OfficialState::Failed: return "failed";
        case OfficialState::Unsupported: return "unsupported";
    }
    return "unknown";
}

void failOfficial(const String& reason) {
    log_e("[OTA] Official update failed: %s", reason.c_str());
    if (Update.isRunning()) Update.abort();
    setOfficialText(s_officialError, sizeof(s_officialError), reason);
    s_officialState.store(OfficialState::Failed, std::memory_order_release);
    s_updateClaimed.store(false, std::memory_order_release);
    mqtt_net_resume_after_ota();
}

void failUpload(AsyncWebServerRequest* request, const char* reason) {
    log_e("[OTA] Update failed: %s", reason);
    if (Update.isRunning()) Update.abort();
    if (s_activeRequest == request) s_activeRequest = nullptr;
    s_updateClaimed.store(false, std::memory_order_release);
    setState(request, RequestState::Failed);
}

bool scheduleRestart() {
    // This call is only reached after Update.end() has validated and activated
    // the complete image. It is the sole restart request allowed while the
    // official updater still owns the OTA state.
    return reliability_manager_request_restart(1500, true);
}

String partitionLabel(const esp_partition_t* partition) {
    return partition ? String(partition->label) : String();
}

bool extractJsonString(const String& json, const char* key, String& value) {
    const String needle = String('"') + key + '"';
    int position = json.indexOf(needle);
    if (position < 0 || (position = json.indexOf(':', position + needle.length())) < 0 ||
        (position = json.indexOf('"', position + 1)) < 0) return false;
    const int end = json.indexOf('"', position + 1);
    if (end < 0) return false;
    value = json.substring(position + 1, end);
    return true;
}

bool extractJsonSize(const String& json, const char* key, size_t& value) {
    const String needle = String('"') + key + '"';
    int position = json.indexOf(needle);
    if (position < 0 || (position = json.indexOf(':', position + needle.length())) < 0) return false;
    ++position;
    while (position < static_cast<int>(json.length()) && isspace(static_cast<unsigned char>(json[position]))) ++position;
    const int start = position;
    while (position < static_cast<int>(json.length()) && isdigit(static_cast<unsigned char>(json[position]))) ++position;
    if (position == start) return false;
    const unsigned long parsed = strtoul(json.substring(start, position).c_str(), nullptr, 10);
    if (parsed == 0) return false;
    value = static_cast<size_t>(parsed);
    return true;
}

bool isHexDigest(const String& digest) {
    if (digest.length() != 64) return false;
    for (size_t i = 0; i < digest.length(); ++i) {
        if (!isxdigit(static_cast<unsigned char>(digest[i]))) return false;
    }
    return true;
}

struct ParsedVersion {
    uint16_t parts[3] = {};
    String prerelease;
};

bool parseVersion(const String& version, ParsedVersion& parsed) {
    const char* cursor = version.c_str();
    if (*cursor == 'v' || *cursor == 'V') ++cursor;
    for (int i = 0; i < 3; ++i) {
        if (!isdigit(static_cast<unsigned char>(*cursor))) return false;
        const char* numberStart = cursor;
        char* end = nullptr;
        const long number = strtol(cursor, &end, 10);
        if (number < 0 || number > 65535 || end == cursor ||
            (end - numberStart > 1 && *numberStart == '0')) return false;
        parsed.parts[i] = static_cast<uint16_t>(number);
        cursor = end;
        if (i < 2) {
            if (*cursor != '.') return false;
            ++cursor;
        }
    }
    if (*cursor == '\0') return true;
    if (*cursor != '-' || *(cursor + 1) == '\0') return false;
    parsed.prerelease = cursor + 1;
    bool previousDot = true;
    for (size_t i = 0; i < parsed.prerelease.length(); ++i) {
        const char c = parsed.prerelease[i];
        if (c == '.') {
            if (previousDot) return false;
            previousDot = true;
        } else if (isalnum(static_cast<unsigned char>(c)) || c == '-') {
            previousDot = false;
        } else {
            return false;
        }
    }
    return !previousDot;
}

bool isNumericIdentifier(const String& value) {
    if (value.isEmpty() || (value.length() > 1 && value[0] == '0')) return false;
    for (size_t i = 0; i < value.length(); ++i) {
        if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    return true;
}

int comparePrerelease(const String& lhs, const String& rhs) {
    if (lhs.isEmpty() || rhs.isEmpty()) {
        if (lhs.isEmpty() && rhs.isEmpty()) return 0;
        return lhs.isEmpty() ? 1 : -1;
    }
    int lhsStart = 0;
    int rhsStart = 0;
    while (true) {
        const int lhsDot = lhs.indexOf('.', lhsStart);
        const int rhsDot = rhs.indexOf('.', rhsStart);
        const String lhsPart = lhs.substring(lhsStart, lhsDot < 0 ? lhs.length() : lhsDot);
        const String rhsPart = rhs.substring(rhsStart, rhsDot < 0 ? rhs.length() : rhsDot);
        const bool lhsNumeric = isNumericIdentifier(lhsPart);
        const bool rhsNumeric = isNumericIdentifier(rhsPart);
        int order = 0;
        if (lhsNumeric && rhsNumeric) {
            if (lhsPart.length() != rhsPart.length()) order = lhsPart.length() > rhsPart.length() ? 1 : -1;
            else order = lhsPart.compareTo(rhsPart);
        } else if (lhsNumeric != rhsNumeric) {
            order = lhsNumeric ? -1 : 1;
        } else {
            order = lhsPart.compareTo(rhsPart);
        }
        if (order != 0) return order > 0 ? 1 : -1;
        if (lhsDot < 0 || rhsDot < 0) {
            if (lhsDot < 0 && rhsDot < 0) return 0;
            return lhsDot < 0 ? -1 : 1;
        }
        lhsStart = lhsDot + 1;
        rhsStart = rhsDot + 1;
    }
}

int compareVersions(const String& current, const String& candidate) {
    ParsedVersion currentVersion;
    ParsedVersion candidateVersion;
    if (!parseVersion(current, currentVersion) || !parseVersion(candidate, candidateVersion)) return -2;
    for (int i = 0; i < 3; ++i) {
        if (candidateVersion.parts[i] > currentVersion.parts[i]) return 1;
        if (candidateVersion.parts[i] < currentVersion.parts[i]) return -1;
    }
    return comparePrerelease(candidateVersion.prerelease, currentVersion.prerelease);
}

bool isPrereleaseVersion(const String& version) {
    ParsedVersion parsed;
    return parseVersion(version, parsed) && !parsed.prerelease.isEmpty();
}

uint32_t deviceJitter(uint32_t range) {
    if (range == 0) return 0;
    const uint64_t mac = ESP.getEfuseMac();
    const uint32_t folded = static_cast<uint32_t>(mac) ^ static_cast<uint32_t>(mac >> 32);
    return folded % range;
}

void scheduleAutomaticCheck(uint32_t delayMs) {
    s_nextAutoCheckAt.store(millis() + delayMs, std::memory_order_release);
}

#ifdef ESP_IDF5_MIGRATION_BUILD
bool fetchOfficialMetadataOnce(bool betaChannel, String& version, String& firmwareUrl, size_t& imageSize, String& sha256) {
    const char* metadataUrl = betaChannel ? BETA_METADATA_URL : STABLE_METADATA_URL;
    const char* expectedFirmwareUrl = betaChannel ? BETA_FIRMWARE_URL : STABLE_FIRMWARE_URL;
    NetworkClientSecure client;
    client.setCACertBundle(rootca_crt_bundle_start,
                           static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
    HTTPClient http;
    http.setConnectTimeout(12000);
    http.setTimeout(15000);
    if (!http.begin(client, metadataUrl)) return false;
    const int status = http.GET();
    if (status != HTTP_CODE_OK) {
        log_e("[OTA] Metadata request returned HTTP %d", status);
        http.end();
        return false;
    }
    const int length = http.getSize();
    if (length <= 0 || length > 2048) {
        log_e("[OTA] Invalid metadata length: %d", length);
        http.end();
        return false;
    }
    const String json = http.getString();
    http.end();

    String board;
    if (!extractJsonString(json, "version", version) || !extractJsonString(json, "board", board) ||
        !extractJsonString(json, "url", firmwareUrl) || !extractJsonSize(json, "size", imageSize) ||
        !extractJsonString(json, "sha256", sha256)) return false;
    sha256.toLowerCase();
    ParsedVersion parsedVersion;
    return board == OFFICIAL_BOARD && firmwareUrl == expectedFirmwareUrl &&
           parseVersion(version, parsedVersion) && isHexDigest(sha256);
}

bool fetchOfficialMetadata(bool betaChannel, String& version, String& firmwareUrl, size_t& imageSize, String& sha256) {
    constexpr uint8_t MAX_METADATA_ATTEMPTS = 4;
    for (uint8_t attempt = 0; attempt < MAX_METADATA_ATTEMPTS; ++attempt) {
        if (fetchOfficialMetadataOnce(betaChannel, version, firmwareUrl, imageSize, sha256)) return true;
        if (attempt + 1 < MAX_METADATA_ATTEMPTS) {
            const uint32_t delayMs = 750U * static_cast<uint32_t>(attempt + 1);
            log_w("[OTA] Metadata fetch failed; retrying in %u ms (%u/%u)",
                  static_cast<unsigned>(delayMs), static_cast<unsigned>(attempt + 2),
                  static_cast<unsigned>(MAX_METADATA_ATTEMPTS));
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
    }
    return false;
}

String digestToHex(const uint8_t digest[32]) {
    static const char HEX_DIGITS[] = "0123456789abcdef";
    char output[65];
    for (size_t i = 0; i < 32; ++i) {
        output[i * 2] = HEX_DIGITS[digest[i] >> 4];
        output[i * 2 + 1] = HEX_DIGITS[digest[i] & 0x0f];
    }
    output[64] = '\0';
    return String(output);
}

void finishOfficialTask(bool installRequested) {
    if (installRequested && s_officialState.load(std::memory_order_acquire) != OfficialState::Rebooting) {
        mqtt_net_resume_after_ota();
    }
    if (installRequested) {
        vTaskDelete(nullptr);
    } else {
        // Metadata-only checks use a PSRAM-backed stack so fragmented internal
        // SRAM cannot prevent a harmless release lookup. Tasks created with
        // xTaskCreateWithCaps() must be released by the matching API.
        vTaskDeleteWithCaps(nullptr);
    }
}

void officialUpdateTask(void* parameter) {
    const uintptr_t taskOptions = reinterpret_cast<uintptr_t>(parameter);
    const bool installRequested = (taskOptions & 0x01U) != 0;
    const bool betaChannel = (taskOptions & 0x02U) != 0;
    const bool automaticCheck = (taskOptions & 0x04U) != 0;
    String version, firmwareUrl, expectedSha256;
    size_t expectedSize = 0;
    s_officialState.store(OfficialState::Checking, std::memory_order_release);
    s_downloadedBytes.store(0, std::memory_order_relaxed);
    s_totalBytes.store(0, std::memory_order_relaxed);
    setOfficialText(s_latestVersion, sizeof(s_latestVersion), "");
    setOfficialText(s_officialError, sizeof(s_officialError), "");

    if (!fetchOfficialMetadata(betaChannel, version, firmwareUrl, expectedSize, expectedSha256)) {
        failOfficial(betaChannel
            ? "Could not validate the beta GitHub release metadata"
            : "Could not validate the stable GitHub release metadata");
        finishOfficialTask(installRequested);
        return;
    }
    setOfficialText(s_latestVersion, sizeof(s_latestVersion), version);
    const int versionOrder = compareVersions(FIRMWARE_VERSION, version);
    if (versionOrder <= 0 && versionOrder != -2) {
        s_officialState.store(OfficialState::Current, std::memory_order_release);
        s_updateClaimed.store(false, std::memory_order_release);
        finishOfficialTask(installRequested);
        return;
    }
    if (versionOrder == -2) {
        failOfficial("Firmware version metadata is invalid");
        finishOfficialTask(installRequested);
        return;
    }
    if (!installRequested) {
        if (automaticCheck) s_autoInstallPending.store(true, std::memory_order_release);
        s_officialState.store(OfficialState::Available, std::memory_order_release);
        s_updateClaimed.store(false, std::memory_order_release);
        finishOfficialTask(installRequested);
        return;
    }

    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (next == nullptr || expectedSize > next->size) {
        failOfficial("Official firmware does not fit the inactive OTA slot");
        finishOfficialTask(installRequested);
        return;
    }

    // MQTT was already released synchronously before this internal-stack task
    // was created. This delay lets final network callbacks release their
    // buffers before the download TLS session starts.
    vTaskDelay(pdMS_TO_TICKS(250));
    s_totalBytes.store(static_cast<uint32_t>(expectedSize), std::memory_order_relaxed);
    s_officialState.store(OfficialState::Downloading, std::memory_order_release);
    log_i("[OTA] Downloading official MeshGate %s (%u bytes, free heap %u)",
          version.c_str(), static_cast<unsigned>(expectedSize), static_cast<unsigned>(ESP.getFreeHeap()));
    if (!Update.begin(expectedSize, U_FLASH)) {
        failOfficial(Update.errorString());
        finishOfficialTask(installRequested);
        return;
    }

    mbedtls_sha256_context shaContext;
    mbedtls_sha256_init(&shaContext);
    if (mbedtls_sha256_starts(&shaContext, 0) != 0) {
        mbedtls_sha256_free(&shaContext);
        failOfficial("Could not initialize SHA-256 validation");
        finishOfficialTask(installRequested);
        return;
    }

    uint8_t buffer[1024];
    size_t remaining = expectedSize;
    bool fatalStreamFailure = false;
    String fatalStreamError;
    constexpr uint32_t MAX_DOWNLOAD_TIME_MS = 20U * 60U * 1000U;
    constexpr uint32_t MAX_RETRY_DELAY_MS = 5000U;
    const uint32_t downloadStartedAt = millis();
    uint16_t connection = 0;

    while (remaining > 0 && millis() - downloadStartedAt < MAX_DOWNLOAD_TIME_MS) {
        ++connection;
        const size_t offset = expectedSize - remaining;
        NetworkClientSecure client;
        client.setCACertBundle(rootca_crt_bundle_start,
                               static_cast<size_t>(rootca_crt_bundle_end - rootca_crt_bundle_start));
        HTTPClient http;
        http.setConnectTimeout(12000);
        http.setTimeout(30000);
        if (!http.begin(client, firmwareUrl)) {
            const uint32_t delayMs = min(MAX_RETRY_DELAY_MS, 500U * static_cast<uint32_t>(connection));
            log_w("[OTA] Could not open firmware URL; retrying in %u ms", static_cast<unsigned>(delayMs));
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            continue;
        }
        if (offset > 0) {
            http.addHeader("Range", "bytes=" + String(static_cast<unsigned long>(offset)) + "-");
            log_w("[OTA] Resuming official download at byte %u (connection %u)",
                  static_cast<unsigned>(offset), static_cast<unsigned>(connection));
        }

        const int status = http.GET();
        const int responseSize = http.getSize();
        const int expectedStatus = offset == 0 ? HTTP_CODE_OK : HTTP_CODE_PARTIAL_CONTENT;
        if (status != expectedStatus || responseSize <= 0 || static_cast<size_t>(responseSize) != remaining) {
            log_w("[OTA] Firmware request at byte %u returned HTTP %d and size %d; retrying",
                  static_cast<unsigned>(offset), status, responseSize);
            http.end();
            const uint32_t delayMs = min(MAX_RETRY_DELAY_MS, 500U * static_cast<uint32_t>(connection));
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            continue;
        }

        NetworkClient* stream = http.getStreamPtr();
        uint32_t lastDataAt = millis();
        while (remaining > 0) {
            const int available = stream->available();
            if (available <= 0) {
                if (!stream->connected() || millis() - lastDataAt > 30000) break;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            const size_t chunk = min(remaining, min(sizeof(buffer), static_cast<size_t>(available)));
            const size_t received = stream->readBytes(buffer, chunk);
            if (received == 0) break;
            if (mbedtls_sha256_update(&shaContext, buffer, received) != 0) {
                fatalStreamFailure = true;
                fatalStreamError = "SHA-256 processing failed during download";
                break;
            }
            if (Update.write(buffer, received) != received) {
                fatalStreamFailure = true;
                fatalStreamError = Update.errorString();
                break;
            }
            remaining -= received;
            s_downloadedBytes.fetch_add(static_cast<uint32_t>(received), std::memory_order_relaxed);
            lastDataAt = millis();
        }
        http.end();
        if (fatalStreamFailure) break;
        if (remaining > 0) {
            const uint32_t delayMs = min(MAX_RETRY_DELAY_MS, 500U * static_cast<uint32_t>(connection));
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
    }

    uint8_t actualDigest[32];
    const bool digestFinished = mbedtls_sha256_finish(&shaContext, actualDigest) == 0;
    mbedtls_sha256_free(&shaContext);
    if (fatalStreamFailure || remaining != 0) {
        failOfficial(fatalStreamError.isEmpty() ? "Firmware download did not complete within the safe retry window" : fatalStreamError);
        finishOfficialTask(installRequested);
        return;
    }
    s_officialState.store(OfficialState::Verifying, std::memory_order_release);
    if (!digestFinished || digestToHex(actualDigest) != expectedSha256) {
        failOfficial("Official firmware SHA-256 verification failed");
        finishOfficialTask(installRequested);
        return;
    }
    s_officialState.store(OfficialState::Installing, std::memory_order_release);
    if (!Update.end(false)) {
        failOfficial(Update.errorString());
        finishOfficialTask(installRequested);
        return;
    }
    log_i("[OTA] Official MeshGate %s verified and activated", version.c_str());
    s_officialState.store(OfficialState::Rebooting, std::memory_order_release);
    if (!scheduleRestart()) failOfficial("Firmware installed, but restart scheduling failed");
    finishOfficialTask(installRequested);
}

bool startOfficialTask(bool checkOnly, bool betaChannel, bool automaticCheck = false) {
    bool expected = false;
    if (!s_updateClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return false;

    if (!checkOnly) {
        // MQTT and the installer both require internal RAM for TLS and a
        // cache-safe task stack. Stop MQTT before allocating the OTA task so a
        // fragmented heap cannot reject the task before it gets a chance to
        // release MQTT itself.
        mqtt_net_pause_for_ota();
    }

    uintptr_t taskOptions = checkOnly ? 0U : 0x01U;
    if (betaChannel) taskOptions |= 0x02U;
    if (automaticCheck) taskOptions |= 0x04U;
    void* taskParameter = reinterpret_cast<void*>(taskOptions);
    const BaseType_t taskCreated = checkOnly
        ? xTaskCreateWithCaps(
            officialUpdateTask,
            "official-check",
            8192,
            taskParameter,
            2,
            nullptr,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        )
        : xTaskCreate(officialUpdateTask, "official-ota", 10240, taskParameter, 2, nullptr);
    if (taskCreated == pdPASS) return true;

    s_updateClaimed.store(false, std::memory_order_release);
    if (!checkOnly) mqtt_net_resume_after_ota();
    return false;
}
#endif
} // namespace

void ota_update_set_token(const String& token) { s_token = token; }

bool ota_update_is_authorized(AsyncWebServerRequest* request) { return isAuthorized(request); }

void ota_update_set_policy(bool betaEnabled, bool autoUpdateEnabled) {
    const bool channelChanged = s_betaEnabled.exchange(betaEnabled, std::memory_order_acq_rel) != betaEnabled;
    s_autoUpdateEnabled.store(autoUpdateEnabled, std::memory_order_release);
    if (!autoUpdateEnabled) s_autoInstallPending.store(false, std::memory_order_release);
    if (channelChanged && !ota_update_is_busy()) {
        s_autoInstallPending.store(false, std::memory_order_release);
        setOfficialText(s_latestVersion, sizeof(s_latestVersion), "");
        setOfficialText(s_officialError, sizeof(s_officialError), "");
        s_officialState.store(OfficialState::Idle, std::memory_order_release);
    }
    if (s_autoSchedulerStarted.load(std::memory_order_acquire) && autoUpdateEnabled) {
        scheduleAutomaticCheck(channelChanged ? 60U * 1000U + deviceJitter(4U * 60U * 1000U)
                                              : AUTO_INITIAL_DELAY_MS + deviceJitter(AUTO_INITIAL_JITTER_MS));
    }
}

void ota_update_start_automatic_checks() {
    s_autoSchedulerStarted.store(true, std::memory_order_release);
    scheduleAutomaticCheck(AUTO_INITIAL_DELAY_MS + deviceJitter(AUTO_INITIAL_JITTER_MS));
}

void ota_update_loop() {
#ifdef ESP_IDF5_MIGRATION_BUILD
    if (!s_autoSchedulerStarted.load(std::memory_order_acquire) ||
        !s_autoUpdateEnabled.load(std::memory_order_acquire) ||
        WiFi.status() != WL_CONNECTED) return;
    if (ota_update_is_busy()) return;
    const bool betaChannel = s_betaEnabled.load(std::memory_order_acquire);
    if (s_autoInstallPending.exchange(false, std::memory_order_acq_rel)) {
        log_i("[OTA] Installing newer release found by the scheduled %s-channel check",
              betaChannel ? "beta" : "stable");
        if (!startOfficialTask(false, betaChannel)) {
            scheduleAutomaticCheck(AUTO_RETRY_MS + deviceJitter(AUTO_INITIAL_JITTER_MS));
        }
        return;
    }
    if (static_cast<int32_t>(millis() - s_nextAutoCheckAt.load(std::memory_order_acquire)) < 0) return;
    log_i("[OTA] Starting scheduled %s-channel metadata check", betaChannel ? "beta" : "stable");
    if (startOfficialTask(true, betaChannel, true)) {
        scheduleAutomaticCheck(AUTO_INTERVAL_MS + deviceJitter(AUTO_INITIAL_JITTER_MS));
    } else {
        scheduleAutomaticCheck(AUTO_RETRY_MS + deviceJitter(AUTO_INITIAL_JITTER_MS));
    }
#endif
}

bool ota_update_is_busy() {
    return s_updateClaimed.load(std::memory_order_acquire) || Update.isRunning();
}

void ota_update_register(AsyncWebServer& server, const String& token) {
    ota_update_set_token(token);

    // Child routes must precede /api/ota because ESPAsyncWebServer permits prefix matches.
    server.on("/api/ota/official", HTTP_GET, [](AsyncWebServerRequest* request) {
        const OfficialState state = s_officialState.load(std::memory_order_acquire);
        const bool betaEnabled = s_betaEnabled.load(std::memory_order_acquire);
        const bool autoUpdateEnabled = s_autoUpdateEnabled.load(std::memory_order_acquire);
        BoundedStreamResponse* response = new BoundedStreamResponse("application/json");
        response->print("{\"supported\":");
#ifdef ESP_IDF5_MIGRATION_BUILD
        response->print("true");
#else
        response->print("false");
#endif
        response->print(",\"state\":\""); response->print(officialStateName(state));
        response->print("\",\"busy\":"); response->print(ota_update_is_busy() ? "true" : "false");
        response->print(",\"channel\":\""); response->print(betaEnabled ? "beta" : "stable");
        response->print("\",\"beta_enabled\":"); response->print(betaEnabled ? "true" : "false");
        response->print(",\"auto_update_enabled\":"); response->print(autoUpdateEnabled ? "true" : "false");
        response->print(",\"current_is_prerelease\":"); response->print(isPrereleaseVersion(FIRMWARE_VERSION) ? "true" : "false");
        response->print(",\"current_version\":\""); response->print(FIRMWARE_VERSION);
        response->print("\",\"latest_version\":\""); response->print(utils_escape_json(getOfficialText(s_latestVersion)));
        response->print("\",\"changelog_url\":\""); response->print(OFFICIAL_CHANGELOG_URL);
        response->print("\",\"downloaded_bytes\":"); response->print(s_downloadedBytes.load(std::memory_order_relaxed));
        response->print(",\"total_bytes\":"); response->print(s_totalBytes.load(std::memory_order_relaxed));
        response->print(",\"error\":\""); response->print(utils_escape_json(getOfficialText(s_officialError)));
        response->print("\"}");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    server.on("/api/ota/official", HTTP_POST, [](AsyncWebServerRequest* request) {
#ifdef ESP_IDF5_MIGRATION_BUILD
        const bool checkOnly = request->hasParam("check_only") &&
                               request->getParam("check_only")->value() == "true";
        const bool betaChannel = s_betaEnabled.load(std::memory_order_acquire);
        if (!startOfficialTask(checkOnly, betaChannel)) {
            if (ota_update_is_busy()) {
                request->send(409, "application/json", "{\"success\":false,\"error\":\"Another OTA update is active\"}");
                return;
            }
            request->send(503, "application/json", checkOnly
                ? "{\"success\":false,\"error\":\"Could not start the release check\"}"
                : "{\"success\":false,\"error\":\"Could not start the update task\"}");
            return;
        }
        request->send(202, "application/json", checkOnly
            ? "{\"success\":true,\"message\":\"Checking the selected GitHub release channel without installing\"}"
            : "{\"success\":true,\"message\":\"Checking and installing the selected GitHub release channel\"}");
#else
        s_officialState.store(OfficialState::Unsupported, std::memory_order_release);
        request->send(501, "application/json", "{\"success\":false,\"error\":\"Official updates require the ESP-IDF 5 production build\"}");
#endif
    });

    server.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest* request) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
        BoundedStreamResponse* response = new BoundedStreamResponse("application/json");
        response->print("{\"enabled\":"); response->print(!s_token.isEmpty() && next != nullptr ? "true" : "false");
        response->print(",\"firmware_version\":\""); response->print(FIRMWARE_VERSION);
        response->print("\",\"board\":\""); response->print(HARDWARE_BOARD_NAME);
        response->print("\",\"running_partition\":\""); response->print(partitionLabel(running));
        response->print("\",\"next_partition\":\""); response->print(partitionLabel(next));
        response->print("\",\"max_image_bytes\":"); response->print(next ? next->size : 0);
        response->print(",\"auth_header\":\"X-OTA-Key\",\"integrity_header\":\"X-Firmware-MD5\"}");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    server.on("/api/ota", HTTP_POST,
        [](AsyncWebServerRequest* request) {
            const RequestState state = getState(request);
            if (state == RequestState::Unauthorized) { request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}"); return; }
            if (state == RequestState::Busy) { request->send(409, "application/json", "{\"success\":false,\"error\":\"Another OTA update is active\"}"); return; }
            if (state == RequestState::Invalid || state == RequestState::None) { request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid or empty firmware body\"}"); return; }
            if (state == RequestState::Failed) { request->send(500, "application/json", "{\"success\":false,\"error\":\"Firmware validation or flash write failed\"}"); return; }
            if (state != RequestState::Success) { request->send(400, "application/json", "{\"success\":false,\"error\":\"Incomplete firmware upload\"}"); return; }
            if (!scheduleRestart()) { request->send(503, "application/json", "{\"success\":false,\"error\":\"Firmware installed but restart scheduling failed\"}"); return; }
            request->send(202, "application/json", "{\"success\":true,\"message\":\"Firmware installed; reboot scheduled\"}");
        }, nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                if (!isAuthorized(request)) { setState(request, RequestState::Unauthorized); return; }
                bool expected = false;
                if (!s_updateClaimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) { setState(request, RequestState::Busy); return; }
                if (total == 0 || esp_ota_get_next_update_partition(nullptr) == nullptr) {
                    s_updateClaimed.store(false, std::memory_order_release);
                    setState(request, RequestState::Invalid);
                    return;
                }
                s_activeRequest = request;
                setState(request, RequestState::Uploading);
                log_i("[OTA] Authenticated firmware upload started (%u bytes)", static_cast<unsigned>(total));
                if (!Update.begin(total, U_FLASH)) { failUpload(request, Update.errorString()); return; }
                if (request->hasHeader("X-Firmware-MD5")) {
                    const String md5 = request->header("X-Firmware-MD5");
                    if (md5.length() != 32 || !Update.setMD5(md5.c_str())) { failUpload(request, "invalid MD5 header"); return; }
                }
            }
            if (s_activeRequest != request || getState(request) != RequestState::Uploading) return;
            if (Update.write(data, len) != len) { failUpload(request, Update.errorString()); return; }
            if (index + len == total) {
                if (!Update.end(false)) { failUpload(request, Update.errorString()); return; }
                s_activeRequest = nullptr;
                s_updateClaimed.store(false, std::memory_order_release);
                setState(request, RequestState::Success);
                log_i("[OTA] Firmware image validated and activated (%u bytes)", static_cast<unsigned>(total));
            }
        });
}
