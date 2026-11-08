#include "config_ui.h"
#include "ble_client.h"
#include "wifi_net.h"
#include "mqtt_net.h"
#include "bridge.h"
#include <WiFi.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include "build_options.h"
#include "diag_telemetry.h"
#include "utils.h"
#include "wireguard_net.h"
#include "tailscale_net.h"
#include "web_log.h"
#include "ota_update.h"
#include "esp_log.h"

/**
 * For retrieving configuration properties stored in NVS.
 */
Preferences preferences;
Preferences backupPreferences;

/**
 * Webserver for Captive Portal/Setup UI.
 */
AsyncWebServer server(80);

/**
 * Defines the namespace which preferences are stored to NVS under.
 */
const char* PREF_NAMESPACE = "bridge_cfg";
const char* BACKUP_PREF_NAMESPACE = "bridge_backup";

static void backupCurrentConfiguration();

static bool isValidOtaToken(const String& token) {
    if (token.length() != 64) {
        return false;
    }
    for (size_t i = 0; i < token.length(); ++i) {
        const char c = token[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static void ensureOtaToken() {
    String token = preferences.getString("ota_token", "");
    if (!isValidOtaToken(token)) {
        token = backupPreferences.getString("ota_token", "");
    }

#ifdef OTA_BOOTSTRAP_TOKEN
    if (!isValidOtaToken(token)) {
        token = OTA_BOOTSTRAP_TOKEN;
        log_i("[Config] Installed OTA authentication token from protected build input");
    }
#endif

    if (!isValidOtaToken(token)) {
        log_w("[Config] OTA disabled because no valid authentication token is configured");
        return;
    }

    preferences.putString("ota_token", token);
    backupPreferences.putString("ota_token", token);
}

static void restartTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP.restart();
    vTaskDelete(nullptr);
}

static bool scheduleRestart() {
    return xTaskCreate(restartTask, "api-restart", 2048, nullptr, 1, nullptr) == pdPASS;
}

static void writeStoredConfiguration(Preferences& target, const BridgeConfig& cfg) {
    target.putString("wifi_ssid", cfg.wifi_ssid);
    target.putString("wifi_pass", cfg.wifi_pass);
    target.putString("ble_name", cfg.ble_name);
    target.putString("ble_mac", cfg.ble_mac);
    target.putString("ble_pin", cfg.ble_pin);
    target.putBool("debug_logs", cfg.debug_logs);
    target.putBool("wg_enabled", cfg.wireguard_enabled);
    target.putString("wg_local_ip", cfg.wireguard_local_ip);
    target.putString("wg_remote", cfg.wireguard_remote_cidr);
    target.putString("wg_endpoint", cfg.wireguard_endpoint);
    target.putUShort("wg_ep_port", cfg.wireguard_endpoint_port);
    target.putString("wg_priv_key", cfg.wireguard_private_key);
    target.putString("wg_pub_key", cfg.wireguard_public_key);
    target.putString("wg_psk", cfg.wireguard_preshared_key);
    target.putBool("ts_enabled", cfg.tailscale_enabled);
    target.putString("ts_name", cfg.tailscale_device_name);
    target.putString("ts_auth", cfg.tailscale_auth_key);
    target.putBool("mqtt_enabled", cfg.mqtt_enabled);
    target.putBool("mqtt_tls_insec", cfg.mqtt_tls_insecure);
    target.putString("mqtt_custom_ca", cfg.mqtt_custom_ca);
}

/**
 * The HTML for the modular captive portal
 */
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Meshtastic Bridge Setup</title>
<style>
  * { box-sizing: border-box; }
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif; padding: 16px; max-width: 480px; margin: auto; background-color: #f0f2f5; color: #1c1e21; }
  h2 { color: #1a1a1a; text-align: center; margin-bottom: 20px; font-size: 22px; }
  .card { background: white; padding: 18px; border-radius: 12px; box-shadow: 0 2px 8px rgba(0,0,0,0.08); margin-bottom: 16px; }
  .card-header { display: flex; justify-content: space-between; align-items: center; border-bottom: 1px solid #eee; padding-bottom: 10px; margin-bottom: 14px; }
  .card-title { font-weight: bold; font-size: 16px; display: flex; align-items: center; gap: 8px; }
  .info-row { display: flex; justify-content: space-between; margin: 8px 0; font-size: 14px; }
  .info-label { color: #65676b; font-weight: 500; }
  .info-val { font-family: monospace; font-weight: 600; color: #050505; word-break: break-all; }
  label { font-weight: 600; margin-top: 12px; display: block; color: #444; font-size: 13px; }
  input, select { width: 100%; padding: 10px; margin: 6px 0 12px; border: 1px solid #ccd0d5; border-radius: 6px; font-size: 14px; background: #fff; }
  button { width: 100%; padding: 10px; margin: 6px 0; border: none; border-radius: 6px; cursor: pointer; font-size: 14px; font-weight: 600; transition: background-color 0.2s, opacity 0.2s; }
  button:hover { opacity: 0.92; }
  button:disabled { opacity: 0.55 !important; cursor: not-allowed; }
  .btn-primary { background-color: #0066cc; color: white; }
  .btn-success { background-color: #28a745; color: white; }
  .btn-secondary { background-color: #e4e6eb; color: #1c1e21; }
  .btn-warning { background-color: #ff9800; color: white; }
  .btn-danger { background-color: #dc3545; color: white; margin-top: 8px; }
  .btn-danger:hover { background-color: #c82333; }
  .btn-edit { width: auto; padding: 6px 16px; font-size: 13px; margin: 0; background-color: #0066cc; color: white; border-radius: 6px; font-weight: 600; }
  .btn-edit:hover { background-color: #0052a3; opacity: 1; }
  .btn-group { display: flex; gap: 8px; margin-top: 8px; }
  .btn-group button { flex: 1; }
  .password-field { position: relative; display: flex; align-items: center; margin-bottom: 12px; }
  .password-field input { padding-right: 65px; margin: 0; }
  .btn-toggle-pass { position: absolute; right: 5px; width: auto; padding: 6px 10px; font-size: 12px; margin: 0; background: #e4e6eb; color: #333; border-radius: 4px; }
  .spinner { display: none; margin: 10px 0; color: #0066cc; font-style: italic; font-size: 13px; text-align: center; }
  .alert { display: none; padding: 10px; border-radius: 6px; font-size: 13px; margin: 10px 0; }
  .alert-success { background-color: #d4edda; color: #155724; border: 1px solid #c3e6cb; }
  .alert-error { background-color: #f8d7da; color: #721c24; border: 1px solid #f5c6cb; }
  .alert-warning { background-color: #fff3cd; color: #856404; border: 1px solid #ffeeba; }
  .alert-info { background-color: #d1ecf1; color: #0c5460; border: 1px solid #bee5eb; }
  .status-pill { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 12px; font-weight: 600; }
  .status-pill.disabled { background-color: #e4e6eb; color: #65676b; }
  .status-pill.connected { background-color: #d4edda; color: #155724; }
  .status-pill.waiting { background-color: #fff3cd; color: #856404; }
  .status-pill.connecting { background-color: #d1ecf1; color: #0c5460; }
  .status-pill.error { background-color: #f8d7da; color: #721c24; }
  .progress-track { width: 100%; background: #e4e6eb; border-radius: 4px; height: 8px; overflow: hidden; margin-top: 4px; }
  .progress-fill { height: 100%; border-radius: 4px; transition: width 0.3s ease, background-color 0.3s ease; }
  .progress-green { background-color: #28a745; }
  .progress-yellow { background-color: #ff9800; }
  .progress-red { background-color: #dc3545; }
  .queue-box { margin: 10px 0; }
  .queue-header { display: flex; justify-content: space-between; font-size: 13px; font-weight: 500; color: #444; }
  .queue-stats { font-family: monospace; font-size: 12px; color: #050505; }
  .section-divider { border-top: 1px solid #eee; margin: 14px 0 10px; padding-top: 10px; font-weight: bold; font-size: 14px; color: #1c1e21; }
  .footer { text-align: center; font-size: 12px; color: #65676b; margin-top: 24px; padding-bottom: 20px; }
  .footer a { color: #0066cc; text-decoration: none; font-weight: 600; }
  .footer a:hover { text-decoration: underline; }
</style>
</head>
<body>
  <h2>Meshtastic Bridge</h2>

  <!-- WiFi Configuration Card -->
  <div class="card" id="card-wifi">
    <div class="card-header">
      <div class="card-title">WiFi Network</div>
      <button class="btn-edit" id="btn-edit-wifi" onclick="toggleEditWifi(true)">Edit</button>
    </div>
    <div id="wifi-view">
      <div class="info-row">
        <span class="info-label">SSID:</span>
        <span class="info-val" id="view-wifi-ssid">Loading...</span>
      </div>
      <div class="info-row">
        <span class="info-label">Password:</span>
        <span class="info-val" id="view-wifi-pass">••••••••</span>
      </div>
      <div class="info-row">
        <span class="info-label">Connection:</span>
        <span class="info-val"><span id="view-wifi-status-pill" class="status-pill waiting">Unknown</span></span>
      </div>
      <div class="info-row">
        <span class="info-label">IP Address:</span>
        <span class="info-val" id="view-wifi-ip">Loading...</span>
      </div>
      <div class="info-row">
        <span class="info-label">mDNS Host:</span>
        <span class="info-val" id="view-wifi-mdns">Loading...</span>
      </div>
      <div class="info-row">
        <span class="info-label">Signal (RSSI):</span>
        <span class="info-val" id="view-wifi-rssi">Loading...</span>
      </div>
      <div class="info-row">
        <span class="info-label">TCP Clients:</span>
        <span class="info-val" id="view-sys-tcp-clients">Loading...</span>
      </div>
      <div class="info-row">
        <span class="info-label">Traffic:</span>
        <span class="info-val">▲ <span id="view-tcp-tx">0</span> sent / ▼ <span id="view-tcp-rx">0</span> rcvd · <span id="view-tcp-inv">0</span> invalid</span>
      </div>
    </div>
    <div id="wifi-edit" style="display:none;">
      <label>Discovered Networks:</label>
      <div id="wifi-scan-status" class="spinner">Scanning for WiFi networks...</div>
      <select id="select-wifi-ssid" onchange="onWifiNetworkSelected()">
        <option value="">-- Select or scan below --</option>
      </select>
      <button type="button" id="btn-scan-wifi" class="btn-secondary" onclick="scanWifi()">🔍 Scan for Networks</button>

      <label>WiFi SSID:</label>
      <input type="text" id="input-wifi-ssid" placeholder="Network Name">
      <label>WiFi Password:</label>
      <div class="password-field">
        <input type="password" id="input-wifi-pass" placeholder="Leave empty to keep existing">
        <button type="button" class="btn-toggle-pass" onclick="togglePassVisibility('input-wifi-pass', this)">Show</button>
      </div>
      <div id="wifi-alert" class="alert"></div>
      <div class="btn-group">
        <button class="btn-success" id="btn-save-wifi" onclick="saveWifi()">Save WiFi</button>
        <button class="btn-secondary" onclick="toggleEditWifi(false)">Cancel</button>
      </div>
    </div>
  </div>

  <!-- Bluetooth Meshtastic Radio Card -->
  <div class="card" id="card-ble">
    <div class="card-header">
      <div class="card-title">Bluetooth Meshtastic Device</div>
      <button class="btn-edit" id="btn-edit-ble" onclick="toggleEditBle(true)">Edit</button>
    </div>
    <div id="ble-view">
      <div class="info-row">
        <span class="info-label">Device:</span>
        <span class="info-val" id="view-ble-device">Loading...</span>
      </div>
      <div class="info-row">
        <span class="info-label">BLE PIN:</span>
        <span class="info-val" id="view-ble-pin">••••••</span>
      </div>
      <div class="info-row">
        <span class="info-label">Connection:</span>
        <span class="info-val"><span id="view-ble-status-pill" class="status-pill waiting">Unknown</span></span>
      </div>
      <div class="info-row">
        <span class="info-label">Traffic:</span>
        <span class="info-val">▲ <span id="view-ble-tx">0</span> sent / ▼ <span id="view-ble-rx">0</span> rcvd · <span id="view-ble-inv">0</span> invalid</span>
      </div>
    </div>
    <div id="ble-edit" style="display:none;">
      <label>Discovered Devices:</label>
      <div id="ble-scan-alert" class="alert"></div>
      <div id="scan-status" class="spinner">Scanning for Bluetooth devices (4 seconds)...</div>
      <select id="select-ble-device" onchange="onBleDeviceSelected()">
        <option value="">-- Select or scan below --</option>
      </select>
      <button type="button" id="btn-scan" class="btn-secondary" onclick="scanBle()">🔍 Scan for Devices</button>

      <input type="hidden" id="input-ble-name" value="">

      <label>Target MAC Address:</label>
      <input type="text" id="input-ble-mac" placeholder="AA:BB:CC:DD:EE:FF">

      <label>BLE PIN (6-digit):</label>
      <input type="number" id="input-ble-pin" min="0" max="999999" placeholder="123456">

      <div id="ble-edit-alert" class="alert"></div>

      <button type="button" id="btn-test-ble" class="btn-warning" onclick="testBleConnection()">⚡ Test Connection</button>
      <div class="btn-group">
        <button class="btn-success" id="btn-save-ble" onclick="saveBle()">Save Bluetooth</button>
        <button class="btn-secondary" onclick="toggleEditBle(false)">Cancel</button>
      </div>
    </div>
  </div>

  <!-- Native Tailscale VPN Card -->
  <div class="card" id="card-tailscale">
    <div class="card-header">
      <div class="card-title">Tailscale Remote Access</div>
      <button class="btn-edit" id="btn-edit-tailscale" onclick="toggleEditTailscale(true)">Edit</button>
    </div>
    <div id="tailscale-view">
      <div class="info-row">
        <span class="info-label">Tailnet:</span>
        <span class="info-val"><span id="view-tailscale-status" class="status-pill disabled">Disabled</span></span>
      </div>
      <div class="info-row"><span class="info-label">Device name:</span><span class="info-val" id="view-tailscale-name">meshgate-torun</span></div>
      <div class="info-row"><span class="info-label">Tailscale IP:</span><span class="info-val" id="view-tailscale-ip">(Not connected)</span></div>
      <div class="info-row"><span class="info-label">Known peers:</span><span class="info-val" id="view-tailscale-peers">0</span></div>
      <div class="info-row"><span class="info-label">Auth key:</span><span class="info-val" id="view-tailscale-auth">(Not set)</span></div>
    </div>
    <div id="tailscale-edit" style="display:none;">
      <div class="alert alert-info" style="display:block; margin-bottom:12px;">
        Native Tailscale creates an outbound connection, so no dynamic DNS or router port forwarding is required. Once connected, this page and Meshtastic TCP port 4403 are reachable through the displayed 100.x address. Tailscale and classic WireGuard are mutually exclusive.
      </div>
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
        <label for="input-tailscale-enabled" style="margin:0;">Enable Tailscale</label>
        <input type="checkbox" id="input-tailscale-enabled" style="width:20px; height:20px; margin:0;">
      </div>
      <label>Tailnet device name:</label>
      <input type="text" id="input-tailscale-name" maxlength="63" placeholder="meshgate-torun">
      <label>Tailscale auth key:</label>
      <div class="password-field">
        <input type="password" id="input-tailscale-auth" maxlength="128" placeholder="Leave empty to keep stored key">
        <button type="button" class="btn-toggle-pass" onclick="togglePassVisibility('input-tailscale-auth', this)">Show</button>
      </div>
      <div style="display:flex; align-items:center; gap:8px; margin:8px 0 12px;">
        <input type="checkbox" id="input-tailscale-clear-auth" style="width:18px; height:18px; margin:0;">
        <label for="input-tailscale-clear-auth" style="margin:0;">Remove stored auth key</label>
      </div>
      <div id="tailscale-alert" class="alert"></div>
      <div class="btn-group">
        <button class="btn-success" id="btn-save-tailscale" onclick="saveTailscale()">Save Tailscale</button>
        <button class="btn-secondary" onclick="toggleEditTailscale(false)">Cancel</button>
      </div>
    </div>
  </div>

  <!-- WireGuard VPN Card -->
  <div class="card" id="card-wireguard">
    <div class="card-header">
      <div class="card-title">WireGuard VPN</div>
      <button class="btn-edit" id="btn-edit-wireguard" onclick="toggleEditWireguard(true)">Edit</button>
    </div>
    <div id="wireguard-view">
      <div class="info-row">
        <span class="info-label">Tunnel:</span>
        <span class="info-val"><span id="view-wireguard-status" class="status-pill disabled">Disabled</span></span>
      </div>
      <div class="info-row"><span class="info-label">Tunnel IP:</span><span class="info-val" id="view-wireguard-ip">(Not configured)</span></div>
      <div class="info-row"><span class="info-label">German LAN:</span><span class="info-val" id="view-wireguard-remote">(Not configured)</span></div>
      <div class="info-row"><span class="info-label">Endpoint:</span><span class="info-val" id="view-wireguard-endpoint">(Not configured)</span></div>
      <div class="info-row"><span class="info-label">Private key:</span><span class="info-val" id="view-wireguard-private">(Not set)</span></div>
      <div class="info-row"><span class="info-label">Preshared key:</span><span class="info-val" id="view-wireguard-psk">(Not set)</span></div>
    </div>
    <div id="wireguard-edit" style="display:none;">
      <div class="alert alert-info" style="display:block; margin-bottom:12px;">
        The ESP32 opens the VPN tunnel itself. TCP port 4403 and this setup page are then reachable at the tunnel IP. Secret keys are write-only and never returned by the web API.
      </div>
      <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
        <label for="input-wireguard-enabled" style="margin:0;">Enable WireGuard</label>
        <input type="checkbox" id="input-wireguard-enabled" style="width:20px; height:20px; margin:0;">
      </div>
      <label>Tunnel IP:</label>
      <input type="text" id="input-wireguard-local-ip" placeholder="192.168.1.254">
      <label>Remote LAN (CIDR):</label>
      <input type="text" id="input-wireguard-remote-cidr" placeholder="192.168.1.0/24">
      <label>FRITZ!Box endpoint (host or public IP):</label>
      <input type="text" id="input-wireguard-endpoint" placeholder="example.myfritz.net">
      <label>Endpoint UDP port:</label>
      <input type="number" id="input-wireguard-endpoint-port" min="1" max="65535" value="51820">
      <label>ESP32 private key:</label>
      <div class="password-field">
        <input type="password" id="input-wireguard-private-key" maxlength="44" placeholder="Leave empty to keep stored key">
        <button type="button" class="btn-toggle-pass" onclick="togglePassVisibility('input-wireguard-private-key', this)">Show</button>
      </div>
      <label>FRITZ!Box public key:</label>
      <input type="text" id="input-wireguard-public-key" maxlength="44" placeholder="Base64 public key">
      <label>Preshared key (optional):</label>
      <div class="password-field">
        <input type="password" id="input-wireguard-preshared-key" maxlength="44" placeholder="Leave empty to keep stored key">
        <button type="button" class="btn-toggle-pass" onclick="togglePassVisibility('input-wireguard-preshared-key', this)">Show</button>
      </div>
      <div style="display:flex; align-items:center; gap:8px; margin:8px 0 12px;">
        <input type="checkbox" id="input-wireguard-clear-psk" style="width:18px; height:18px; margin:0;">
        <label for="input-wireguard-clear-psk" style="margin:0;">Remove stored preshared key</label>
      </div>
      <div id="wireguard-alert" class="alert"></div>
      <div class="btn-group">
        <button class="btn-success" id="btn-save-wireguard" onclick="saveWireguard()">Save WireGuard</button>
        <button class="btn-secondary" onclick="toggleEditWireguard(false)">Cancel</button>
      </div>
    </div>
  </div>

  <!-- MQTT Gateway Card -->
  <div class="card" id="card-mqtt">
    <div class="card-header">
      <div class="card-title">MQTT Gateway</div>
      <button class="btn-edit" id="btn-edit-mqtt" onclick="toggleEditMqtt(true)">Edit</button>
    </div>
    <div id="mqtt-view">
      <!-- Global Feature Enabled/Disabled -->
      <div class="info-row">
        <span class="info-label">Gateway Feature:</span>
        <span class="info-val"><span id="view-mqtt-feature-enabled-pill" class="status-pill waiting">Unknown: Loading...</span></span>
      </div>

      <!-- If Enabled, status of the client, otherwise hidden -->
      <div id="mqtt-gateway-status-container" style="display: none;">
        <div class="info-row">
          <span class="info-label">Client Status:</span>
          <span class="info-val"><span id="view-mqtt-status-pill" class="status-pill disabled">Disabled</span></span>
        </div>
        <div class="info-row">
          <span class="info-label">Radio Proxy:</span>
          <span class="info-val" id="view-mqtt-radio-proxy">(Unknown)</span>
        </div>
        <div class="info-row">
          <span class="info-label">Detected Broker:</span>
          <span class="info-val" id="view-mqtt-broker">(Waiting for radio)</span>
        </div>
        <div class="info-row">
          <span class="info-label">Root Topic:</span>
          <span class="info-val" id="view-mqtt-root">msh</span>
        </div>
        <div class="info-row">
          <span class="info-label">Traffic:</span>
          <span class="info-val">▲ <span id="view-mqtt-tx">0</span> sent / ▼ <span id="view-mqtt-rx">0</span> rcvd · <span id="view-mqtt-inv">0</span> invalid</span>
        </div>
      </div>
    </div>
    <div id="mqtt-edit" style="display:none;">
      <div class="alert alert-info" style="display:block; margin-bottom: 12px;">
        The bridge automatically queries your radio for its MQTT settings (hostname, credentials, and root topic) and connects
        to that server.<br/>
        <br/>
        <strong>NOTE</strong>: Make sure your radio has both <strong>MQTT Enabled</strong> and <strong>MQTT Proxy Enabled</strong> configured.
      </div>

      <div style="margin: 10px 0 6px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom: 12px;">
          <label for="input-mqtt-enabled" style="margin:0; font-size:14px; font-weight:600;">Enable MQTT Gateway</label>
          <input type="checkbox" id="input-mqtt-enabled" style="width:20px; height:20px; margin:0;">
        </div>
        <small style="color:#65676b; display:block; margin-bottom:10px;">
          Enable/Disable this feature.
        </small>
      </div>

      <div style="margin: 10px 0 6px;">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom: 6px;">
          <label for="input-mqtt-tls-insecure" style="margin:0; font-size:13px;">Skip Certificate Validation</label>
          <input type="checkbox" id="input-mqtt-tls-insecure" style="width:20px; height:20px; margin:0;">
        </div>
        <small style="color:#65676b; display:block; margin-bottom:10px;">Allow connections to brokers using self-signed certificates, IP addresses, or invalid SSL certificates.</small>
      </div>

      <div style="margin: 10px 0;">
        <label style="margin-bottom:4px;">Server Certificate (Optional):</label>
        <textarea id="input-mqtt-ca" rows="4" maxlength="3500" style="width:100%; padding:8px; border:1px solid #ccd0d5; border-radius:6px; font-family:monospace; font-size:11px; resize:vertical;" placeholder="-----BEGIN CERTIFICATE-----&#10;&#10;-----END CERTIFICATE-----"></textarea>
        <div style="display:flex; gap:8px; margin-top:4px;">
          <input type="file" id="file-mqtt-ca" accept=".pem,.crt,.cer" style="display:none;" onchange="loadCaFile(this)">
          <button type="button" class="btn-secondary" style="flex:1; padding:6px; font-size:12px; margin:0;" onclick="document.getElementById('file-mqtt-ca').click()">📂 Load Cert File</button>
          <button type="button" class="btn-secondary" style="flex:1; padding:6px; font-size:12px; margin:0;" onclick="document.getElementById('input-mqtt-ca').value=''">🗑 Clear</button>
        </div>
        <small style="color:#65676b; display:block; margin-top:4px;">
          Paste the public certificate or CA certificate for servers using self-signed SSL certificates.
          This allows the bridge to validate the server's identity even when it's not signed by a trusted certificate authority.<br/><br/>
          Leave blank to use built-in Mozilla Root CAs (Let's Encrypt, DigiCert, etc.)
        </small>
      </div>

      <div id="mqtt-alert" class="alert"></div>
      <div class="btn-group">
        <button class="btn-success" id="btn-save-mqtt" onclick="saveMqtt()">Save MQTT</button>
        <button class="btn-secondary" onclick="toggleEditMqtt(false)">Cancel</button>
      </div>
    </div>
  </div>

  <!-- System Actions Card -->
  <div class="card">
    <div class="card-header">
      <div class="card-title">System</div>
    </div>
    <div id="system-alert" class="alert"></div>

    <div class="info-row">
      <span class="info-label">Uptime:</span>
      <span class="info-val" id="view-sys-uptime">Loading...</span>
    </div>
    <div class="info-row">
      <span class="info-label">Free Heap:</span>
      <span class="info-val" id="view-sys-heap">Loading...</span>
    </div>
    <div class="info-row">
      <span class="info-label">Free Task Stacks:</span>
      <span class="info-val" id="view-sys-stacks">Loading...</span>
    </div>

    <div class="section-divider">Queue Buffers</div>
    <div class="queue-box">
      <div class="queue-header">
        <span>TCP ➔ BLE Queue:</span>
        <span class="queue-stats" id="view-queue-t2b-text">0 / 32 (0%) · 0 drops</span>
      </div>
      <div class="progress-track"><div id="view-queue-t2b-bar" class="progress-fill progress-green" style="width:0%;"></div></div>
    </div>

    <div class="queue-box">
      <div class="queue-header">
        <span>BLE ➔ TCP Queue:</span>
        <span class="queue-stats" id="view-queue-b2t-text">0 / 32 (0%) · 0 drops</span>
      </div>
      <div class="progress-track"><div id="view-queue-b2t-bar" class="progress-fill progress-green" style="width:0%;"></div></div>
    </div>

    <div class="queue-box">
      <div class="queue-header">
        <span>MQTT ➔ BLE Queue:</span>
        <span class="queue-stats" id="view-queue-m2b-text">0 / 16 (0%) · 0 drops</span>
      </div>
      <div class="progress-track"><div id="view-queue-m2b-bar" class="progress-fill progress-green" style="width:0%;"></div></div>
    </div>

    <div class="section-divider">Settings & Controls</div>
    <div class="input-group" style="display:flex; justify-content:space-between; align-items:center; margin: 10px 0 12px;">
      <label for="sys-live-polling" style="margin-bottom:0;">Live Telemetry Polling (<span id="sys-poll-rate">7s</span>)</label>
      <input type="checkbox" id="sys-live-polling" checked onchange="toggleLivePolling()" style="width:20px; height:20px; margin:0;">
    </div>
    <div class="input-group" style="display:flex; justify-content:space-between; align-items:center; margin-bottom: 20px;">
      <label for="sys-debug-logs" style="margin-bottom:0;">Enable Serial Debug Logs</label>
      <input type="checkbox" id="sys-debug-logs" onchange="saveSystemSettings()" style="width:20px; height:20px; margin:0;">
    </div>

    <button class="btn-primary" id="btn-reboot" onclick="rebootBridge()">Reboot & Start Bridge</button>
    <button class="btn-danger" id="btn-reset" onclick="resetBridge()">Reset All Settings</button>
  </div>

  <div class="footer">
    Firmware Version: )rawliteral" FIRMWARE_VERSION R"rawliteral(<br>
    <a href=")rawliteral" PROJECT_GITHUB_URL R"rawliteral(" target="_blank">View Project on GitHub</a>
  </div>

  <!-- Custom In-DOM Modal for Captive Portal compatibility (macOS/iOS CNA) -->
  <div id="modal-overlay" style="display:none; position:fixed; top:0; left:0; right:0; bottom:0; background:rgba(0,0,0,0.55); z-index:9999; align-items:center; justify-content:center; padding:16px;">
    <div style="background:white; padding:22px; border-radius:12px; max-width:380px; width:100%; box-shadow:0 8px 24px rgba(0,0,0,0.25); text-align:center;">
      <h3 id="modal-title" style="margin-top:0; font-size:18px; color:#1a1a1a;">Confirm Action</h3>
      <p id="modal-msg" style="font-size:14px; color:#444; margin:14px 0 20px; line-height:1.45;"></p>
      <div class="btn-group">
        <button id="modal-btn-confirm" class="btn-primary" style="margin:0;">Confirm</button>
        <button class="btn-secondary" style="margin:0;" onclick="closeModal()">Cancel</button>
      </div>
    </div>
  </div>

  <script>
    let currentConfig = { wifi_ssid: '', wifi_has_pass: false, ble_mac: '', ble_pin: '' };
    let pollInterval = null;
    let wifiPollInterval = null;

    function showAlert(id, type, msg) {
      const el = document.getElementById(id);
      el.className = 'alert alert-' + type;
      el.innerHTML = msg;
      el.style.display = 'block';
    }

    function hideAlert(id) {
      document.getElementById(id).style.display = 'none';
    }

    function togglePassVisibility(inputId, btn) {
      const input = document.getElementById(inputId);
      if (input.type === 'password') {
        input.type = 'text';
        btn.innerText = 'Hide';
      } else {
        input.type = 'password';
        btn.innerText = 'Show';
      }
    }

    function formatUptime(sec) {
      if (!sec && sec !== 0) return '(Unknown)';
      const d = Math.floor(sec / 86400);
      const h = Math.floor((sec % 86400) / 3600);
      const m = Math.floor((sec % 3600) / 60);
      const s = sec % 60;
      if (d > 0) return d + 'd ' + h + 'h ' + m + 'm ' + s + 's';
      if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
      if (m > 0) return m + 'm ' + s + 's';
      return s + 's';
    }

    function renderProgressBar(barId, textId, waiting, capacity, dropped) {
      const cap = capacity || 1;
      const pct = Math.min(100, Math.max(0, Math.round((waiting / cap) * 100)));
      const barEl = document.getElementById(barId);
      const textEl = document.getElementById(textId);
      if (textEl) {
        textEl.innerText = waiting + ' / ' + cap + ' (' + pct + '%) · ' + (dropped || 0) + ' drops';
      }
      if (barEl) {
        barEl.style.width = pct + '%';
        let colorClass = 'progress-fill progress-green';
        if (pct >= 80) colorClass = 'progress-fill progress-red';
        else if (pct >= 50) colorClass = 'progress-fill progress-yellow';
        barEl.className = colorClass;
      }
    }

    const pollIntervals = [7000, 3000, 1000];
    let pollIntervalIdx = 0;
    let statusPollTimer = null;
    let statusFetchInFlight = false;

    function scheduleNextStatusPoll() {
      if (statusPollTimer) {
        clearTimeout(statusPollTimer);
        statusPollTimer = null;
      }
      const enabled = document.getElementById('sys-live-polling').checked;
      if (enabled) {
        statusPollTimer = setTimeout(updateStatus, pollIntervals[pollIntervalIdx]);
      }
    }

    function toggleLivePolling() {
      if (statusPollTimer) {
        clearTimeout(statusPollTimer);
        statusPollTimer = null;
      }
      const enabled = document.getElementById('sys-live-polling').checked;
      if (enabled) {
        updateStatus();
      }
    }

    function loadConfig() {
      fetch('/config')
        .then(r => r.json())
        .then(cfg => {
          currentConfig = cfg;
          document.getElementById('view-wifi-ssid').innerText = cfg.wifi_ssid || '(Not configured)';
          document.getElementById('view-wifi-pass').innerText = cfg.wifi_has_pass ? '••••••••' : '(None)';

          let deviceDisplay = '(Not configured)';
          if (cfg.ble_mac) {
            deviceDisplay = cfg.ble_name ? cfg.ble_name + ' (' + cfg.ble_mac + ')' : '(' + cfg.ble_mac + ')';
          }
          document.getElementById('view-ble-device').innerText = deviceDisplay;
          document.getElementById('view-ble-pin').innerText = cfg.ble_pin ? '••••••' : '(Not set)';

          document.getElementById('input-wifi-ssid').value = cfg.wifi_ssid || '';
          document.getElementById('input-ble-name').value = cfg.ble_name || '';
          document.getElementById('input-ble-mac').value = cfg.ble_mac || '';
          document.getElementById('input-ble-pin').value = cfg.ble_pin || '';
          document.getElementById('sys-debug-logs').checked = !!cfg.debug_logs;

          // Tailscale fields. The auth key is write-only.
          document.getElementById('view-tailscale-name').innerText = cfg.tailscale_device_name || 'meshgate-torun';
          document.getElementById('view-tailscale-auth').innerText = cfg.tailscale_has_auth_key ? '•••••••• (stored)' : '(Not set)';
          document.getElementById('input-tailscale-enabled').checked = !!cfg.tailscale_enabled;
          document.getElementById('input-tailscale-name').value = cfg.tailscale_device_name || 'meshgate-torun';
          document.getElementById('input-tailscale-auth').value = '';
          document.getElementById('input-tailscale-clear-auth').checked = false;
          document.getElementById('btn-edit-tailscale').disabled = !cfg.tailscale_supported;
          updateTailscaleStatus();

          // WireGuard fields. Secret inputs intentionally remain empty.
          document.getElementById('view-wireguard-ip').innerText = cfg.wireguard_local_ip || '(Not configured)';
          document.getElementById('view-wireguard-remote').innerText = cfg.wireguard_remote_cidr || '(Not configured)';
          document.getElementById('view-wireguard-endpoint').innerText = cfg.wireguard_endpoint ? cfg.wireguard_endpoint + ':' + cfg.wireguard_endpoint_port : '(Not configured)';
          document.getElementById('view-wireguard-private').innerText = cfg.wireguard_has_private_key ? '•••••••• (stored)' : '(Not set)';
          document.getElementById('view-wireguard-psk').innerText = cfg.wireguard_has_preshared_key ? '•••••••• (stored)' : '(Not set)';
          document.getElementById('input-wireguard-enabled').checked = !!cfg.wireguard_enabled;
          document.getElementById('input-wireguard-local-ip').value = cfg.wireguard_local_ip || '';
          document.getElementById('input-wireguard-remote-cidr').value = cfg.wireguard_remote_cidr || '';
          document.getElementById('input-wireguard-endpoint').value = cfg.wireguard_endpoint || '';
          document.getElementById('input-wireguard-endpoint-port').value = cfg.wireguard_endpoint_port || 51820;
          document.getElementById('input-wireguard-public-key').value = cfg.wireguard_public_key || '';
          document.getElementById('input-wireguard-private-key').value = '';
          document.getElementById('input-wireguard-preshared-key').value = '';
          document.getElementById('input-wireguard-clear-psk').checked = false;
          updateWireguardStatus();

          // MQTT fields
          document.getElementById('input-mqtt-enabled').checked = !!cfg.mqtt_enabled;
          document.getElementById('input-mqtt-tls-insecure').checked = !!cfg.mqtt_tls_insecure;
          document.getElementById('input-mqtt-ca').value = cfg.mqtt_custom_ca || '';
          updateStatus();
        })
        .catch(() => {
          document.getElementById('view-wifi-ssid').innerText = 'Error loading';
          document.getElementById('view-ble-device').innerText = 'Error loading';
        });
    }

    function updateTailscaleStatus() {
      const pill = document.getElementById('view-tailscale-status');
      fetch('/tailscale_status')
        .then(r => r.json())
        .then(status => {
          document.getElementById('view-tailscale-ip').innerText = status.vpn_ip || '(Not connected)';
          document.getElementById('view-tailscale-peers').innerText = status.peer_count || 0;
          if (!currentConfig || !currentConfig.tailscale_supported) {
            pill.className = 'status-pill disabled';
            pill.innerText = 'Requires IDF 5 firmware';
          } else if (!status.enabled) {
            pill.className = 'status-pill disabled';
            pill.innerText = 'Disabled';
          } else if (status.connected) {
            pill.className = 'status-pill connected';
            pill.innerText = 'Connected';
          } else {
            pill.className = 'status-pill connecting';
            pill.innerText = status.state || 'Connecting';
          }
        })
        .catch(() => {
          pill.className = 'status-pill error';
          pill.innerText = 'Status unavailable';
        });
    }

    function toggleEditTailscale(edit) {
      if (edit && currentConfig && !currentConfig.tailscale_supported) {
        return;
      }
      if (edit && currentConfig) {
        document.getElementById('input-tailscale-enabled').checked = !!currentConfig.tailscale_enabled;
        document.getElementById('input-tailscale-name').value = currentConfig.tailscale_device_name || 'meshgate-torun';
        document.getElementById('input-tailscale-auth').value = '';
        document.getElementById('input-tailscale-clear-auth').checked = false;
      }
      document.getElementById('tailscale-view').style.display = edit ? 'none' : 'block';
      document.getElementById('tailscale-edit').style.display = edit ? 'block' : 'none';
      document.getElementById('btn-edit-tailscale').style.display = edit ? 'none' : 'block';
      hideAlert('tailscale-alert');
    }

    function saveTailscale() {
      const enabled = document.getElementById('input-tailscale-enabled').checked;
      const deviceName = document.getElementById('input-tailscale-name').value.trim().toLowerCase();
      const authKey = document.getElementById('input-tailscale-auth').value.trim();
      const clearAuthKey = document.getElementById('input-tailscale-clear-auth').checked;

      if (enabled && (!/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(deviceName) || (!authKey && !currentConfig.tailscale_has_auth_key))) {
        showAlert('tailscale-alert', 'error', 'Enter a lowercase device name and a Tailscale auth key.');
        return;
      }

      const button = document.getElementById('btn-save-tailscale');
      button.disabled = true;
      fetch('/save_tailscale', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'enabled=' + (enabled ? 'true' : 'false') +
              '&device_name=' + encodeURIComponent(deviceName) +
              '&auth_key=' + encodeURIComponent(authKey) +
              '&clear_auth_key=' + (clearAuthKey ? 'true' : 'false')
      })
      .then(async r => {
        const result = await r.json();
        if (!r.ok) throw new Error(result.error || 'Failed to save Tailscale');
        return result;
      })
      .then(() => {
        button.disabled = false;
        showAlert('tailscale-alert', 'success', 'Tailscale saved redundantly. Reboot to connect.');
        setTimeout(() => { toggleEditTailscale(false); loadConfig(); }, 900);
      })
      .catch(err => {
        button.disabled = false;
        showAlert('tailscale-alert', 'error', err.message || 'Network error saving Tailscale.');
      });
    }

    function updateWireguardStatus() {
      const pill = document.getElementById('view-wireguard-status');
      fetch('/wireguard_status')
        .then(r => r.json())
        .then(status => {
          if (!status.enabled) {
            pill.className = 'status-pill disabled';
            pill.innerText = 'Disabled';
          } else if (status.connected) {
            pill.className = 'status-pill connected';
            pill.innerText = 'Connected';
          } else if (status.initialized) {
            pill.className = 'status-pill connecting';
            pill.innerText = 'Handshake pending';
          } else {
            pill.className = 'status-pill waiting';
            pill.innerText = 'Waiting / restart required';
          }
        })
        .catch(() => {
          pill.className = 'status-pill error';
          pill.innerText = 'Status unavailable';
        });
    }

    function toggleEditWireguard(edit) {
      if (edit && currentConfig) {
        document.getElementById('input-wireguard-enabled').checked = !!currentConfig.wireguard_enabled;
        document.getElementById('input-wireguard-local-ip').value = currentConfig.wireguard_local_ip || '';
        document.getElementById('input-wireguard-remote-cidr').value = currentConfig.wireguard_remote_cidr || '';
        document.getElementById('input-wireguard-endpoint').value = currentConfig.wireguard_endpoint || '';
        document.getElementById('input-wireguard-endpoint-port').value = currentConfig.wireguard_endpoint_port || 51820;
        document.getElementById('input-wireguard-public-key').value = currentConfig.wireguard_public_key || '';
        document.getElementById('input-wireguard-private-key').value = '';
        document.getElementById('input-wireguard-preshared-key').value = '';
        document.getElementById('input-wireguard-clear-psk').checked = false;
      }
      document.getElementById('wireguard-view').style.display = edit ? 'none' : 'block';
      document.getElementById('wireguard-edit').style.display = edit ? 'block' : 'none';
      document.getElementById('btn-edit-wireguard').style.display = edit ? 'none' : 'block';
      hideAlert('wireguard-alert');
    }

    function saveWireguard() {
      const enabled = document.getElementById('input-wireguard-enabled').checked;
      const localIp = document.getElementById('input-wireguard-local-ip').value.trim();
      const remoteCidr = document.getElementById('input-wireguard-remote-cidr').value.trim();
      const endpoint = document.getElementById('input-wireguard-endpoint').value.trim();
      const endpointPort = document.getElementById('input-wireguard-endpoint-port').value;
      const privateKey = document.getElementById('input-wireguard-private-key').value.trim();
      const publicKey = document.getElementById('input-wireguard-public-key').value.trim();
      const presharedKey = document.getElementById('input-wireguard-preshared-key').value.trim();
      const clearPsk = document.getElementById('input-wireguard-clear-psk').checked;

      if (enabled && (!localIp || !remoteCidr || !endpoint || !endpointPort || !publicKey || (!privateKey && !currentConfig.wireguard_has_private_key))) {
        showAlert('wireguard-alert', 'error', 'Complete all required WireGuard fields.');
        return;
      }

      const body = 'enabled=' + (enabled ? 'true' : 'false') +
        '&local_ip=' + encodeURIComponent(localIp) +
        '&remote_cidr=' + encodeURIComponent(remoteCidr) +
        '&endpoint=' + encodeURIComponent(endpoint) +
        '&endpoint_port=' + encodeURIComponent(endpointPort) +
        '&private_key=' + encodeURIComponent(privateKey) +
        '&public_key=' + encodeURIComponent(publicKey) +
        '&preshared_key=' + encodeURIComponent(presharedKey) +
        '&clear_psk=' + (clearPsk ? 'true' : 'false');

      const button = document.getElementById('btn-save-wireguard');
      button.disabled = true;
      fetch('/save_wireguard', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      })
      .then(async r => {
        const result = await r.json();
        if (!r.ok) throw new Error(result.error || 'Failed to save WireGuard');
        return result;
      })
      .then(() => {
        button.disabled = false;
        showAlert('wireguard-alert', 'success', 'WireGuard saved redundantly. Reboot to apply.');
        setTimeout(() => { toggleEditWireguard(false); loadConfig(); }, 900);
      })
      .catch(err => {
        button.disabled = false;
        showAlert('wireguard-alert', 'error', err.message || 'Network error saving WireGuard.');
      });
    }

    function toggleEditMqtt(edit) {
      // Reset inputs
      if (edit && currentConfig) {
        document.getElementById('input-mqtt-enabled').checked = !!currentConfig.mqtt_enabled;
        document.getElementById('input-mqtt-tls-insecure').checked = !!currentConfig.mqtt_tls_insecure;
        document.getElementById('input-mqtt-ca').value = currentConfig.mqtt_custom_ca || '';
      }
      document.getElementById('mqtt-view').style.display = edit ? 'none' : 'block';
      document.getElementById('mqtt-edit').style.display = edit ? 'block' : 'none';
      document.getElementById('btn-edit-mqtt').style.display = edit ? 'none' : 'block';
      hideAlert('mqtt-alert');
    }

    function loadCaFile(input) {
      if (input.files && input.files[0]) {
        const reader = new FileReader();
        reader.onload = function(e) {
          document.getElementById('input-mqtt-ca').value = e.target.result;
        };
        reader.readAsText(input.files[0]);
      }
    }

    function saveMqtt() {
      const enabled = document.getElementById('input-mqtt-enabled').checked;
      const tls_insecure = document.getElementById('input-mqtt-tls-insecure').checked;
      const custom_ca = document.getElementById('input-mqtt-ca').value.trim();

      if (custom_ca.length > 3500) {
        showAlert('mqtt-alert', 'error', 'Custom CA certificate is too large (max 3500 characters).');
        return;
      }

      let body = 'mqtt_enabled=' + (enabled ? 'true' : 'false') +
                 '&mqtt_tls_insecure=' + (tls_insecure ? 'true' : 'false') +
                 '&mqtt_custom_ca=' + encodeURIComponent(custom_ca);

      const btnSave = document.getElementById('btn-save-mqtt');
      btnSave.disabled = true;

      fetch('/save_mqtt', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      })
      .then(r => r.json())
      .then(res => {
        btnSave.disabled = false;
        if (res.success) {
          showAlert('mqtt-alert', 'success', 'MQTT configuration saved!');
          setTimeout(() => {
            toggleEditMqtt(false);
            loadConfig();
            updateStatus();
          }, 800);
        } else {
          showAlert('mqtt-alert', 'error', res.error || 'Failed to save MQTT.');
        }
      })
      .catch(() => {
        btnSave.disabled = false;
        showAlert('mqtt-alert', 'error', 'Network error saving MQTT.');
      });
    }

    function updateStatus() {
      if (statusFetchInFlight) return;
      statusFetchInFlight = true;

      fetch('/status')
        .then(r => r.json())
        .then(st => {
          // WiFi Metrics
          if (st.wifi) {
            const wifiPill = document.getElementById('view-wifi-status-pill');
            if (wifiPill) {
              if (st.wifi.is_ap_mode) {
                wifiPill.innerText = 'AP Mode';
                wifiPill.className = 'status-pill waiting';
              } else if (st.wifi.connected) {
                wifiPill.innerText = 'Connected';
                wifiPill.className = 'status-pill connected';
              } else if (!currentConfig.wifi_ssid) {
                wifiPill.innerText = 'Not Configured';
                wifiPill.className = 'status-pill disabled';
              } else {
                wifiPill.innerText = 'Disconnected';
                wifiPill.className = 'status-pill error';
              }
            }

            const ipEl = document.getElementById('view-wifi-ip');
            if (ipEl) ipEl.innerText = st.wifi.ip + (st.wifi.is_ap_mode ? ' (AP Mode)' : '');
            const mdnsEl = document.getElementById('view-wifi-mdns');
            if (mdnsEl) mdnsEl.innerText = st.wifi.mdns_host || '(Not active)';
            const rssiEl = document.getElementById('view-wifi-rssi');
            if (rssiEl) {
              if (st.wifi.connected && !st.wifi.is_ap_mode) {
                let pillClass = 'status-pill error';
                let label = 'Weak';
                if (st.wifi.rssi >= -60) { pillClass = 'status-pill connected'; label = 'Excellent'; }
                else if (st.wifi.rssi >= -70) { pillClass = 'status-pill connected'; label = 'Good'; }
                else if (st.wifi.rssi >= -80) { pillClass = 'status-pill waiting'; label = 'Fair'; }
                rssiEl.innerHTML = st.wifi.rssi + ' dBm <span class="' + pillClass + '" style="margin-left:4px;">' + label + '</span>';
              } else if (st.wifi.is_ap_mode) {
                rssiEl.innerHTML = '<span class="status-pill disabled">N/A (AP Mode)</span>';
              } else {
                rssiEl.innerHTML = '<span class="status-pill disabled">Disconnected</span>';
              }
            }
          }

          // BLE Metrics
          if (st.ble) {
            const blePill = document.getElementById('view-ble-status-pill');
            if (blePill) {
              if (st.ble.connected) {
                blePill.innerText = 'Connected';
                blePill.className = 'status-pill connected';
              } else {
                blePill.innerText = 'Disconnected';
                blePill.className = 'status-pill error';
              }
            }
            const bleTx = document.getElementById('view-ble-tx');
            if (bleTx) bleTx.innerText = st.ble.tx_count || 0;
            const bleRx = document.getElementById('view-ble-rx');
            if (bleRx) bleRx.innerText = st.ble.rx_count || 0;
            const bleInv = document.getElementById('view-ble-inv');
            if (bleInv) bleInv.innerText = st.ble.invalid_count || 0;
          }

          // MQTT Metrics
          if (st.mqtt) {
            const m = st.mqtt;
            const featureEnabledPill = document.getElementById('view-mqtt-feature-enabled-pill');
            if (featureEnabledPill) {
              if (m.gateway_enabled === true) {
                featureEnabledPill.innerText = 'Enabled';
                featureEnabledPill.className = 'status-pill connected';
              } else {
                featureEnabledPill.innerText = 'Disabled';
                featureEnabledPill.className = 'status-pill disabled';
              }
            }

            const mqttStatusContainer = document.getElementById('mqtt-gateway-status-container');
            if (mqttStatusContainer) {
              mqttStatusContainer.style.display = m.gateway_enabled ? 'block' : 'none';
            }

            const statusPill = document.getElementById('view-mqtt-status-pill');
            if (statusPill) {
              statusPill.innerText = m.state || 'Disabled';
              if (m.state === 'Connected') statusPill.className = 'status-pill connected';
              else if (m.state === 'Waiting for Radio Config') statusPill.className = 'status-pill waiting';
              else if (m.state === 'Connecting...') statusPill.className = 'status-pill connecting';
              else if (m.state === 'Disabled') statusPill.className = 'status-pill disabled';
              else statusPill.className = 'status-pill error';
            }

            document.getElementById('view-mqtt-radio-proxy').innerText = m.radio_proxy_enabled ? 'Enabled (Ready)' : (m.radio_server ? 'Disabled on Radio' : '(Waiting for radio)');
            if (m.active_server) {
              document.getElementById('view-mqtt-broker').innerText = m.active_server + ':' + m.active_port + (m.active_tls ? ' (TLS)' : '');
            } else if (m.radio_server) {
              document.getElementById('view-mqtt-broker').innerText = m.radio_server + ':' + m.radio_port + (m.radio_tls ? ' (TLS)' : '');
            } else {
              document.getElementById('view-mqtt-broker').innerText = '(Waiting for radio)';
            }
            document.getElementById('view-mqtt-root').innerText = m.active_root || m.radio_root || 'msh';
            const mqttTx = document.getElementById('view-mqtt-tx');
            if (mqttTx) mqttTx.innerText = m.published || 0;
            const mqttRx = document.getElementById('view-mqtt-rx');
            if (mqttRx) mqttRx.innerText = m.received || 0;
            const mqttInv = document.getElementById('view-mqtt-inv');
            if (mqttInv) mqttInv.innerText = m.invalid || 0;
          }

          // WiFi & TCP Metrics
          if (st.tcp) {
            document.getElementById('view-sys-tcp-clients').innerText = (st.tcp.connected_clients || 0) + ' / ' + (st.tcp.max_clients || 3);
            const tcpTx = document.getElementById('view-tcp-tx');
            if (tcpTx) tcpTx.innerText = st.tcp.tx_count || 0;
            const tcpRx = document.getElementById('view-tcp-rx');
            if (tcpRx) tcpRx.innerText = st.tcp.rx_count || 0;
            const tcpInv = document.getElementById('view-tcp-inv');
            if (tcpInv) tcpInv.innerText = st.tcp.invalid_count || 0;
          }

          // System Metrics
          document.getElementById('view-sys-uptime').innerText = formatUptime(st.uptime_seconds);
          if (st.heap) {
            const freeKb = Math.round(st.heap.free_bytes / 1024);
            const minKb = Math.round(st.heap.min_free_bytes / 1024);
            const maxAllocKb = Math.round(st.heap.max_alloc_bytes / 1024);
            document.getElementById('view-sys-heap').innerText = freeKb + ' KB (Min: ' + minKb + ' KB, MaxBlock: ' + maxAllocKb + ' KB)';
          }
          if (st.stacks) {
            document.getElementById('view-sys-stacks').innerText = 'BLE: ' + (st.stacks.ble_task_free_bytes || 0) + ' B | Net: ' + (st.stacks.net_task_free_bytes || 0) + ' B';
          }

          // Queues
          if (st.queues) {
            if (st.queues.tcp_to_ble) {
              renderProgressBar('view-queue-t2b-bar', 'view-queue-t2b-text', st.queues.tcp_to_ble.waiting, st.queues.tcp_to_ble.capacity, st.queues.tcp_to_ble.dropped);
            }
            if (st.queues.ble_to_tcp) {
              renderProgressBar('view-queue-b2t-bar', 'view-queue-b2t-text', st.queues.ble_to_tcp.waiting, st.queues.ble_to_tcp.capacity, st.queues.ble_to_tcp.dropped);
            }
            if (st.queues.mqtt_to_ble) {
              renderProgressBar('view-queue-m2b-bar', 'view-queue-m2b-text', st.queues.mqtt_to_ble.waiting, st.queues.mqtt_to_ble.capacity, st.queues.mqtt_to_ble.dropped);
            }
          }
        })
        .catch(() => {})
        .finally(() => {
          statusFetchInFlight = false;
          updateWireguardStatus();
          scheduleNextStatusPoll();
        });
    }

    function toggleEditWifi(edit) {
      // Copy current config values back into inputs.
      if (edit && currentConfig) {
        document.getElementById('input-wifi-ssid').value = currentConfig.wifi_ssid || '';
        document.getElementById('input-wifi-pass').value = '';
      }

      document.getElementById('wifi-view').style.display = edit ? 'none' : 'block';
      document.getElementById('wifi-edit').style.display = edit ? 'block' : 'none';
      document.getElementById('btn-edit-wifi').style.display = edit ? 'none' : 'block';
      hideAlert('wifi-alert');
      if (edit) {
        scanWifi();
      } else if (wifiPollInterval) {
        clearInterval(wifiPollInterval);
        wifiPollInterval = null;
      }
    }

    function onWifiNetworkSelected() {
      const select = document.getElementById('select-wifi-ssid');
      const opt = select.options[select.selectedIndex];
      if (opt && opt.value) {
        document.getElementById('input-wifi-ssid').value = opt.value;
      }
    }

    let wifiScanInFlight = false;

    function scanWifi() {
      const select = document.getElementById('select-wifi-ssid');
      const status = document.getElementById('wifi-scan-status');
      const btnScan = document.getElementById('btn-scan-wifi');
      const btnSave = document.getElementById('btn-save-wifi');

      if (wifiPollInterval) {
        clearInterval(wifiPollInterval);
        wifiPollInterval = null;
      }
      wifiScanInFlight = false;

      // Disable scan and save while scanning is active
      btnScan.disabled = true;
      btnSave.disabled = true;

      select.innerHTML = '<option value="">-- Scan in progress... --</option>';
      status.innerText = 'Scanning for WiFi networks...';
      status.style.display = 'block';

      fetch('/start_scan_wifi')
        .then(() => {
          wifiPollInterval = setInterval(() => {
            if (wifiScanInFlight) return;
            wifiScanInFlight = true;

            fetch('/scan_wifi_results')
              .then(r => r.json())
              .then(data => {
                wifiScanInFlight = false;
                if (data.status === 'done') {
                  clearInterval(wifiPollInterval);
                  wifiPollInterval = null;

                  btnScan.disabled = false;
                  btnSave.disabled = false;
                  status.style.display = 'none';

                  select.innerHTML = '<option value="">-- Select Discovered Network --</option>';
                  if (!data.networks || data.networks.length === 0) {
                    status.innerText = 'No WiFi networks found.';
                    status.style.display = 'block';
                  } else {
                    data.networks.forEach(net => {
                      const opt = document.createElement('option');
                      opt.value = net.ssid;
                      const lockStr = net.is_open ? ' 🔓' : ' 🔒';
                      const rssiStr = (net.rssi !== undefined) ? ' [' + net.rssi + ' dBm]' : '';
                      opt.textContent = net.ssid + rssiStr + lockStr;
                      select.appendChild(opt);
                    });
                  }
                }
              })
              .catch(() => {
                wifiScanInFlight = false;
                clearInterval(wifiPollInterval);
                wifiPollInterval = null;
                btnScan.disabled = false;
                btnSave.disabled = false;
                status.innerText = 'Error checking WiFi scan status.';
                status.style.display = 'block';
              });
          }, 800);
        })
        .catch(() => {
          btnScan.disabled = false;
          btnSave.disabled = false;
          status.innerText = 'Error initiating WiFi scan.';
          status.style.display = 'block';
        });
    }

    function toggleEditBle(edit) {
      // Copy current config values back into inputs.
      if (edit && currentConfig) {
        document.getElementById('input-ble-name').value = currentConfig.ble_name || '';
        document.getElementById('input-ble-mac').value = currentConfig.ble_mac || '';
        document.getElementById('input-ble-pin').value = currentConfig.ble_pin || '';
      }

      document.getElementById('ble-view').style.display = edit ? 'none' : 'block';
      document.getElementById('ble-edit').style.display = edit ? 'block' : 'none';
      document.getElementById('btn-edit-ble').style.display = edit ? 'none' : 'block';
      hideAlert('ble-scan-alert');
      hideAlert('ble-edit-alert');
      if (edit) {
        scanBle();
      }
    }

    function onBleDeviceSelected() {
      const select = document.getElementById('select-ble-device');
      const opt = select.options[select.selectedIndex];
      if (opt && opt.value) {
        document.getElementById('input-ble-mac').value = opt.value;
        document.getElementById('input-ble-name').value = opt.getAttribute('data-name') || '';
      }
    }

    function scanBle() {
      const select = document.getElementById('select-ble-device');
      const status = document.getElementById('scan-status');
      const btnScan = document.getElementById('btn-scan');
      const btnTest = document.getElementById('btn-test-ble');
      const btnSave = document.getElementById('btn-save-ble');

      if (pollInterval) {
        clearInterval(pollInterval);
        pollInterval = null;
      }

      // Disable scan, test, and save while scanning is active
      btnScan.disabled = true;
      btnTest.disabled = true;
      btnSave.disabled = true;
      select.disabled = true;
      select.style.display = 'none';

      select.innerHTML = '<option value="">-- Scan in progress... --</option>';
      status.innerText = 'Scanning for Meshtastic radios (8 seconds)...';
      status.style.display = 'block';
      hideAlert('ble-scan-alert');
      hideAlert('ble-edit-alert');

      fetch('/start_scan')
        .then(() => {
          pollInterval = setInterval(() => {
            fetch('/scan_results')
              .then(r => r.json())
              .then(data => {
                if (data.status === 'done') {
                  clearInterval(pollInterval);
                  pollInterval = null;

                  btnScan.disabled = false;
                  btnTest.disabled = false;
                  btnSave.disabled = false;
                  select.disabled = false;
                  status.style.display = 'none';
                  select.style.display = 'block';

                  if (data.error !== null) {
                    showAlert('ble-scan-alert', 'error', data.error);
                  }

                  select.innerHTML = '<option value="">-- Select Meshtastic Radio --</option>';
                  if (!data.devices || data.devices.length === 0) {
                    status.innerText = 'No Meshtastic radio found. Disconnect the radio from the phone, wait 10 seconds, then scan again.';
                    status.style.display = 'block';
                  } else {
                    data.devices.forEach(device => {
                      const opt = document.createElement('option');
                      opt.value = device.mac;
                      opt.setAttribute('data-name', device.name || '');
                      const nameStr = device.name ? device.name : 'Unknown';
                      const rssiStr = (device.rssi !== undefined) ? ' [' + device.rssi + ' dBm]' : '';
                      opt.textContent = nameStr + ' (' + device.mac + ')' + rssiStr;
                      select.appendChild(opt);
                    });
                  }
                }
              })
              .catch(() => {
                clearInterval(pollInterval);
                pollInterval = null;
                btnScan.disabled = false;
                btnTest.disabled = false;
                btnSave.disabled = false;
                select.disabled = false;
                status.innerText = 'Error checking scan status.';
                status.style.display = 'block';
                select.style.display = 'block';
              });
          }, 800);
        })
        .catch(() => {
          btnScan.disabled = false;
          btnTest.disabled = false;
          btnSave.disabled = false;
          select.disabled = false;
          status.innerText = 'Error initiating BLE scan.';
          status.style.display = 'block';
          select.style.display = 'block';
        });
    }

    function saveWifi() {
      const ssid = document.getElementById('input-wifi-ssid').value.trim();
      const pass = document.getElementById('input-wifi-pass').value;

      if (!ssid) {
        showAlert('wifi-alert', 'error', 'WiFi SSID cannot be empty.');
        return;
      }

      const body = 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(pass);
      fetch('/save_wifi', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      })
      .then(r => r.json())
      .then(res => {
        if (res.success) {
          showAlert('wifi-alert', 'success', 'WiFi saved. Connecting now; the setup AP stays available.');
          setTimeout(() => {
            toggleEditWifi(false);
            loadConfig();
            updateStatus();
          }, 800);
        } else {
          showAlert('wifi-alert', 'error', res.error || 'Failed to save WiFi.');
        }
      })
      .catch(() => showAlert('wifi-alert', 'error', 'Network error saving WiFi.'));
    }

    function saveBle() {
      const name = document.getElementById('input-ble-name').value.trim();
      const mac = document.getElementById('input-ble-mac').value.trim();
      const pin = document.getElementById('input-ble-pin').value.trim();

      if (!mac) {
        showAlert('ble-edit-alert', 'error', 'Target BLE MAC address cannot be empty.');
        return;
      }
      if (!pin) {
        showAlert('ble-edit-alert', 'error', 'BLE 6-digit PIN cannot be empty.');
        return;
      }

      const body = 'ble_name=' + encodeURIComponent(name) + '&ble_mac=' + encodeURIComponent(mac) + '&ble_pin=' + encodeURIComponent(pin);
      fetch('/save_ble', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      })
      .then(r => r.json())
      .then(res => {
        if (res.success) {
          showAlert('ble-edit-alert', 'success', 'Bluetooth settings saved!');
          setTimeout(() => {
            toggleEditBle(false);
            loadConfig();
          }, 800);
        } else {
          showAlert('ble-edit-alert', 'error', res.error || 'Failed to save Bluetooth.');
        }
      })
      .catch(() => showAlert('ble-edit-alert', 'error', 'Network error saving Bluetooth.'));
    }

    let testInterval = null;

    function testBleConnection() {
      const mac = document.getElementById('input-ble-mac').value.trim();
      const pin = document.getElementById('input-ble-pin').value.trim();
      const btnTest = document.getElementById('btn-test-ble');
      const btnScan = document.getElementById('btn-scan');
      const btnSave = document.getElementById('btn-save-ble');
      const select = document.getElementById('select-ble-device');

      if (!mac) {
        showAlert('ble-edit-alert', 'error', 'Please enter or select a BLE MAC address first.');
        return;
      }
      if (!pin) {
        showAlert('ble-edit-alert', 'error', 'Please enter the 6-digit pairing PIN.');
        return;
      }

      if (testInterval) {
        clearInterval(testInterval);
        testInterval = null;
      }

      // Disable test, scan, and save while testing is active
      btnTest.disabled = true;
      btnScan.disabled = true;
      btnSave.disabled = true;
      select.disabled = true;
      btnTest.innerText = 'Connecting & Testing...';
      showAlert('ble-edit-alert', 'info', 'Connecting to ' + mac + ' and verifying Meshtastic radio service...');

      const body = 'ble_mac=' + encodeURIComponent(mac) + '&ble_pin=' + encodeURIComponent(pin);
      let elapsedTicks = 0;

      fetch('/start_test_ble', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: body
      })
      .then(() => {
        testInterval = setInterval(() => {
          elapsedTicks++;
          if (elapsedTicks > 15) { // 12 seconds max
            clearInterval(testInterval);
            testInterval = null;
            btnTest.disabled = false;
            btnScan.disabled = false;
            btnSave.disabled = false;
            select.disabled = false;
            btnTest.innerText = '⚡ Test Connection';
            showAlert('ble-edit-alert', 'error', 'Test timed out after 12 seconds.');
            return;
          }

          fetch('/test_ble_status')
            .then(r => r.json())
            .then(res => {
              if (res.status === 'done') {
                clearInterval(testInterval);
                testInterval = null;
                btnTest.disabled = false;
                btnScan.disabled = false;
                btnSave.disabled = false;
                select.disabled = false;
                btnTest.innerText = '⚡ Test Connection';

                if (res.success) {
                  showAlert('ble-edit-alert', 'success', '✓ ' + (res.message || 'Connected and verified successfully!'));
                } else {
                  showAlert('ble-edit-alert', 'error', '✗ ' + (res.error || res.message || 'Connection test failed.'));
                }
              }
            })
            .catch(() => {
              // Retry on transient poll error
            });
        }, 800);
      })
      .catch(() => {
        btnTest.disabled = false;
        btnScan.disabled = false;
        btnSave.disabled = false;
        select.disabled = false;
        btnTest.innerText = '⚡ Test Connection';
        showAlert('ble-edit-alert', 'error', 'Could not initiate connection test request.');
      });
    }

    let modalConfirmCallback = null;

    function showModal(title, msg, btnText, btnClass, onConfirm) {
      document.getElementById('modal-title').innerText = title;
      document.getElementById('modal-msg').innerText = msg;
      const btn = document.getElementById('modal-btn-confirm');
      btn.innerText = btnText;
      btn.className = btnClass;
      modalConfirmCallback = onConfirm;
      document.getElementById('modal-overlay').style.display = 'flex';
    }

    function closeModal() {
      document.getElementById('modal-overlay').style.display = 'none';
      modalConfirmCallback = null;
    }

    function confirmModalAction() {
      if (modalConfirmCallback) {
        modalConfirmCallback();
      }
      closeModal();
    }

    function saveSystemSettings() {
      const debugLogs = document.getElementById('sys-debug-logs').checked;
      fetch('/save_system', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'debug_logs=' + (debugLogs ? 'true' : 'false')
      }).then(r => r.json()).then(res => {
        if (res.success) {
          showAlert('system-alert', 'success', 'System settings saved!');
          setTimeout(() => { document.getElementById('system-alert').style.display = 'none'; }, 2000);
        } else {
          showAlert('system-alert', 'error', res.error || 'Failed to save');
        }
      }).catch(err => {
        showAlert('system-alert', 'error', 'Error saving system settings');
      });
    }

    function rebootBridge() {
      showModal(
        'Reboot Bridge',
        'Are you sure you want to reboot the bridge and start in normal mode?',
        'Reboot Now',
        'btn-primary',
        function() {
          const btnReboot = document.getElementById('btn-reboot');
          const btnReset = document.getElementById('btn-reset');
          btnReboot.disabled = true;
          btnReset.disabled = true;
          showAlert('system-alert', 'info', 'Bridge is rebooting... Connect to your local WiFi to use.');
          fetch('/reboot', { method: 'POST' });
        }
      );
    }

    function resetBridge() {
      showModal(
        'Reset All Settings',
        'Are you sure you want to erase all saved settings? This will restore factory defaults and restart in setup mode.',
        'Reset & Erase',
        'btn-danger',
        function() {
          const btnReset = document.getElementById('btn-reset');
          const btnReboot = document.getElementById('btn-reboot');
          btnReset.disabled = true;
          btnReboot.disabled = true;
          showAlert('system-alert', 'warning', 'All settings erased. Rebooting into Setup Mode...');
          fetch('/reset', { method: 'POST' });
        }
      );
    }

    document.addEventListener('keydown', function(e) {
      if (e.key && e.key.toLowerCase() === 'p') {
        const active = document.activeElement;
        if (active && ((active.tagName === 'INPUT' && active.type !== 'checkbox') || active.tagName === 'TEXTAREA' || active.tagName === 'SELECT')) {
          return;
        }
        pollIntervalIdx = (pollIntervalIdx + 1) % pollIntervals.length;
        const rateSec = pollIntervals[pollIntervalIdx] / 1000;
        const rateEl = document.getElementById('sys-poll-rate');
        if (rateEl) rateEl.innerText = rateSec + 's';
        if (statusPollTimer) {
          clearTimeout(statusPollTimer);
          statusPollTimer = null;
        }
        updateStatus();
      }
    });

    window.onload = function() {
      document.getElementById('modal-btn-confirm').onclick = confirmModalAction;
      loadConfig();
      updateStatus();
    };
  </script>
</body>
</html>
)rawliteral";

/**
 * Initialize the Configuration UI.
 */
void config_ui_init() {
    preferences.begin(PREF_NAMESPACE, false);
    backupPreferences.begin(BACKUP_PREF_NAMESPACE, false);

    ensureOtaToken();

    if (!preferences.isKey("wifi_ssid") && backupPreferences.isKey("wifi_ssid")) {
        BridgeConfig recovered = config_ui_load();
        writeStoredConfiguration(preferences, recovered);
        log_w("[Config] Primary NVS configuration restored from redundant backup");
    } else if (preferences.isKey("wifi_ssid")) {
        backupCurrentConfiguration();
    }
}

/**
 * @return BridgeConfig preferences retrieved from NVS.
 */
BridgeConfig config_ui_load() {
    BridgeConfig cfg;

    auto getString = [](const char* key, const char* fallback = "") -> String {
        if (preferences.isKey(key)) return preferences.getString(key, fallback);
        if (backupPreferences.isKey(key)) return backupPreferences.getString(key, fallback);
        return String(fallback);
    };
    auto getBool = [](const char* key, bool fallback = false) -> bool {
        if (preferences.isKey(key)) return preferences.getBool(key, fallback);
        if (backupPreferences.isKey(key)) return backupPreferences.getBool(key, fallback);
        return fallback;
    };
    auto getUShort = [](const char* key, uint16_t fallback) -> uint16_t {
        if (preferences.isKey(key)) return preferences.getUShort(key, fallback);
        if (backupPreferences.isKey(key)) return backupPreferences.getUShort(key, fallback);
        return fallback;
    };

    // Wifi settings
    cfg.wifi_ssid = getString("wifi_ssid");
    cfg.wifi_pass = getString("wifi_pass");

    // Bluetooth settings
    cfg.ble_name = getString("ble_name");
    cfg.ble_mac = getString("ble_mac");
    cfg.ble_pin = getString("ble_pin");

    // System settings
    cfg.debug_logs = getBool("debug_logs", false);

    // WireGuard preferences
    cfg.wireguard_enabled = getBool("wg_enabled", false);
    cfg.wireguard_local_ip = getString("wg_local_ip");
    cfg.wireguard_remote_cidr = getString("wg_remote");
    cfg.wireguard_endpoint = getString("wg_endpoint");
    cfg.wireguard_endpoint_port = getUShort("wg_ep_port", 51820);
    cfg.wireguard_private_key = getString("wg_priv_key");
    cfg.wireguard_public_key = getString("wg_pub_key");
    cfg.wireguard_preshared_key = getString("wg_psk");

    // Tailscale preferences
    cfg.tailscale_enabled = getBool("ts_enabled", false);
    cfg.tailscale_device_name = getString("ts_name", "meshgate-torun");
    cfg.tailscale_auth_key = getString("ts_auth");

    // MQTT preferences
    cfg.mqtt_enabled = getBool("mqtt_enabled", false);
    cfg.mqtt_tls_insecure = getBool("mqtt_tls_insec", false);
    cfg.mqtt_custom_ca = getString("mqtt_custom_ca");
    return cfg;
}

String config_ui_get_ota_token() {
    String token = preferences.getString("ota_token", "");
    if (!isValidOtaToken(token)) {
        token = backupPreferences.getString("ota_token", "");
    }
    return isValidOtaToken(token) ? token : String();
}

static void backupCurrentConfiguration() {
    BridgeConfig cfg = config_ui_load();
    writeStoredConfiguration(backupPreferences, cfg);
}

/**
 * Start the UI Captive Portal Server.
 */
void config_ui_start_server() {
    ota_update_register(server, config_ui_get_ota_token());

    // Machine-readable diagnostics endpoint used for remote support. It mirrors
    // the bounded serial log ring without exposing pairing PIN values.
    server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse* response = request->beginResponse(200, "text/plain; charset=utf-8", web_log_snapshot());
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // Serve the main HTML page directly from PROGMEM flash avoiding heap copy.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET / requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        request->send(200, "text/html; charset=utf-8", (const uint8_t*)index_html, sizeof(index_html) - 1);
    });

    // Fetch current configuration JSON
    server.on("/config", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /config requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        BridgeConfig cfg = config_ui_load();
        AsyncResponseStream *response = request->beginResponseStream("application/json");

        response->print("{\"wifi_ssid\":\"");
        utils_escape_json_print(*response, cfg.wifi_ssid);
        response->print("\",\"wifi_has_pass\":");
        response->print(cfg.wifi_pass.length() > 0 ? "true" : "false");
        response->print(",\"ble_name\":\"");
        utils_escape_json_print(*response, cfg.ble_name);
        response->print("\",\"ble_mac\":\"");
        utils_escape_json_print(*response, cfg.ble_mac);
        response->print("\",\"ble_pin\":\"");
        utils_escape_json_print(*response, cfg.ble_pin);
        response->print("\",\"debug_logs\":");
        response->print(cfg.debug_logs ? "true" : "false");
        response->print(",\"mqtt_enabled\":");
        response->print(cfg.mqtt_enabled ? "true" : "false");
        response->print(",\"mqtt_tls_insecure\":");
        response->print(cfg.mqtt_tls_insecure ? "true" : "false");
        response->print(",\"mqtt_custom_ca\":\"");
        utils_escape_json_print(*response, cfg.mqtt_custom_ca);
        response->print("\",\"wireguard_enabled\":");
        response->print(cfg.wireguard_enabled ? "true" : "false");
        response->print(",\"wireguard_local_ip\":\"");
        utils_escape_json_print(*response, cfg.wireguard_local_ip);
        response->print("\",\"wireguard_remote_cidr\":\"");
        utils_escape_json_print(*response, cfg.wireguard_remote_cidr);
        response->print("\",\"wireguard_endpoint\":\"");
        utils_escape_json_print(*response, cfg.wireguard_endpoint);
        response->print("\",\"wireguard_endpoint_port\":");
        response->print(cfg.wireguard_endpoint_port);
        response->print(",\"wireguard_public_key\":\"");
        utils_escape_json_print(*response, cfg.wireguard_public_key);
        response->print("\",\"wireguard_has_private_key\":");
        response->print(cfg.wireguard_private_key.isEmpty() ? "false" : "true");
        response->print(",\"wireguard_has_preshared_key\":");
        response->print(cfg.wireguard_preshared_key.isEmpty() ? "false" : "true");
        response->print(",\"tailscale_enabled\":");
        response->print(cfg.tailscale_enabled ? "true" : "false");
        response->print(",\"tailscale_supported\":");
#ifdef ESP_IDF5_MIGRATION_BUILD
        response->print("true");
#else
        response->print("false");
#endif
        response->print(",\"tailscale_device_name\":\"");
        utils_escape_json_print(*response, cfg.tailscale_device_name);
        response->print("\",\"tailscale_has_auth_key\":");
        response->print(cfg.tailscale_auth_key.isEmpty() ? "false" : "true");
        response->print("}");

        request->send(response);
    });

    /**
     * Fetch unified live system, network, queue, and MQTT telemetry status.
     *
     * PERFORMANCE / MEMORY CONCURRENCY TRADEOFF NOTE:
     * This handler gathers diagnostic stats across FreeRTOS tasks and cores in a 100% wait-free manner.
     * To prevent any possible bridging throughput degradation or fast-path lock contention:
     * 1. We read atomic scalar values using memory_order_relaxed.
     * 2. We inspect FreeRTOS queues via non-blocking uxQueueMessagesWaiting.
     * 3. We inspect task stacks via uxTaskGetStackHighWaterMark without pausing execution.
     * 4. We do NOT acquire heavy mutex locks (like tcpClientsMutex).
     * Concession: telemetry values might reflect a slightly staggered snapshot across cores,
     * which is strongly preferred over stalling live BLE/TCP packet transfers.
    */
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /status requested (Client: %s)", request->client()->remoteIP().toString().c_str());

        SystemTelemetrySnapshot snapshot = diag_telemetry_get_snapshot();
        AsyncResponseStream *response = request->beginResponseStream("application/json");
        diag_telemetry_format_json(snapshot, *response);
        request->send(response);
    });

    server.on("/wireguard_status", HTTP_GET, [](AsyncWebServerRequest *request){
        String json = "{\"enabled\":" + String(wireguard_net_is_enabled() ? "true" : "false") +
                      ",\"initialized\":" + String(wireguard_net_is_initialized() ? "true" : "false") +
                      ",\"connected\":" + String(wireguard_net_is_connected() ? "true" : "false") + "}";
        request->send(200, "application/json", json);
    });

    server.on("/tailscale_status", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        response->print("{\"enabled\":");
        response->print(tailscale_net_is_enabled() ? "true" : "false");
        response->print(",\"connected\":");
        response->print(tailscale_net_is_connected() ? "true" : "false");
        response->print(",\"derp_connected\":");
        response->print(tailscale_net_is_derp_connected() ? "true" : "false");
        response->print(",\"state\":\"");
        utils_escape_json_print(*response, tailscale_net_state_name());
        response->print("\",\"vpn_ip\":\"");
        utils_escape_json_print(*response, tailscale_net_vpn_ip());
        response->print("\",\"peer_count\":");
        const int peerCount = tailscale_net_peer_count();
        response->print(peerCount);
        response->print(",\"peers\":[");
        for (int i = 0; i < peerCount; ++i) {
            if (i > 0) response->print(',');
            response->print("{\"ip\":\"");
            utils_escape_json_print(*response, tailscale_net_peer_ip(i));
            response->print("\",\"name\":\"");
            utils_escape_json_print(*response, tailscale_net_peer_name(i));
            response->print("\",\"online\":");
            response->print(tailscale_net_peer_online(i) ? "true" : "false");
            response->print(",\"direct\":");
            response->print(tailscale_net_peer_direct(i) ? "true" : "false");
            response->print('}');
        }
        response->print(']');
        response->print("}");
        request->send(response);
    });

    /**
     * Standards-compliant Prometheus / OpenMetrics scraping endpoint.
     * Uses AsyncResponseStream to stream metrics chunk-by-chunk directly into lwIP
     * network output buffers with zero large contiguous heap allocations.
     */
    server.on("/metrics", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /metrics requested (Client: %s)", request->client()->remoteIP().toString().c_str());

        SystemTelemetrySnapshot snapshot = diag_telemetry_get_snapshot();
        AsyncResponseStream *response = request->beginResponseStream("text/plain; version=0.0.4; charset=utf-8");
        diag_telemetry_format_prometheus(snapshot, *response);
        request->send(response);
    });

    // Save MQTT configuration
    server.on("/save_mqtt", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /save_mqtt requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        bool enabled = request->hasParam("mqtt_enabled", true) && (request->getParam("mqtt_enabled", true)->value() == "true");
        bool tlsInsecureVal = request->hasParam("mqtt_tls_insecure", true) && (request->getParam("mqtt_tls_insecure", true)->value() == "true");
        String customCaVal = request->hasParam("mqtt_custom_ca", true) ? request->getParam("mqtt_custom_ca", true)->value() : "";
        if (customCaVal.length() > MQTT_MAX_CUSTOM_CA_LENGTH) {
            customCaVal = customCaVal.substring(0, MQTT_MAX_CUSTOM_CA_LENGTH);
        }

        preferences.putBool("mqtt_enabled", enabled);
        preferences.putBool("mqtt_tls_insec", tlsInsecureVal);
        preferences.putString("mqtt_custom_ca", customCaVal);
        backupCurrentConfiguration();

        // Apply updated MQTT config immediately to the runtime subsystem
        BridgeConfig fullCfg = config_ui_load();
        MqttConfig mcfg;
        mcfg.enabled = fullCfg.mqtt_enabled;
        mcfg.tls_insecure = fullCfg.mqtt_tls_insecure;
        mcfg.custom_ca = fullCfg.mqtt_custom_ca;

        // Only apply live configuration to the runtime subsystem if operating in Normal mode.
        // In AP / Setup mode, settings are safely stored in NVS and applied on normal boot.
        if (!wifi_net_is_ap_mode()) {
            mqtt_net_apply_config(mcfg);
        }

        request->send(200, "application/json", "{\"success\":true}");
    });

    // Save WireGuard configuration. Secret fields are write-only: an empty
    // value keeps the previously stored key, unless clear_psk is requested.
    server.on("/save_wireguard", HTTP_POST, [](AsyncWebServerRequest *request){
        bool enabled = request->hasParam("enabled", true) && request->getParam("enabled", true)->value() == "true";
        String localIp = request->hasParam("local_ip", true) ? request->getParam("local_ip", true)->value() : "";
        String remoteCidr = request->hasParam("remote_cidr", true) ? request->getParam("remote_cidr", true)->value() : "";
        String endpoint = request->hasParam("endpoint", true) ? request->getParam("endpoint", true)->value() : "";
        uint32_t portValue = request->hasParam("endpoint_port", true) ? request->getParam("endpoint_port", true)->value().toInt() : 0;
        String publicKey = request->hasParam("public_key", true) ? request->getParam("public_key", true)->value() : "";
        String privateKey = request->hasParam("private_key", true) ? request->getParam("private_key", true)->value() : "";
        String presharedKey = request->hasParam("preshared_key", true) ? request->getParam("preshared_key", true)->value() : "";
        bool clearPsk = request->hasParam("clear_psk", true) && request->getParam("clear_psk", true)->value() == "true";

        BridgeConfig oldCfg = config_ui_load();
        if (privateKey.isEmpty()) privateKey = oldCfg.wireguard_private_key;
        if (presharedKey.isEmpty() && !clearPsk) presharedKey = oldCfg.wireguard_preshared_key;
        if (clearPsk) presharedKey = "";

        if (enabled && (localIp.isEmpty() || remoteCidr.isEmpty() || endpoint.isEmpty() ||
                        portValue == 0 || portValue > 65535 || publicKey.length() != 44 ||
                        privateKey.length() != 44 || (!presharedKey.isEmpty() && presharedKey.length() != 44))) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"WireGuard configuration is incomplete or contains an invalid key/port.\"}");
            return;
        }

        preferences.putBool("wg_enabled", enabled);
        if (enabled) preferences.putBool("ts_enabled", false);
        preferences.putString("wg_local_ip", localIp);
        preferences.putString("wg_remote", remoteCidr);
        preferences.putString("wg_endpoint", endpoint);
        preferences.putUShort("wg_ep_port", static_cast<uint16_t>(portValue == 0 ? 51820 : portValue));
        preferences.putString("wg_priv_key", privateKey);
        preferences.putString("wg_pub_key", publicKey);
        preferences.putString("wg_psk", presharedKey);
        backupCurrentConfiguration();

        request->send(200, "application/json", "{\"success\":true,\"restart_required\":true}");
    });

    // Save native Tailscale configuration. The auth key is write-only; an
    // empty value keeps the existing key unless clear_auth_key is requested.
    server.on("/save_tailscale", HTTP_POST, [](AsyncWebServerRequest *request){
        const bool enabled = request->hasParam("enabled", true) && request->getParam("enabled", true)->value() == "true";
        String deviceName = request->hasParam("device_name", true) ? request->getParam("device_name", true)->value() : "";
        String authKey = request->hasParam("auth_key", true) ? request->getParam("auth_key", true)->value() : "";
        const bool clearAuthKey = request->hasParam("clear_auth_key", true) && request->getParam("clear_auth_key", true)->value() == "true";
        deviceName.trim();
        authKey.trim();

        BridgeConfig oldCfg = config_ui_load();
        if (authKey.isEmpty() && !clearAuthKey) authKey = oldCfg.tailscale_auth_key;
        if (clearAuthKey) authKey = "";

        bool validName = !deviceName.isEmpty() && deviceName.length() <= 63;
        for (size_t i = 0; validName && i < deviceName.length(); ++i) {
            const char c = deviceName[i];
            validName = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        }
        if (validName) {
            validName = deviceName[0] != '-' && deviceName[deviceName.length() - 1] != '-';
        }
        const bool validAuthKey = !authKey.isEmpty() && authKey.length() <= 128 && authKey.startsWith("tskey-auth-");
        if (enabled && (!validName || !validAuthKey)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Tailscale requires a lowercase hostname and a valid tskey-auth key.\"}");
            return;
        }

        preferences.putBool("ts_enabled", enabled);
        preferences.putString("ts_name", deviceName);
        preferences.putString("ts_auth", authKey);
        if (enabled) preferences.putBool("wg_enabled", false);
        backupCurrentConfiguration();
        request->send(200, "application/json", "{\"success\":true,\"restart_required\":true}");
    });

    // Trigger the WiFi scan asynchronously
    server.on("/start_scan_wifi", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /start_scan_wifi requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        wifi_net_start_scan();
        request->send(200, "application/json", "{\"status\": \"started\"}");
    });

    // Poll for WiFi scan results
    server.on("/scan_wifi_results", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /scan_wifi_results requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (wifi_net_is_scanning()) {
            request->send(200, "application/json", "{\"status\": \"scanning\"}");
        } else {
            String jsonResults = wifi_net_get_scan_results_json();
            request->send(200, "application/json", "{\"status\": \"done\", \"networks\": " + jsonResults + "}");
        }
    });

    // Trigger the BLE scan asynchronously
    server.on("/start_scan", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /start_scan requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        ble_client_start_scan();
        request->send(200, "application/json", "{\"status\": \"started\"}");
    });

    // Poll for BLE scan results
    server.on("/scan_results", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /scan_results requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (ble_client_is_scanning()) {
            request->send(200, "application/json", "{\"status\": \"scanning\"}");
        } else {
            String jsonResults = ble_client_get_scan_results_json();
            String lastScanError = ble_client_get_scan_error();
            request->send(200, "application/json", "{\"status\": \"done\", \"error\": " + (lastScanError.isEmpty() ? "null" : ( "\"" + utils_escape_json(lastScanError) + "\"")) + ", \"devices\": " + jsonResults + "}");
        }
    });

    // Start async Bluetooth Connection Test
    server.on("/start_test_ble", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /start_test_ble requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (request->hasParam("ble_mac", true) && request->hasParam("ble_pin", true)) {
            String mac = request->getParam("ble_mac", true)->value();
            String pin = request->getParam("ble_pin", true)->value();

            ble_client_start_test(mac, pin);
            request->send(200, "application/json", "{\"status\":\"started\"}");
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing ble_mac or ble_pin parameter.\"}");
        }
    });

    // Poll Bluetooth Connection Test Status
    server.on("/test_ble_status", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET /test_ble_status requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (ble_client_is_testing()) {
            request->send(200, "application/json", "{\"status\":\"testing\"}");
        } else {
            BleTestResult res = ble_client_get_test_result();
            String json = "{\"status\":\"done\",";
            json += "\"success\":" + String(res.success ? "true" : "false") + ",";
            if (res.success) {
                json += "\"message\":\"" + res.message + "\"";
            } else {
                json += "\"error\":\"" + res.message + "\"";
            }
            json += "}";
            request->send(200, "application/json", json);
        }
    });

    // Save WiFi configuration
    server.on("/save_wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /save_wifi requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (request->hasParam("ssid", true)) {
            String ssid = request->getParam("ssid", true)->value();
            preferences.putString("wifi_ssid", ssid);
            if (request->hasParam("pass", true)) {
                String pass = request->getParam("pass", true)->value();
                if (pass.length() > 0) {
                    preferences.putString("wifi_pass", pass);
                }
            }
            backupCurrentConfiguration();
            request->send(200, "application/json", "{\"success\":true}");
            BridgeConfig updated = config_ui_load();
            wifi_net_connect_ap_sta_async(updated.wifi_ssid, updated.wifi_pass);
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing ssid parameter.\"}");
        }
    });

    // Save Bluetooth configuration
    server.on("/save_ble", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /save_ble requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (request->hasParam("ble_mac", true) && request->hasParam("ble_pin", true)) {
            String mac = request->getParam("ble_mac", true)->value();
            String pin = request->getParam("ble_pin", true)->value();
            String name = request->hasParam("ble_name", true) ? request->getParam("ble_name", true)->value() : "";
            preferences.putString("ble_name", name);
            preferences.putString("ble_mac", mac);
            preferences.putString("ble_pin", pin);
            backupCurrentConfiguration();
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing ble_mac or ble_pin parameter.\"}");
        }
    });

    // Save System configuration
    server.on("/save_system", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /save_system requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (request->hasParam("debug_logs", true)) {
            String val = request->getParam("debug_logs", true)->value();
            bool enableDebug = (val == "true");

            // Update global debug log flag based on passed value
            utils_set_debug_logging(enableDebug);

            // Push into preferences.
            preferences.putBool("debug_logs", enableDebug);
            backupCurrentConfiguration();
            request->send(200, "application/json", "{\"success\":true}");
        } else {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing debug_logs parameter.\"}");
        }
    });

    // Reboot system
    server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /reboot requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (!scheduleRestart()) {
            request->send(503, "application/json", "{\"success\":false,\"message\":\"Unable to schedule reboot\"}");
            return;
        }
        request->send(202, "application/json", "{\"success\":true,\"message\":\"Reboot scheduled\"}");
    });

    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        log_i("[API] Remote reboot requested from %s", request->client()->remoteIP().toString().c_str());
        if (!scheduleRestart()) {
            request->send(503, "application/json", "{\"success\":false,\"message\":\"Unable to schedule reboot\"}");
            return;
        }
        request->send(202, "application/json", "{\"success\":true,\"message\":\"Reboot scheduled\"}");
    });

    // Reset all settings and reboot
    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /reset requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        tailscale_net_factory_reset();
        preferences.clear();
        backupPreferences.clear();
        if (!scheduleRestart()) {
            request->send(503, "application/json", "{\"success\":false,\"message\":\"Settings erased but reboot scheduling failed\"}");
            return;
        }
        request->send(202, "application/json", "{\"success\":true,\"message\":\"Settings erased. Reboot scheduled\"}");
    });

    // Fallback for captive portal redirection
    server.onNotFound([](AsyncWebServerRequest *request){
        if (request->url().endsWith(".ico") || request->url().endsWith(".png")) {
            request->send(404);
            return;
        }
        if (wifi_net_is_ap_mode()) {
            log_d("[WebUI] AP Captive portal redirect for '%s' -> /", request->url().c_str());
            request->redirect("/");
        } else {
            log_d("[WebUI] 404 Not Found for '%s'", request->url().c_str());
            request->send(404, "text/plain", "Not Found");
        }
    });

    server.begin();
}

/**
 * Shutdown the captive portal webserver.
 */
void config_ui_stop_server() {
    server.end();
}
