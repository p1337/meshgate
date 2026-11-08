#include "ip_led.h"
#include "web_log.h"

#include <Arduino.h>

#if defined(ARDUINO_NANO_ESP32)

namespace {

constexpr uint32_t BLINK_ON_MS = 180;
constexpr uint32_t BLINK_OFF_MS = 220;
constexpr uint32_t GROUP_PAUSE_MS = 800;

struct IpBlinkDigits {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

void allOff() {
    // The Nano ESP32 RGB LED is common-anode, so HIGH is off.
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_GREEN, HIGH);
    digitalWrite(LED_BLUE, HIGH);
}
void blinkChannel(uint8_t pin, uint8_t count) {
    for (uint8_t i = 0; i < count; ++i) {
        digitalWrite(pin, LOW);
        vTaskDelay(pdMS_TO_TICKS(BLINK_ON_MS));
        digitalWrite(pin, HIGH);
        vTaskDelay(pdMS_TO_TICKS(BLINK_OFF_MS));
    }
}

void ipBlinkTask(void* parameter) {
    IpBlinkDigits digits = *static_cast<IpBlinkDigits*>(parameter);
    delete static_cast<IpBlinkDigits*>(parameter);

    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    allOff();

    blinkChannel(LED_RED, digits.red);
    vTaskDelay(pdMS_TO_TICKS(GROUP_PAUSE_MS));
    blinkChannel(LED_GREEN, digits.green);
    vTaskDelay(pdMS_TO_TICKS(GROUP_PAUSE_MS));
    blinkChannel(LED_BLUE, digits.blue);
    allOff();

    vTaskDelete(nullptr);
}

} // namespace

#endif

void ip_led_init() {
#if defined(ARDUINO_NANO_ESP32)
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_GREEN, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
    allOff();
    log_i("[IP LED] RGB cleared after boot (R=%d G=%d B=%d)",
          digitalRead(LED_RED), digitalRead(LED_GREEN), digitalRead(LED_BLUE));
#endif
}

void ip_led_show_once(const IPAddress& address) {
#if defined(ARDUINO_NANO_ESP32)
    uint8_t finalOctet = address[3];
    auto* digits = new IpBlinkDigits{
        static_cast<uint8_t>(finalOctet / 100),
        static_cast<uint8_t>((finalOctet / 10) % 10),
        static_cast<uint8_t>(finalOctet % 10)
    };

    if (digits == nullptr) {
        log_e("[IP LED] Could not allocate blink task data");
        return;
    }

    log_i("[IP LED] Showing final IP octet %u once: red=%u, green=%u, blue=%u",
          finalOctet, digits->red, digits->green, digits->blue);

    if (xTaskCreatePinnedToCore(ipBlinkTask, "ip_led_task", 2048, digits, 1, nullptr, 1) != pdPASS) {
        delete digits;
        log_e("[IP LED] Could not create blink task");
    }
#else
    (void)address;
#endif
}

