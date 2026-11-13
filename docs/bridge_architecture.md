# Meshtastic ESP32 BLE-to-TCP Bridge Architecture

## Overview
This document outlines the architecture for the core BLE-to-TCP bridge functionality of our C++ ESP32-S3 firmware. It is heavily based on lessons learned from the `meshtastic-ble-bridge` Python reference implementation.

The primary goal of the bridge is to translate between the TCP stream protocol expected by tools like MeshMonitor and the Bluetooth Low Energy (BLE) GATT characteristics exposed by a Meshtastic device.

## Protocol Translation

### 1. TCP Protocol (Client side)
MeshMonitor expects a continuous TCP stream on port 4403. Messages are framed to define their boundaries:
- **Header**: 4 bytes `[0x94] [0xC3] [LENGTH_MSB] [LENGTH_LSB]`
- **Payload**: Raw Protobuf bytes (up to 512 bytes)
- **Behavior**: The ESP32 will act as a TCP Server. It must parse the stream, strip the 4-byte header, and forward the payload to the BLE node.

### 2. BLE Protocol (Meshtastic Node side)
The ESP32 will act as a BLE Client and connect to the target Meshtastic Node (BLE Peripheral).
- **Service UUID**: `6ba1b218-15a8-461f-9fa8-5dcae273eafd`
- **ToRadio (Write)**: `f75c76d2-129e-4dad-a1dd-7866124401e7`
  - Used to send data to the node. Raw protobufs only (no framing).
- **FromRadio (Read/Notify)**: `2c55e69e-4993-11ed-b878-0242ac120002`
  - Used to receive data from the node. Raw protobufs only.
- **FromNum (Notify)**: `ed9da18c-a800-4f66-a670-aa7547e34453`
  - Triggers when a new packet is available to be read from `FromRadio`.

### 3. HTTP Protocol (browser client side)
- **Write:** `PUT /api/v1/toradio` carries exactly one raw `ToRadio` protobuf.
- **Read:** `GET /api/v1/fromradio?all=false` returns one raw `FromRadio`
  protobuf or an empty body. `all=true` drains all currently queued packets.
- **Framing:** no TCP `0x94 0xC3` header is used.
- **Browser access:** both endpoints answer `OPTIONS`, CORS, and Chromium
  Private Network Access preflights. The API is HTTP on port 80; Tailscale
  supplies transport encryption for remote access.
- **Buffering:** a dedicated static `ble_to_http_queue` absorbs HTTP round-trip
  latency. If it fills during an active HTTP session, BLE draining pauses for
  every consumer so the radio's queue supplies lossless backpressure. A stale HTTP session is expired and
  cleared after 15 seconds without a request.

### 4. BLE Addressing & Auto-Detection
Meshtastic devices utilize different BLE address types depending on the manufacturer:
- **ESP32 Nodes** (e.g., T-Beam, Heltec) typically use **Public Addresses**.
- **nRF52 Nodes** (e.g., SenseCAP, RAK) typically use **Random Static Addresses** (mathematically required to start with `C`, `D`, `E`, or `F` hex characters).
- To support seamless bridging to any device, our `utils_parse_ble_address` dynamically inspects the MSB of the configured MAC address. If it detects a C/D/E/F prefix, it forces `BLE_ADDR_RANDOM`, preventing connection timeouts across all hardware architectures.

## C++ ESP32 Implementation Strategy

### 1. Multithreading, Tasks & Core Pinning
Because BLE operations (connecting, reading characteristics) can block, and we are using `ESPAsyncTCP` (which runs on Core 0), we separate concerns using FreeRTOS tasks, strict priorities, and static queues:

| Task Name | Core | Priority | Stack Size | Purpose / Responsibilities |
| :--- | :---: | :---: | :---: | :--- |
| **`LwIP / TCP Stack`** | Core 0 | **18** | System | Internal ESP-IDF IP/MAC networking stack. |
| **`AsyncTCP Worker`** | Core 0 | **3** | System | Handles async socket read/write events and WebUI endpoints. |
| **`mqtt_client` Task** | Core 0 | **2** | 8192 B | Background MQTT networking, keepalive pings, and TLS processing. |
| **`bridgeNetTask`** | Core 0 | **1** | 4096 B | Network broadcast task: drains `ble_to_tcp_queue` and writes framed packets to connected TCP clients under `tcpClientsMutex`. |
| **`diag_telemetry`** | Core 0 | **1** | 3072 B | Periodic background task (15s): outputs atomic single-line health diagnostics (Heap, Min/MaxBlock, WiFi RSSI, TCP clients, Queue wait/capacity/drops, Task Stacks, MQTT status). |
| **`bridgeBleTask`** | Core 1 | **1** | 8192 B | BLE Engine: manages connection/reconnection, polls `FromRadio`, handles `FromNum` notifications, inspects/routes uplink packets via `mqtt_net_handle_from_radio()`, and writes prioritized packets (`tcp_to_ble_queue` then `mqtt_to_ble_queue`) to `ToRadio`. Yields 1ms (`vTaskDelay(1)`) each cycle for co-located tasks. |

> [!NOTE]
> **Priority Hierarchy Rationale:** Keeping `mqtt_client` at Priority 2 (below `AsyncTCP` at 3 and LwIP at 18) guarantees that heavy TLS/MQTT operations will never starve WebUI interactions or incoming TCP socket connections. Giving `bridgeNetTask`, `bridgeBleTask`, and `diag_telemetry` Priority 1 ensures equal round-robin scheduling for bridging and telemetry.

### 2. BLE Interaction Flow
- **Connect**: Scan and connect to the configured BLE MAC address (`cfg.ble_mac`).
- **Subscribe**: Register notification callbacks on `FromNum` and `FromRadio`.
- **Receive (BLE -> Network / MQTT)**: 
  - When notified or on fast-poll, read `FromRadio`.
  - Decode `TEXT_MESSAGE_APP` packets into the persistent mailbox before any
    transport-specific consumer can consume the envelope. The mailbox keeps
    the BLE drain active even when no app or MQTT client is online.
  - Pass the raw bytes to `mqtt_net_handle_from_radio()`:
    - If it contains `ModuleConfig.mqtt`, automatically syncs broker address, port, and credentials.
    - If it contains `MqttClientProxyMessage`, publishes directly to the active MQTT broker and consumes the packet.
  - If not consumed and active TCP clients exist, prepend `0x94 0xC3` framing and push to `ble_to_tcp_queue`.
  - Core 0 network task broadcasts framed packet to all active TCP clients.
  - After a client receives the `config_complete_id` matching its own
    `want_config_id`, replay a bounded snapshot of the retained mailbox directly
    to that client. Browser PhoneAPI applies the same handshake gate.
- **Transmit (Network / MQTT -> BLE)**:
  - High-priority TCP app frames are parsed, stripped of framing, and pushed to `tcp_to_ble_queue`.
  - Background MQTT downlink packets are framed as `ToRadio` protobufs and pushed to `mqtt_to_ble_queue`.
  - The BLE task on Core 1 pops the highest priority packet and calls `writeValue(..., false)` (Write Without Response) on `ToRadio`.

### 3. Reconnection, Grace Period & Stability
- **60-Second Disconnect Grace Period**: If the Bluetooth connection drops momentarily, the bridge keeps the MQTT broker connection alive for 60 seconds and buffers incoming messages in `mqtt_to_ble_queue`. If BLE reconnects within 60s, the session is preserved with 0 dropped messages. If 60s elapses, the client is cleanly disconnected and the queue is cleared.
- **MTU Negotiation**: We request an MTU of at least 512 bytes during the BLE connection phase to support maximum Meshtastic protobuf payloads.

### 4. Memory Management (100% Static `.bss` Queues)
- **Zero Heap Fragmentation**: All FreeRTOS queues (`tcp_to_ble_queue`, `ble_to_tcp_queue`, `ble_to_http_queue`, `mqtt_to_ble_queue`) are allocated statically in `.bss` via `xQueueCreateStatic`.
- **Packet Sizing (`MESHTASTIC_MAX_PACKET_SIZE = 576`)**: `BridgePacket` uses a fixed `uint8_t data[576]` array (584 bytes per slot aligned), accounting for max 512-byte payload + 32-byte topic + headers. TCP frames use stack memory (`uint8_t frame[580]`), ensuring zero dynamic `malloc`/`free` calls in fast-path routing.
- **Arduino Nano ESP32 production queue sizing**:
  - `BRIDGE_QUEUE_SIZE = 4`
  - `MQTT_QUEUE_SIZE = 2`
  - Values are deliberately bounded for the ESP32-S3 production workload with
    BLE, MQTT TLS and native Tailscale active at the same time.
- **Queue Drop Telemetry & Backpressure Strategy**:
  - `tcp_to_ble_queue`: 100ms backpressure timeout; drops and increments `tcp_to_ble_dropped` if full.
  - `ble_to_tcp_queue`: 50ms backpressure timeout; drops and increments `ble_to_tcp_dropped` if full.
  - `mqtt_to_ble_queue`: 0ms non-blocking timeout; drops immediately and increments `mqtt_to_ble_dropped` (`msgs_dropped`) to prevent stalling the MQTT event loop.

### 5. Client Limits & Auto-Discovery
- **TCP Limits**: The bridge enforces a hard limit of `MAX_TCP_CLIENTS = 3`. Incoming TCP connections beyond this are instantly rejected to protect the ESP32's LwIP buffer memory and the massive 100-packet FreeRTOS queues.
- **mDNS Auto-Discovery**: The bridge runs an mDNS responder. It dynamically reads the paired BLE device's name (e.g. `DSC_ae28`), and uses a custom parser to extract the Shortname (`DSC`) and Node ID (`!ae28`), padding with `x`s if necessary. It sanitizes the full BLE name to strict RFC 1035 limits and uses it as the host (advertising as `dsc-ae28.local`). It advertises the `_meshtastic._tcp` service on port `4403`, and critically, includes the required TXT records (`name`, `shortname`, `id`) so that official Meshtastic apps can instantly discover and properly display the bridge on the local network.

## Quirks & Potential Pitfalls to Watch Out For
1. **TCP Stream Fragmentation**: `AsyncTCP` might deliver a single frame in multiple chunks, or multiple frames in a single chunk. We must implement a state machine to buffer incoming TCP bytes until a complete frame is assembled.
2. **NimBLE MTU Quirks**: Default BLE MTU is 23 bytes. To support large protobufs, we must explicitly request a larger MTU via `NimBLEDevice::setMTU(512)`.
3. **Thread Safety**: Network events trigger on Core 0, while BLE operations usually block heavily. They must be decoupled using FreeRTOS queues.
4. **NimBLE Deadlocks (FromNum)**: Calling a blocking `readValue()` inside a BLE notification callback (like `notifyFromNum`) will instantly deadlock the NimBLE background task. To prevent this, we subscribe to `FromNum` but only set a `volatile bool pendingRadioRead = true;` flag in the callback. The background task checks this flag and calls `readValue()` safely.
5. **Dynamic Queue Timeouts & Backpressure**: To read large bursts of packets instantly, our BLE task dynamically alters its wait time to 0ms when `pendingRadioRead` is true. To prevent these high-speed bursts from silently dropping packets, our FreeRTOS queues are sized massively (`100` slots) and `xQueueSend` implements a `50ms` backpressure timeout to wait for the slower TCP stream to clear.
6. **Write Without Response**: The Meshtastic `ToRadio` characteristic expects *Write Without Response*. Attempting to write with response (requesting an ACK) will fail or hang the BLE communication.
7. **TCP Stability & Fragmentation**:
   - **Contiguous Frames**: TCP headers (`0x94 0xC3` + length) and payloads must be built into a single contiguous stack array and pushed via a single `client->write()` command. Sending them via multiple `write()` calls causes TCP fragmentation, which crashes the Mac Desktop App's protobuf parser.
   - **Nagle's Algorithm**: We explicitly leave Nagle's algorithm enabled (`setNoDelay(false)`) so LwIP safely batches microscopic 6-byte and 50-byte packets into a single WiFi transmission, preventing severe WiFi congestion and TCP retransmission timeouts.
   - **Timeouts**: We configure `setRxTimeout(600)` to prevent the ESP32 from dropping clients that sit idle for up to 10 minutes.
8. **Identical Heartbeat Bypass**: The `FromRadio` mailbox does not clear on read. If the radio responds to a Mac App heartbeat with a 6-byte ACK, and a minute later responds to a new heartbeat with the *exact same* 6-byte ACK, strict byte deduplication will silently drop the second ACK, causing the Mac App to crash and disconnect. We implement a time-bypass (`2500ms`) and a `wasNotified` flag so identical but legitimate ACKs are properly forwarded.
9. **Concurrency Crashes**: LwIP networking events (`onDisconnect`) fire asynchronously on Core 0. If a client abruptly drops while the custom `bridgeNetTask` is iterating over the `tcpClients` vector to broadcast a packet, the vector pointers shift and cause an instant `Guru Meditation Error` crash. All modifications and iterations of the `tcpClients` vector must be guarded by a FreeRTOS `SemaphoreHandle_t` Mutex (`portMAX_DELAY`).
