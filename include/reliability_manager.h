#pragma once

#include "config_ui.h"
#include <stdint.h>

// Starts scheduled maintenance reboots and connection supervision. This must
// only be called in normal bridge mode after WiFi has connected.
void reliability_manager_start(const BridgeConfig& cfg);

// Requests a delayed restart through the already-running supervisor task.
// This path performs no task or heap allocation and remains available when
// network services have fragmented the remaining heap.
bool reliability_manager_request_restart(uint32_t delayMs = 1000,
                                         bool allowDuringOta = false);
