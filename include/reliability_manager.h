#pragma once

#include "config_ui.h"

// Starts scheduled maintenance reboots and connection supervision. This must
// only be called in normal bridge mode after WiFi has connected.
void reliability_manager_start(const BridgeConfig& cfg);
