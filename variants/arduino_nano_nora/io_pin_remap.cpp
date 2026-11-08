#if defined(BOARD_HAS_PIN_REMAP) && !defined(ARDUINO_CORE_BUILD)
#error This build system is not supported. Rebuild without BOARD_HAS_PIN_REMAP.
#endif

#include "Arduino.h"

// Must match the virtual Arduino pin sequence in pins_arduino.h. The custom
// variant deliberately replaces the stock Nano double-tap recovery code, so it
// must also retain the stock pin remapping implementation for Arduino Core 2.
static const int8_t TO_GPIO_NUMBER[] = {
    44, 43, 5, 6, 7, 8, 9, 10, 17, 18, 21, 38, 47, 48,
    46, 0, 45,
    1, 2, 3, 4, 11, 12, 13, 14,
};

#if defined(BOARD_HAS_PIN_REMAP) && !defined(BOARD_USES_HW_GPIO_NUMBERS)

static constexpr int8_t REMAP_TABLE_ENTRIES = sizeof(TO_GPIO_NUMBER) / sizeof(TO_GPIO_NUMBER[0]);

int8_t digitalPinToGPIONumber(int8_t digitalPin) {
    if (digitalPin < 0 || digitalPin >= REMAP_TABLE_ENTRIES) {
        return -1;
    }
    return TO_GPIO_NUMBER[digitalPin];
}

int8_t gpioNumberToDigitalPin(int8_t gpioNumber) {
    if (gpioNumber < 0) {
        return -1;
    }
    for (int8_t digitalPin = 0; digitalPin < REMAP_TABLE_ENTRIES; ++digitalPin) {
        if (TO_GPIO_NUMBER[digitalPin] == gpioNumber) {
            return digitalPin;
        }
    }
    return -1;
}

#endif
