#pragma once

#include "config_ui.h"
#include <stdint.h>

// Starts the persistent restart supervisor in every boot mode. Runtime link,
// memory and maintenance supervision is enabled only for normal bridge mode;
// setup mode still needs the allocation-free Web UI restart path.
void reliability_manager_start(const BridgeConfig& cfg,
                               bool enableRuntimeSupervision);

// Requests a delayed restart through the already-running supervisor task.
// This path performs no task or heap allocation and remains available when
// network services have fragmented the remaining heap.
bool reliability_manager_request_restart(uint32_t delayMs = 1000,
                                         bool allowDuringOta = false);
