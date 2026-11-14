#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Registers authenticated, raw-binary HTTP OTA endpoints on the existing
// asynchronous server. An empty token keeps OTA safely disabled.
void ota_update_register(AsyncWebServer& server, const String& token);

// Replaces the in-memory authentication token after a new key has been
// provisioned through the local setup AP. Persistence is owned by config_ui.
void ota_update_set_token(const String& token);

// Checks an administrator request against the write-only OTA key. This is
// used for dangerous update-channel changes without exposing the key through
// the configuration API or embedding it in the browser application.
bool ota_update_is_authorized(AsyncWebServerRequest* request);

// Applies the persistent release policy. Stable is the default channel;
// automatic installation is opt-out. Changing the channel resets cached
// release metadata and schedules a fresh automatic check when enabled.
void ota_update_set_policy(bool betaEnabled, bool autoUpdateEnabled);

// Enables the background update scheduler after normal-mode networking and
// critical services have started. Setup/captive-portal mode never calls this.
void ota_update_start_automatic_checks();

// Allocation-free scheduler hook for the Arduino loop.
void ota_update_loop();

// True while release metadata, download, validation or raw flash writing owns
// the OTA path. Recovery logic must not restart the device in this window.
bool ota_update_is_busy();
