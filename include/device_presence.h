#pragma once

#include <Arduino.h>

// Starts the bounded installation-presence worker.
void device_presence_init();
void device_presence_start();
void device_presence_set_meshtastic_node(uint32_t nodeNum);

