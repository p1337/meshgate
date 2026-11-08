#pragma once

#include <IPAddress.h>

// Put the Nano ESP32 RGB LED into a defined OFF state immediately after boot.
// The ROM/WebSerial flasher can otherwise leave red and blue enabled, which
// looks like a persistent purple bootloader/error indication even though the
// application is already running. Other boards compile this as a no-op.
void ip_led_init();

// On the Arduino Nano ESP32, blink the decimal digits of the final IPv4 octet
// exactly once: hundreds in red, tens in green, ones in blue.
// Other boards compile this as a no-op.
void ip_led_show_once(const IPAddress& address);
