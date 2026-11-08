#include <Arduino.h>
#include "status_led.h"
#include "build_options.h"

#if LED_ACTIVE_LOW
#define LED_ON LOW
#define LED_OFF_STATE HIGH
#else
#define LED_ON HIGH
#define LED_OFF_STATE LOW
#endif

static volatile LedState currentState = LED_OFF;

static void ledTask(void* param) {
#if STATUS_LED_PIN >= 0
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LED_OFF_STATE);
    
    while(true) {
        LedState state = currentState;
        if (state == LED_OFF) {
            digitalWrite(STATUS_LED_PIN, LED_OFF_STATE);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (state == LED_SOLID_ON) {
            digitalWrite(STATUS_LED_PIN, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(100));
        } else if (state == LED_SETUP_PATTERN) {
            // Complex pattern: . . - - (Short Short Long Long)
            struct Blink { int onMs; int offMs; };
            Blink pattern[] = {
                {100, 150}, // Short
                {100, 150}, // Short
                {600, 150}, // Long
                {600, 800}  // Long + Pause
            };
            
            for (int i = 0; i < 4; i++) {
                if (currentState != state) break;
                digitalWrite(STATUS_LED_PIN, LED_ON);
                vTaskDelay(pdMS_TO_TICKS(pattern[i].onMs));
                
                if (currentState != state) break;
                digitalWrite(STATUS_LED_PIN, LED_OFF_STATE);
                vTaskDelay(pdMS_TO_TICKS(pattern[i].offMs));
            }
        } else {
            // Normal uniform blinks
            int delayMs = 500; // LED_MED_BLINK
            if (state == LED_FAST_BLINK) delayMs = 100;
            
            digitalWrite(STATUS_LED_PIN, LED_ON);
            vTaskDelay(pdMS_TO_TICKS(delayMs));
            
            // Check state again so we don't force a full blink cycle if the state changed rapidly
            if (currentState != state) continue;
            
            digitalWrite(STATUS_LED_PIN, LED_OFF_STATE);
            vTaskDelay(pdMS_TO_TICKS(delayMs));
        }
    }
#else
    vTaskDelete(NULL);
#endif
}

void status_led_init() {
#if STATUS_LED_PIN >= 0
    // Run on Core 1 at low priority (1) so it doesn't interrupt anything important
    xTaskCreatePinnedToCore(ledTask, "led_task", 2048, NULL, 1, NULL, 1);
#endif
}

void status_led_set(LedState state) {
    currentState = state;
}
