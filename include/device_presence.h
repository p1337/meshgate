#pragma once

#include <Arduino.h>

// Sends the boot presence request once WiFi is online and arms periodic checks.
void device_presence_init();
void device_presence_start();
void device_presence_loop();
void device_presence_pause_for_ota();
void device_presence_resume_after_ota();
void device_presence_request_now();
void device_presence_set_meshtastic_node(uint32_t nodeNum);

