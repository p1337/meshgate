#pragma once

#include <IPAddress.h>

// On the Arduino Nano ESP32, blink the decimal digits of the final IPv4 octet
// exactly once: hundreds in red, tens in green, ones in blue.
// Other boards compile this as a no-op.
void ip_led_show_once(const IPAddress& address);
