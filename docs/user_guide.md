# User Guide

## Status LED Indicators

The bridge uses the ESP32's built-in LED to visually communicate its current state:

*   **Fast Blink** (100ms on / 100ms off): **Booting up.** The device is starting. This lasts for 5 seconds. Press and hold the BOOT button now to enter Setup Mode.
*   **Setup Pattern** (Short, Short, Long, Long): **Setup Mode / Captive Portal.** The bridge is broadcasting its `Meshtastic-Bridge-Setup` WiFi network. Connect to it to configure the bridge.
*   **Medium Blink** (500ms on / 500ms off): **Searching.** The bridge is in normal operating mode but is currently disconnected from the target Meshtastic Bluetooth radio. It is actively scanning to reconnect.
*   **Solid ON:** **Connected.** The bridge has successfully secured a Bluetooth connection with the Meshtastic radio and is actively routing packets.

## Entering Setup Mode

If you need to change your WiFi credentials or the target Bluetooth device:

1. Press the RST (Reset) button on the ESP32.
2. The LED will begin flashing rapidly.
3. Immediately press and hold the BOOT button.
4. The LED will change to the **Setup Pattern** (short-short-long-long), indicating it is now in Setup Mode.
5. Connect your phone or computer to the `Meshtastic-Bridge-Setup` WiFi network to access the Captive Portal.

## Remote access and OTA updates

### Target deployment: standalone Raspberry Pi gateway

The production gateway in Poland is a Raspberry Pi with WiFi and Bluetooth.
It runs the Linux BLE-to-TCP bridge, exposes the Meshtastic PhoneAPI on TCP
port 4403, and joins the private Tailscale network for remote access from
Germany. The Raspberry Pi connects directly to the RAK node over BLE; the
ESP32-S3 is not part of this production data path.

Keep the ESP32-S3 connected to the Raspberry Pi over USB only as an isolated
development and recovery target. Experimental OTA firmware may add future
fallback paths, but failure or reboot of the ESP32-S3 must not interrupt the
Raspberry Pi-to-RAK bridge. The Raspberry Pi should retain the ESP32 firmware
image, USB flashing tool, and serial logs so the fallback device can be
recovered remotely after its initial USB/OTA provisioning.

Use Tailscale or another authenticated VPN for TCP 4403. Meshtastic PhoneAPI
over TCP is plaintext and must never be exposed directly to the public
internet.

VPN discovery is not automatic because mDNS does not cross a routed WireGuard
tunnel. In the Meshtastic app, configure the bridge manually as a TCP device at
`192.168.1.202:4403`. Local Polish access remains `192.168.0.217:4403`.

The WireGuard status `connected` means that the ESP32 has negotiated a valid
WireGuard session key with the configured FRITZ!Box peer. It does not prove
that the FRITZ!Box forwards traffic from a second VPN client to that peer. To
separate bridge faults from VPN routing faults, first open
`http://192.168.1.202/status` from the remote VPN client. If this fails while
`http://192.168.0.217/status` and TCP port 4403 work in the Polish LAN, the
Meshtastic bridge is healthy and the missing route is upstream of it.

Never activate one exported WireGuard profile on multiple devices. The
temporary `192.168.1.202` deployment reuses the profile also used by the
Schwaebisch Gmuend FRITZ!Box client. That client and the ESP32 must not be
active simultaneously. Their handshakes and 25-second keepalives move the one
peer identity between two public endpoints, so whichever endpoint spoke most
recently becomes reachable and the other one stops receiving traffic. The
ESP32 needs its own Waiblingen FRITZ!Box peer, with a different private key and
tunnel address. Reusing the same profile on any second device creates the same
failure.

In the current deployment the iPhone instead uses an IPsec tunnel. Therefore
it cannot collide with the ESP32's WireGuard identity. If the iPhone reaches
the German home network over IPsec but cannot open
`http://192.168.1.202/status`, and the ESP32 simultaneously reports a valid
WireGuard session, the failed hop is forwarding from the FRITZ!Box IPsec VPN
participant to its WireGuard VPN participant. Use a separate WireGuard profile
for the iPhone or place a TCP relay on an always-on device in the German LAN;
changing Meshtastic or MQTT settings cannot repair this routing boundary.

Creating a dedicated FRITZ!Box WireGuard peer is a security-sensitive action
that requires an additional confirmation. A physical button is not required
when TOTP confirmation was already configured for the logged-in FRITZ!Box
user: the six-digit code from that user's authenticator app can be entered
remotely. MyFRITZ! account two-factor authentication is separate and does not
replace the FRITZ!Box user's confirmation. If no authenticator was configured,
enabling it normally requires one local confirmation first. A locally attached
analog or DECT telephone can also enter the displayed confirmation code, while
IP telephones such as FRITZ!App Fon cannot.

The Arduino Nano ESP32 build has two firmware partitions. After the one-time
USB partition migration, future firmware binaries can be installed locally or
through WireGuard with `tools/upload_ota.ps1`. The uploader reads the private,
git-ignored `.ota-token`, verifies device capacity, supplies an MD5 checksum,
and confirms the partition switch after reboot. WiFi, BLE, and WireGuard
settings are held in NVS and are not erased by OTA.

---

## Configuring the MQTT Gateway & Proxy (Optional)

To use the bridge as an autonomous 24/7 MQTT gateway for your Bluetooth radio:

### 1. Configure the Radio (via Meshtastic Mobile App)
In the Meshtastic App on your phone:
1. Go to **Radio Configuration ➔ Module Configuration ➔ MQTT**.
2. Turn **MQTT Enabled** ON.
3. Turn **Proxy to Client Enabled** (or "Proxy") ON.
4. Set the **Server Address** (e.g. `mqtt.meshtastic.org` or your private broker IP).
5. Set **Username** and **Password** if required by your broker.
6. Turn **TLS Enabled** ON if connecting securely to port 8883.
7. Under **Channels ➔ [Your Channel] ➔ Module Settings**, ensure **Uplink Enabled** and/or **Downlink Enabled** is toggled ON.

### 2. Enable on the Bridge Web UI
1. Connect to the bridge's Web UI (in Setup Mode or by navigating to `http://<bridge-name>.local`).
2. In the **MQTT Gateway** card, toggle **Enable MQTT Gateway** ON.
3. If using a private broker with a self-signed cert, check **Skip Certificate Validation** or paste your **Custom CA Root Certificate (PEM)**.
4. Click **Save MQTT Settings**.

The bridge will automatically discover the radio's MQTT configuration over Bluetooth and maintain an active broker connection.

---

## Prometheus & OpenMetrics Monitoring (`GET /metrics`)

The bridge features a built-in Prometheus exporter on port 80 at `http://<bridge-name>.local/metrics` (or `http://<ip>/metrics`). It streams metrics in standard Prometheus 0.0.4 text format with zero large heap allocations.

### Scraping with Prometheus (`prometheus.yml`)

Add a scrape job to your `prometheus.yml`:

```yaml
scrape_configs:
  - job_name: "meshtastic_bridge"
    metrics_path: "/metrics"
    scrape_interval: 15s
    scrape_timeout: 5s
    static_configs:
      - targets:
          - "192.168.1.145:80"       # By IP address
          - "mesh-af28.local:80"     # By mDNS hostname
        labels:
          role: "base-station-relay"
```

### Complete Metric Reference Table

| Metric Name | Type | Labels | Description |
| :--- | :--- | :--- | :--- |
| `meshbridge_info` | gauge | `version`, `mdns`, `board` | Identity and hardware metadata for the bridge (always `1`). |
| `meshbridge_uptime_seconds` | counter | *(none)* | Total seconds elapsed since the ESP32 booted. |
| `meshbridge_heap_free_bytes` | gauge | *(none)* | Current free internal heap memory in bytes. |
| `meshbridge_heap_min_free_bytes` | gauge | *(none)* | Lowest historical free heap memory in bytes since boot. |
| `meshbridge_heap_max_alloc_bytes` | gauge | *(none)* | Largest contiguous allocatable block in heap memory in bytes. |
| `meshbridge_task_stack_free_bytes` | gauge | `task="ble"\|"net"` | Stack high-water mark remaining for internal FreeRTOS tasks. |
| `meshbridge_wifi_connected` | gauge | *(none)* | WiFi connection state (`1` = connected to AP, `0` = disconnected). |
| `meshbridge_wifi_rssi_dbm` | gauge | *(none)* | WiFi Received Signal Strength Indicator (RSSI) in dBm. |
| `meshbridge_ble_connected` | gauge | *(none)* | Bluetooth connection state to Meshtastic radio (`1` = connected, `0` = disconnected). |
| `meshbridge_tcp_clients` | gauge | *(none)* | Current number of active TCP app clients (0 to 3). |
| `meshbridge_tcp_clients_max` | gauge | *(none)* | Maximum allowable concurrent TCP clients (`3`). |
| `meshbridge_messages_rx_total` | counter | `channel="ble"\|"tcp"\|"mqtt"` | Valid packets successfully received across the given transport boundary. |
| `meshbridge_messages_tx_total` | counter | `channel="ble"\|"tcp"\|"mqtt"` | Packets successfully transmitted across the given transport boundary. |
| `meshbridge_messages_invalid_total` | counter | `channel="ble"\|"tcp"\|"mqtt"` | Malformed, oversized, or unparseable packets rejected at the transport boundary. |
| `meshbridge_queue_messages` | gauge | `queue="tcp_to_ble"\|"ble_to_tcp"\|"mqtt_to_ble"` | Current number of queued items waiting in the inter-task buffer. |
| `meshbridge_queue_capacity` | gauge | `queue="tcp_to_ble"\|"ble_to_tcp"\|"mqtt_to_ble"` | Maximum buffer capacity of the inter-task queue (e.g. 32 or 16). |
| `meshbridge_queue_dropped_total` | counter | `queue="tcp_to_ble"\|"ble_to_tcp"\|"mqtt_to_ble"` | Packets dropped strictly due to FreeRTOS queue buffer exhaustion. |
| `meshbridge_mqtt_enabled` | gauge | *(none)* | Whether the MQTT Gateway subsystem is enabled (`1` = enabled, `0` = disabled). |
| `meshbridge_mqtt_connected` | gauge | *(none)* | Whether the MQTT client is currently connected to the broker (`1` = connected, `0` = disconnected). |

### Example Prometheus Alerting Rules

```yaml
groups:
  - name: meshtastic_bridge_alerts
    rules:
      - alert: MeshtasticRadioDisconnected
        expr: meshbridge_ble_connected == 0
        for: 2m
        labels:
          severity: critical
        annotations:
          summary: "Bridge {{ $labels.instance }} lost Bluetooth connection to radio"

      - alert: BridgeQueueDropsDetected
        expr: rate(meshbridge_queue_dropped_total[5m]) > 0
        for: 1m
        labels:
          severity: warning
        annotations:
          summary: "Bridge {{ $labels.instance }} is dropping packets due to queue congestion on {{ $labels.queue }}"

      - alert: BridgeLowHeapMemory
        expr: meshbridge_heap_free_bytes < 40000
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Bridge {{ $labels.instance }} has low heap memory ({{ $value }} bytes remaining)"
```
