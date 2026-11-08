#include "web_log.h"

#include <esp_log.h>
#include <stdarg.h>
#include <stdio.h>

namespace {
constexpr size_t LOG_CAPACITY = 16384;
constexpr size_t LINE_CAPACITY = 512;

char s_logBuffer[LOG_CAPACITY] = {};
size_t s_writePosition = 0;
size_t s_logSize = 0;
portMUX_TYPE s_logMux = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t s_previousVprintf = nullptr;
bool s_initialized = false;

void appendLog(const char* data, size_t length) {
    portENTER_CRITICAL(&s_logMux);
    for (size_t i = 0; i < length; ++i) {
        s_logBuffer[s_writePosition] = data[i];
        s_writePosition = (s_writePosition + 1) % LOG_CAPACITY;
        if (s_logSize < LOG_CAPACITY) {
            ++s_logSize;
        }
    }
    portEXIT_CRITICAL(&s_logMux);
}

int captureVprintf(const char* format, va_list args) {
    va_list serialArgs;
    va_list captureArgs;
    va_copy(serialArgs, args);
    va_copy(captureArgs, args);

    const int result = s_previousVprintf ? s_previousVprintf(format, serialArgs)
                                         : vprintf(format, serialArgs);

    char line[LINE_CAPACITY];
    const int formatted = vsnprintf(line, sizeof(line), format, captureArgs);
    if (formatted > 0) {
        const size_t length = min(static_cast<size_t>(formatted), sizeof(line) - 1);
        appendLog(line, length);
    }

    va_end(serialArgs);
    va_end(captureArgs);
    return result;
}
} // namespace

extern "C" int web_log_printf(const char* format, ...) {
    char line[LINE_CAPACITY];

    va_list args;
    va_start(args, format);
    const int formatted = vsnprintf(line, sizeof(line), format, args);
    va_end(args);

    if (formatted <= 0) {
        return formatted;
    }

    const size_t length = min(static_cast<size_t>(formatted), sizeof(line) - 1);
    appendLog(line, length);

    // Native USB serial can block an unattended Nano for many seconds when no
    // host has opened the CDC port. Preserve serial diagnostics only while a
    // real host is attached; the RAM/API log is always populated above.
    if (Serial) {
        Serial.write(reinterpret_cast<const uint8_t*>(line), length);
    }
    return formatted;
}

void web_log_init() {
    if (s_initialized) {
        return;
    }
    s_previousVprintf = esp_log_set_vprintf(captureVprintf);
    s_initialized = true;
}

String web_log_snapshot() {
    String snapshot;

    portENTER_CRITICAL(&s_logMux);
    const size_t capturedSize = s_logSize;
    const size_t start = (s_writePosition + LOG_CAPACITY - capturedSize) % LOG_CAPACITY;
    snapshot.reserve(capturedSize + 1);
    for (size_t i = 0; i < capturedSize; ++i) {
        snapshot += s_logBuffer[(start + i) % LOG_CAPACITY];
    }
    portEXIT_CRITICAL(&s_logMux);

    return snapshot;
}
