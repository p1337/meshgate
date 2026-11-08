#pragma once

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

// Registers authenticated, raw-binary HTTP OTA endpoints on the existing
// asynchronous server. An empty token keeps OTA safely disabled.
void ota_update_register(AsyncWebServer& server, const String& token);
