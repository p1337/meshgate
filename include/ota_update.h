#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Registers authenticated, raw-binary HTTP OTA endpoints on the existing
// asynchronous server. An empty token keeps OTA safely disabled.
void ota_update_register(AsyncWebServer& server, const String& token);

// Replaces the in-memory authentication token after a new key has been
// provisioned through the local setup AP. Persistence is owned by config_ui.
void ota_update_set_token(const String& token);
