#include <Arduino.h>

// The stock Nano ESP32 variant implements double-tap recovery by switching to
// a factory partition. Meshgate deliberately uses two 6.25 MiB OTA slots and
// has no factory partition, so that stock handler can enter an endless reboot
// loop after two quick resets. ROM download mode (B1/GPIO0 to GND + RST) stays
// available; only the incompatible factory-partition redirect is omitted.
extern "C" void initVariant() {
}
