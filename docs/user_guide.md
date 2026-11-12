# User Guide

## Status LED Indicators

On Arduino Nano ESP32, a red-and-blue (purple) RGB light immediately after a
USB installation is inherited from the ROM/WebSerial download mode; it is not
an application error code. Current MeshGate releases clear that color as soon
as the application starts. In the Web Flasher, wait for **Installation
complete** and select **Next** so the browser releases the serial connection;
do not unplug the board while the installation dialog is open.

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

Use Tailscale or another authenticated VPN for TCP 4403. Meshtastic PhoneAPI
over TCP is plaintext and must never be exposed directly to the public
internet.

Browser-based Meshtastic clients use the HTTP PhoneAPI instead. Choose
**HTTP**, enter the MeshGate LAN or Tailscale IP without `:4403`, and leave
HTTPS disabled. The browser client itself must be opened over HTTP for this
connection; an HTTPS page is not allowed to call an HTTP device because of
browser mixed-content rules. CORS and Private Network Access preflights are
handled by MeshGate. The Tailscale tunnel still encrypts remote traffic.

### Offline message mailbox

RAK WisBlock radios do not retain ordinary received chat messages for a
disconnected PhoneAPI client. MeshGate therefore provides automatic,
gateway-local store-and-forward behavior without requiring the separate
Meshtastic Store & Forward module or any extra radio configuration.

MeshGate stores the newest 128 decoded Meshtastic text messages even when no
phone or computer is connected. This includes received messages and messages
you sent through MeshGate that the radio successfully accepted over BLE.
Reconnect the native Meshtastic app to `<meshgate-ip>:4403`, or reconnect a
browser Meshtastic Client over HTTP. After the normal radio configuration
finishes, MeshGate automatically replays both sides of the retained
conversation through the standard PhoneAPI. Sent history is replayed only to
the client and is never transmitted over LoRa a second time. Existing inner
packet IDs are preserved so the client can ignore messages already present
locally.

For another integration, request
`http://<meshgate-ip>/api/mailbox?limit=32`. Poll incrementally with the last
returned sequence, for example `/api/mailbox?after=123&limit=32`. Each message
contains `direction: sent` or `direction: received`; the response also reports
capacity, oldest/next sequence, overwritten, duplicate and invalid counters.
The mailbox survives reboot and OTA. **Reset All Settings** clears it
along with configuration. Retained text is readable to anyone who can access
the private MeshGate Web API, so do not publish that API to the internet.

### Native Tailscale on the IDF 5 Nano build

The `arduino_nano_esp32_idf5` image can join a tailnet directly through
MicroLink. No public IP, dynamic DNS, inbound firewall rule or additional Linux
computer is required.

The Web UI contains a three-step keyless wizard:

1. Install Tailscale on the phone or computer that will access MeshGate and
   sign in to the intended tailnet. The wizard contains an offline QR code and
   direct iOS, Android, Windows and macOS installation buttons.
2. Choose a lowercase device name and select **Add This Device to Tailscale**.
   MeshGate saves, reboots, reconnects the still-open page and opens the
   one-time Tailscale browser approval automatically. No auth/API key, admin
   console or command line is required.
3. Sign in and approve MeshGate. The original page checks authorization and
   changes to **Your device has been added**, then prominently displays
   `<tailscale-ip>:4403` for the Meshtastic app and `<tailscale-ip>` without a
   port for the browser-based Meshtastic Client.

The connected **Tailscale (Remote VPN)** card displays exactly two connection
values: **Meshtastic App** uses `<tailscale-ip>:4403`; **Meshtastic Client**
uses only `<tailscale-ip>` with HTTP selected and HTTPS disabled. The lower
status list deliberately does not repeat the IP. **Open MeshGate** opens the
control page without printing a third address. On the remote phone or computer,
Tailscale must be active and signed into the same tailnet.

mDNS is link-local and normally does not cross a VPN. Manual IP entry is
therefore expected. A machine appearing in the Tailscale console proves only
registration; successful HTTP or TCP 4403 traffic proves the data plane.

The login URL is temporary and is never stored. The joined machine continues
using its persistent MicroLink identity after reboots and OTA updates. For a
trusted unattended gateway, review and disable device key expiry in the
Tailscale Machines page after enrollment.

### Classic WireGuard

Each MeshGate requires its own WireGuard peer identity, private key and tunnel
address. Never activate one exported profile on multiple devices: the peer's
endpoint will move to whichever device sent the most recent handshake and the
other device will stop receiving traffic.

A WireGuard status of `connected` proves the ESP32 negotiated a session key;
it does not prove that a remote router forwards traffic between two different
VPN clients. Test the MeshGate `/status` endpoint before diagnosing the
Meshtastic protocol.

Tailscale and classic WireGuard are mutually exclusive in the IDF 5 image.

### One-click OTA

The Arduino Nano ESP32 profile uses two application partitions. After the
one-time USB partition-table migration, later firmware can be installed over
the LAN or tailnet with `tools/upload_ota.ps1`. The uploader reads the private,
git-ignored `.ota-token`, verifies image capacity, supplies an MD5 checksum and
confirms the partition switch after reboot. Wi-Fi, BLE, VPN and MQTT settings
are stored in NVS and are not erased by an ordinary OTA update.

The normal update is entirely automatic:

1. Open the MeshGate control page through its LAN or Tailscale IP.
2. In **Firmware OTA Update**, select **Check & Install Latest Release** and
   confirm.
3. MeshGate fetches the official release metadata and firmware from the
   project's GitHub Pages release channel, validates TLS, board identity, image
   size and SHA-256, writes the inactive slot and reboots.

No local file or key is requested. The authenticated raw endpoint used by
`tools/upload_ota.ps1` remains available for developers and recovery, but is
not shown in the normal Web UI.

The one-click OTA path writes only the inactive application partition and does
not erase NVS. Wi-Fi credentials, BLE pairing data, Tailscale/WireGuard keys,
MQTT configuration and the OTA token therefore survive. The public USB Web
Flasher is a factory-installation/recovery tool instead: selecting **Erase
device** there deliberately removes all NVS data and cannot preserve secrets.

---

## Configuring the MQTT Gateway & Proxy (Optional)

To use the bridge as an autonomous 24/7 MQTT gateway for your Bluetooth radio:

MeshGate starts with MeshRadar's public preset: broker `meshradar.pl`, port
`8883`, username and password `meshradar`, TLS enabled, and root topic `msh`.
These values are published by the service. A complete custom broker
configuration stored on the radio overrides the preset.

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
