#pragma once

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compile-time replacement for Arduino's log_printf. It preserves serial
// output and also appends the fully formatted line to the remote log ring.
int web_log_printf(const char* format, ...);

#ifdef __cplusplus
}
#endif

// Arduino's log_i/log_w/etc. macros resolve log_printf when they are expanded,
// so project sources that include this header are transparently mirrored.
#define log_printf web_log_printf

// Mirrors ESP-IDF/Arduino log output into a bounded in-memory ring buffer while
// preserving the normal serial console output.
void web_log_init();

// Returns the retained log output in chronological order.
String web_log_snapshot();
