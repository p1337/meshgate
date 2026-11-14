#pragma once

#include "config_ui.h"

// Starts WireGuard asynchronously after WiFi and the system clock are ready.
// The BLE/TCP bridge remains available on the local network while the tunnel
// is being established.
void wireguard_net_start(const BridgeConfig& cfg);
bool wireguard_net_pause_for_ota();
void wireguard_net_resume_after_ota();

bool wireguard_net_is_enabled();
bool wireguard_net_is_initialized();
bool wireguard_net_is_connected();
