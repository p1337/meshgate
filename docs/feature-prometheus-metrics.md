# Feature: Prometheus & OpenMetrics Exporter (`GET /metrics`)

## Overview
This feature adds a standards-compliant Prometheus / OpenMetrics scraping endpoint (`GET /metrics`) to the bridge's existing asynchronous web server (`ESPAsyncWebServer`). 

External monitoring platforms (such as Prometheus, VictoriaMetrics, Grafana Agent, and Home Assistant) can scrape this endpoint on a periodic schedule (e.g. every 15s to 60s) to monitor long-term ESP32 health, queue backpressure, wireless signal stability, TCP client concurrency, and MQTT proxy throughput.

This feature also unifies all system diagnostics across the bridge:
1. **Prometheus Exposition Endpoint (`GET /metrics`)**
2. **WebUI Status Endpoint (`GET /status`)**
3. **Serial Diagnostic Console Logger (`diag_telemetry`)**

This document builds directly upon the groundwork established in [docs/feature-webui-metrics.md](file:///Users/spowis/Documents/code/meshtastic-esp32-bt-bridge/docs/feature-webui-metrics.md).

---

## Core Requirements & System Principles

Every architectural choice for the metrics endpoint must adhere to three non-negotiable requirements:

1. **Zero Impact on Fast-Path Processing Loops:**
   * Telemetry publishing must never reduce bridging throughput or introduce jitter into the Core 1 BLE packet engine, TCP server, or Core 0 MQTT proxy loops.
   * Execution must take place strictly in the asynchronous web server context (Core 0 AsyncTCP / lwIP) at normal priority, yielding immediately to packet forwarding.

2. **Lock-Free & Wait-Free Constructs Exclusively:**
   * Handlers must read all telemetry via `std::atomic` variables (`std::memory_order_relaxed`) and non-blocking FreeRTOS inquiry functions (`uxQueueMessagesWaiting`, `uxTaskGetStackHighWaterMark`).
   * **Never acquire heavy mutexes** (e.g. `tcpClientsMutex`, `mqttMutex`, BLE client locks) inside HTTP endpoint handlers.
   * Accept slightly staggered point-in-time telemetry snapshots to guarantee 100% non-blocking execution.

3. **Zero Heap Fragmentation & Minimal Memory Overhead:**
   * Microcontrollers are susceptible to heap fragmentation from repeated String allocations.
   * We avoid building a monolithic 2KB+ `String` on the heap with repeated reallocations (`+=`).
   * Use `ESPAsyncWebServer`'s chunked `AsyncResponseStream` (`request->beginResponseStream("text/plain; version=0.0.4; charset=utf-8")`) to stream metric lines directly into lwIP network buffers, or format lines using a fixed stack-allocated scratch buffer. This results in deterministic, near-zero heap footprint and zero heap fragmentation even during aggressive scrape intervals (e.g., 5s).

---

## Unified Diagnostics Architecture (`diag_telemetry`)

To ensure complete consistency across all three telemetry consumers without duplicating sampling logic or risking desyncs, `diag_telemetry` serves as the central diagnostics engine:

```
                          ┌───────────────────────────┐
                          │    System State Sources   │
                          │ (Atomics, Queues, WiFi)   │
                          └─────────────┬─────────────┘
                                        │
                                        ▼
                        ┌───────────────────────────────┐
                        │ diag_telemetry_get_snapshot() │
                        │  (SystemTelemetrySnapshot)    │
                        └───────┬───────┬───────┬───────┘
                                │       │       │
            ┌───────────────────┘       │       └───────────────────┐
            ▼                           ▼                           ▼
┌───────────────────────┐   ┌───────────────────────┐   ┌───────────────────────┐
│ diag_telemetry_format │   │ diag_telemetry_format │   │ diag_telemetry_format │
│      _prometheus()    │   │         _json()       │   │        _serial()      │
│     (GET /metrics)    │   │      (GET /status)    │   │    (15s Background)   │
└───────────────────────┘   └───────────────────────┘   └───────────────────────┘
```

1. **Canonical Snapshot (`SystemTelemetrySnapshot`)**:
   Captures a point-in-time, wait-free snapshot of:
   * System memory (Free, Min Free, Max Alloc heap)
   * Uptime seconds
   * WiFi RSSI, IP, connection state, mDNS hostname
   * FreeRTOS Task stack watermarks (`bridge_ble`, `bridge_net`)
   * FreeRTOS Queue occupancies and capacity drops (`tcp_to_ble`, `ble_to_tcp`, `mqtt_to_ble`)
   * Symmetric channel traffic & invalid error counters (`ble`, `tcp`, `mqtt`)
   * MQTT gateway connection state and parameters

2. **Streamed Prometheus Generation**:
   `diag_telemetry_format_prometheus(const SystemTelemetrySnapshot& s, Print& out)` accepts a `Print&` stream, allowing `AsyncResponseStream` to directly emit lines into network buffers without dynamic heap allocation.

3. **Unified JSON Generation**:
   `diag_telemetry_format_json(const SystemTelemetrySnapshot& s, String& jsonOut)` constructs the single JSON payload for `/status` using a single pre-reserved buffer.

4. **Console Log Formatting**:
   `diag_telemetry_format_serial(const SystemTelemetrySnapshot& s, char* buf, size_t maxLen)` formats the periodic 15-second console log line including the new traffic throughput stats.

---

## Metric Naming & Label Architecture

### Metric Family Prefix: `meshbridge_`
To avoid collision with other Prometheus exporters while keeping payload sizes compact, all metrics use the prefix `meshbridge_`.

### Target Identity & Fleet Monitoring (Info Metric Pattern)
Prometheus scrapers natively differentiate individual devices via target labels (`instance="192.168.1.145:80"`, `job="meshbridge"`). To support friendly naming and fleet dashboards in Grafana without adding high cardinality to every metric line, a single info metric is exposed:

```text
# HELP meshbridge_info Metadata and identity of the bridge
# TYPE meshbridge_info gauge
meshbridge_info{version="v0.7.0",mdns="dsc-ae28.local",radio_mac="AA:BB:CC:DD:EE:FF",board="seeed_xiao_esp32s3"} 1
```

### Symmetric Channel Traffic Matrix
All three communication channels (**BLE**, **TCP**, **MQTT**) report traffic through unified metric families labeled by `channel`:

```text
# HELP meshbridge_messages_rx_total Valid packets received at channel boundary
# TYPE meshbridge_messages_rx_total counter
meshbridge_messages_rx_total{channel="ble"} 1420
meshbridge_messages_rx_total{channel="tcp"} 350
meshbridge_messages_rx_total{channel="mqtt"} 82

# HELP meshbridge_messages_tx_total Packets transmitted across channel boundary
# TYPE meshbridge_messages_tx_total counter
meshbridge_messages_tx_total{channel="ble"} 310
meshbridge_messages_tx_total{channel="tcp"} 700
meshbridge_messages_tx_total{channel="mqtt"} 120

# HELP meshbridge_messages_invalid_total Malformed, oversized, or unparseable packets rejected
# TYPE meshbridge_messages_invalid_total counter
meshbridge_messages_invalid_total{channel="ble"} 0
meshbridge_messages_invalid_total{channel="tcp"} 0
meshbridge_messages_invalid_total{channel="mqtt"} 0
```

### Complete Metric Catalog

#### 1. Identity & System Memory
* `meshbridge_info{version="...",mdns="...",radio_mac="...",board="..."}` (gauge, always `1`)
* `meshbridge_uptime_seconds` (counter)
* `meshbridge_heap_free_bytes` (gauge)
* `meshbridge_heap_min_free_bytes` (gauge)
* `meshbridge_heap_max_alloc_bytes` (gauge)
* `meshbridge_task_stack_free_bytes{task="ble"|"net"}` (gauge)

#### 2. Wireless & Network Status
* `meshbridge_wifi_connected` (gauge: 0 or 1)
* `meshbridge_wifi_rssi_dbm` (gauge)
* `meshbridge_ble_connected` (gauge: 0 or 1)
* `meshbridge_tcp_clients` (gauge)
* `meshbridge_tcp_clients_max` (gauge: constant `3`)

#### 3. Traffic & Packet Integrity
* `meshbridge_messages_rx_total{channel="ble"|"tcp"|"mqtt"}` (counter)
* `meshbridge_messages_tx_total{channel="ble"|"tcp"|"mqtt"}` (counter)
* `meshbridge_messages_invalid_total{channel="ble"|"tcp"|"mqtt"}` (counter)

#### 4. FreeRTOS Queues (Capacity Overflows)
* `meshbridge_queue_messages{queue="tcp_to_ble"|"ble_to_tcp"|"mqtt_to_ble"}` (gauge)
* `meshbridge_queue_capacity{queue="tcp_to_ble"|"ble_to_tcp"|"mqtt_to_ble"}` (gauge)
* `meshbridge_queue_dropped_total{queue="tcp_to_ble"|"ble_to_tcp"|"mqtt_to_ble"}` (counter, strictly queue full drops)

#### 5. MQTT Gateway Status
* `meshbridge_mqtt_enabled` (gauge: 0 or 1)
* `meshbridge_mqtt_connected` (gauge: 0 or 1)

---

## Implementation Plan & Tasks

### Checklist / TODOs
- [x] **Task 1: Feature Proposal & Alignment**
  - Propose metric names, types, and formatting with user.
  - Settle buffer management strategy for `ESPAsyncWebServer` response (`AsyncResponseStream`).
  - Establish symmetric RX / TX / Invalid / Queue Dropped traffic model across all channels.
  - Settle prefix (`meshbridge_`) and Info Metric pattern (`meshbridge_info`).
- [x] **Task 2: Channel Traffic Counters in `bridge.h`, `bridge.cpp`, `mqtt_net.h`, and `mqtt_net.cpp`**
  - Add atomic counters in `bridge.cpp`: `s_ble_rx_count`, `s_ble_tx_count`, `s_ble_invalid_count`.
  - Add atomic counters in `bridge.cpp`: `s_tcp_rx_count`, `s_tcp_tx_count`, `s_tcp_invalid_count`.
  - Add atomic counter in `bridge.cpp`: `s_mqtt_to_ble_dropped` (strictly queue full).
  - Add atomic counter in `mqtt_net.cpp`: `s_msgs_invalid` (rejected payloads/topics/encode errors).
  - Update `BridgeDiagStats` and `MqttStatus` structs to expose these counters atomically.
- [x] **Task 3: Refactor `diag_telemetry` into Central Diagnostics Engine**
  - Define `SystemTelemetrySnapshot` struct in `include/diag_telemetry.h`.
  - Implement `diag_telemetry_get_snapshot()`.
  - Implement `diag_telemetry_format_serial()` for 15s console logging.
  - Implement `diag_telemetry_format_json()` for `GET /status`.
  - Implement `diag_telemetry_format_prometheus()` for `GET /metrics`.
- [x] **Task 4: Update Endpoints & WebUI in `config_ui.cpp`**
  - Wire `GET /status` to `diag_telemetry_format_json()`.
  - Wire `GET /metrics` to `diag_telemetry_format_prometheus()`.
  - Update WebUI to display the new channel traffic counters (RX / TX / Invalid).
- [x] **Task 5: Memory & Concurrency Validation**
  - Verify wait-free execution across Core 0 and Core 1.
  - Verify zero heap fragmentation or spikes during scraping.
- [x] **Task 6: PlatformIO Multi-Board Build Verification**
  - Compile and verify on all target board profiles (`seeed_xiao_esp32s3`, `esp32dev`, `esp32-s3-devkitc-1`).
- [x] **Task 7: Comprehensive End-User Documentation for `/metrics`**
  - Document the `/metrics` endpoint in `docs/user_guide.md` and `docs/configuration_ui.md`.
  - Provide a complete reference table with every metric name, type, label set, and exact human-readable definition.
  - Include an example Prometheus scrape job (`prometheus.yml`) and sample PromQL alerting rules (e.g. BLE disconnect alert, memory leak alert, queue drop alert).
- [x] **Task 8: Roadmap & Project Tracking**
  - Update `TODO.md` to record completion of Prometheus Exporter and unified diagnostics.

---

## Technical Notes, Issues & Decisions Log
*(Maintained throughout development)*

* **Decision 1 (Oct 12, 2026):** Use compact standard prefix `meshbridge_` with base units (`_seconds`, `_bytes`, `_total` for counters, `_dbm`).
* **Decision 2 (Oct 12, 2026):** Consolidate all telemetry snapshotting and formatting (Prometheus, JSON for WebUI, Serial logger) into `diag_telemetry` to guarantee 100% consistency and eliminate duplicated sampling code.
* **Decision 3 (Oct 12, 2026):** Adopt symmetric channel metrics (`meshbridge_messages_rx_total{channel="..."}`, `tx_total`, `invalid_total`) across BLE, TCP, and MQTT. Differentiate congestion drops (`queue_dropped_total`) from payload/framing errors (`messages_invalid_total`).
* **Decision 4 (Oct 12, 2026):** Use `AsyncResponseStream` streaming directly to lwIP buffers to prevent heap fragmentation.
* **Decision 5 (Oct 12, 2026):** Expose `meshbridge_info` with metadata labels (`version`, `mdns`, `radio_mac`, `board`) rather than repeating identity labels across every operational metric line.
