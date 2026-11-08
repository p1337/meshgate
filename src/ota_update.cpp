#include "ota_update.h"

#include <Update.h>
#include <esp_ota_ops.h>

#include "build_options.h"
#include "web_log.h"

namespace {
enum class RequestState : uintptr_t {
    None = 0,
    Unauthorized,
    Busy,
    Invalid,
    Uploading,
    Failed,
    Success,
};

String s_token;
AsyncWebServerRequest* s_activeRequest = nullptr;

RequestState getState(AsyncWebServerRequest* request) {
    return static_cast<RequestState>(reinterpret_cast<uintptr_t>(request->_tempObject));
}

void setState(AsyncWebServerRequest* request, RequestState state) {
    request->_tempObject = reinterpret_cast<void*>(static_cast<uintptr_t>(state));
}

bool constantTimeEquals(const String& lhs, const String& rhs) {
    if (lhs.length() != rhs.length()) {
        return false;
    }
    uint8_t difference = 0;
    for (size_t i = 0; i < lhs.length(); ++i) {
        difference |= static_cast<uint8_t>(lhs[i] ^ rhs[i]);
    }
    return difference == 0;
}

bool isAuthorized(AsyncWebServerRequest* request) {
    return !s_token.isEmpty() && request->hasHeader("X-OTA-Key") &&
           constantTimeEquals(request->header("X-OTA-Key"), s_token);
}

void failUpload(AsyncWebServerRequest* request, const char* reason) {
    log_e("[OTA] Update failed: %s", reason);
    if (Update.isRunning()) {
        Update.abort();
    }
    if (s_activeRequest == request) {
        s_activeRequest = nullptr;
    }
    setState(request, RequestState::Failed);
}

void otaRestartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP.restart();
    vTaskDelete(nullptr);
}

bool scheduleRestart() {
    return xTaskCreate(otaRestartTask, "ota-restart", 2048, nullptr, 1, nullptr) == pdPASS;
}

String partitionLabel(const esp_partition_t* partition) {
    return partition ? String(partition->label) : String();
}
} // namespace

void ota_update_set_token(const String& token) {
    s_token = token;
}

void ota_update_register(AsyncWebServer& server, const String& token) {
    ota_update_set_token(token);

    server.on("/api/ota", HTTP_GET, [](AsyncWebServerRequest* request) {
        const esp_partition_t* running = esp_ota_get_running_partition();
        const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);

        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{\"enabled\":");
        response->print(!s_token.isEmpty() && next != nullptr ? "true" : "false");
        response->print(",\"firmware_version\":\"");
        response->print(FIRMWARE_VERSION);
        response->print("\",\"board\":\"");
        response->print(HARDWARE_BOARD_NAME);
        response->print("\",\"running_partition\":\"");
        response->print(partitionLabel(running));
        response->print("\",\"next_partition\":\"");
        response->print(partitionLabel(next));
        response->print("\",\"max_image_bytes\":");
        response->print(next ? next->size : 0);
        response->print(",\"auth_header\":\"X-OTA-Key\",\"integrity_header\":\"X-Firmware-MD5\"}");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    server.on(
        "/api/ota",
        HTTP_POST,
        [](AsyncWebServerRequest* request) {
            const RequestState state = getState(request);
            if (state == RequestState::Unauthorized) {
                request->send(401, "application/json", "{\"success\":false,\"error\":\"Unauthorized\"}");
                return;
            }
            if (state == RequestState::Busy) {
                request->send(409, "application/json", "{\"success\":false,\"error\":\"Another OTA update is active\"}");
                return;
            }
            if (state == RequestState::Invalid || state == RequestState::None) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Invalid or empty firmware body\"}");
                return;
            }
            if (state == RequestState::Failed) {
                request->send(500, "application/json", "{\"success\":false,\"error\":\"Firmware validation or flash write failed\"}");
                return;
            }
            if (state != RequestState::Success) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"Incomplete firmware upload\"}");
                return;
            }

            if (!scheduleRestart()) {
                request->send(503, "application/json", "{\"success\":false,\"error\":\"Firmware installed but restart scheduling failed\"}");
                return;
            }
            request->send(202, "application/json", "{\"success\":true,\"message\":\"Firmware installed; reboot scheduled\"}");
        },
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                if (!isAuthorized(request)) {
                    setState(request, RequestState::Unauthorized);
                    return;
                }
                if (s_activeRequest != nullptr) {
                    setState(request, RequestState::Busy);
                    return;
                }
                if (total == 0 || esp_ota_get_next_update_partition(nullptr) == nullptr) {
                    setState(request, RequestState::Invalid);
                    return;
                }

                s_activeRequest = request;
                setState(request, RequestState::Uploading);
                log_i("[OTA] Authenticated firmware upload started (%u bytes)", static_cast<unsigned>(total));

                if (!Update.begin(total, U_FLASH)) {
                    failUpload(request, Update.errorString());
                    return;
                }

                if (request->hasHeader("X-Firmware-MD5")) {
                    const String md5 = request->header("X-Firmware-MD5");
                    if (md5.length() != 32 || !Update.setMD5(md5.c_str())) {
                        failUpload(request, "invalid MD5 header");
                        return;
                    }
                }
            }

            if (s_activeRequest != request || getState(request) != RequestState::Uploading) {
                return;
            }

            if (Update.write(data, len) != len) {
                failUpload(request, Update.errorString());
                return;
            }

            if (index + len == total) {
                if (!Update.end(false)) {
                    failUpload(request, Update.errorString());
                    return;
                }
                s_activeRequest = nullptr;
                setState(request, RequestState::Success);
                log_i("[OTA] Firmware image validated and activated (%u bytes)", static_cast<unsigned>(total));
            }
        });
}
