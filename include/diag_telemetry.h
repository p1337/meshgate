#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <Print.h>
#include "bridge.h"
#include "mqtt_net.h"

/**
 * Point-in-time, wait-free telemetry snapshot of all system components.
 * Aggregates runtime stats across FreeRTOS tasks and cores without holding locks.
 */
struct SystemTelemetrySnapshot {
    // System & Uptime
    uint32_t uptime_seconds = 0;
    uint32_t heap_free_bytes = 0;
    uint32_t heap_min_free_bytes = 0;
    uint32_t heap_max_alloc_bytes = 0;

    // WiFi & Network
    bool wifi_connected = false;
    bool wifi_is_ap = false;
    IPAddress wifi_ip;
    int8_t wifi_rssi = 0;
    String mdns_host = "";

    // Bridge, Queues & Traffic
    BridgeDiagStats bridge = {};

    // MQTT Gateway
    bool mqtt_enabled = false;
    MqttStatus mqtt = {};
};

/**
 * Takes a wait-free snapshot of current system telemetry.
 */
SystemTelemetrySnapshot diag_telemetry_get_snapshot();

/**
 * Formats diagnostic telemetry into a single log string for Serial console output.
 */
void diag_telemetry_format_serial(const SystemTelemetrySnapshot& s, char* buf, size_t maxLen);

/**
 * Formats diagnostic telemetry into a JSON payload for GET /status.
 * Streams directly into Print& without a second large intermediate payload.
 */
void diag_telemetry_format_json(const SystemTelemetrySnapshot& s, Print& out);

/**
 * Formats diagnostic telemetry into Prometheus text format 0.0.4.
 * Streams directly into Print& without a second large intermediate payload.
 */
void diag_telemetry_format_prometheus(const SystemTelemetrySnapshot& s, Print& out);

/**
 * Starts the periodic background diagnostic telemetry task on Core 0.
 * Logs heap, stack watermarks, queue occupancies, and network stats every 15 seconds.
 */
void diag_telemetry_start();
void diag_telemetry_pause_for_ota();
void diag_telemetry_resume_after_ota();
