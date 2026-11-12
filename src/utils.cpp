#include "utils.h"
#include "esp_log.h"

bool g_debug_logs = false;

/**
 * Enable/Disable debug logging.
 * @param enableDebugLogs true to enable debug logging, false to disable
 */
void utils_set_debug_logging(bool enableDebugLogs) {
    g_debug_logs = enableDebugLogs;
    
    if (enableDebugLogs) {
        // Set default log level to INFO.
        esp_log_level_set("*", ESP_LOG_INFO);
    } else {
        // Set default log level to warning.
        esp_log_level_set("*", ESP_LOG_WARN);
    }
}

/**
 * Stream an escaped JSON string directly to a Print stream, avoiding any heap allocation.
 * @param out Destination Print stream
 * @param input Raw null-terminated C string to escape
 */
void utils_escape_json_print(Print& out, const char* input) {
    if (!input) return;
    while (*input) {
        char c = *input++;
        if (c == '"') out.print("\\\"");
        else if (c == '\\') out.print("\\\\");
        else if (c == '\b') out.print("\\b");
        else if (c == '\f') out.print("\\f");
        else if (c == '\n') out.print("\\n");
        else if (c == '\r') out.print("\\r");
        else if (c == '\t') out.print("\\t");
        else if (c >= 32 && c <= 126) out.print(c);
    }
}

/**
 * Overload to stream an escaped Arduino String directly to a Print stream.
 */
void utils_escape_json_print(Print& out, const String& input) {
    utils_escape_json_print(out, input.c_str());
}

/**
 * Given a string json value, escape it.
 * @param input Value to be escaped.
 * @return Escaped input.
 */
String utils_escape_json(const String& input) {
    String output;
    output.reserve(input.length() + 16);
    const char* p = input.c_str();
    while (*p) {
        char c = *p++;
        if (c == '"') output += "\\\"";
        else if (c == '\\') output += "\\\\";
        else if (c == '\b') output += "\\b";
        else if (c == '\f') output += "\\f";
        else if (c == '\n') output += "\\n";
        else if (c == '\r') output += "\\r";
        else if (c == '\t') output += "\\t";
        else if (c >= 32 && c <= 126) output += c;
    }
    return output;
}

/**
 * @param macStr MAC Address to parse
 * @return NimBLEAddress configured with the appropriate type and MAC address
 */
NimBLEAddress utils_parse_ble_address(const String& macStr) {
    uint8_t addrType = BLE_ADDR_PUBLIC; // Default to public

    if (macStr.length() > 0) {
        char firstChar = macStr.charAt(0);
        // Bluetooth Core Specification: Random Static addresses have the two most
        // significant bits set to 1. In hex, this means they start with C, D, E, or F.
        if (firstChar == 'c' || firstChar == 'C' ||
            firstChar == 'd' || firstChar == 'D' ||
            firstChar == 'e' || firstChar == 'E' ||
            firstChar == 'f' || firstChar == 'F') {
            addrType = BLE_ADDR_RANDOM;
        }
    }

    return NimBLEAddress(macStr.c_str(), addrType);
}
