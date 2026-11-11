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
#include "message_mailbox.h"
#include "reliability_manager.h"
#include "device_presence.h"
#include <esp_heap_caps.h>

/**
 * For retrieving configuration properties stored in NVS.
 */
Preferences preferences;
Preferences backupPreferences;

/**
 * Webserver for Captive Portal/Setup UI.
 */
AsyncWebServer server(80);

// Official MeshGate logo. PlatformIO embeds the exact shared SVG used by the
// repository and Web Flasher, so the captive portal never needs the internet.
extern const uint8_t meshgate_logo_svg_start[] asm("_binary_web_assets_meshgate_logo_svg_start");
extern const uint8_t meshgate_logo_svg_end[] asm("_binary_web_assets_meshgate_logo_svg_end");
extern const uint8_t index_html_gz_start[] asm("_binary_web_generated_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_web_generated_index_html_gz_end");

namespace {
constexpr uint8_t MAX_NORMAL_WEB_REQUESTS = 3;
constexpr uint8_t MAX_RESERVED_WEB_REQUESTS = 3;
constexpr uint8_t LOW_MEMORY_NORMAL_WEB_REQUESTS = 1;
constexpr uint8_t LOW_MEMORY_RESERVED_WEB_REQUESTS = 2;
constexpr uint32_t WEB_LOW_FREE_HEAP_BYTES = 24 * 1024;
constexpr uint32_t WEB_LOW_LARGEST_BLOCK_BYTES = 8 * 1024;

portMUX_TYPE s_webAdmissionMux = portMUX_INITIALIZER_UNLOCKED;
uint8_t s_activeNormalWebRequests = 0;
uint8_t s_activeReservedWebRequests = 0;
bool s_logRequestActive = false;

bool isReservedWebPath(const String& url) {
    return url == "/status" ||
           url == "/api/reboot" ||
           url == "/reboot" ||
           url.startsWith("/api/ota") ||
           url.startsWith("/api/v1/toradio") ||
           url.startsWith("/api/v1/fromradio");
}

void releaseNormalWebRequest() {
    portENTER_CRITICAL(&s_webAdmissionMux);
    if (s_activeNormalWebRequests > 0) --s_activeNormalWebRequests;
    portEXIT_CRITICAL(&s_webAdmissionMux);
}

void releaseReservedWebRequest() {
    portENTER_CRITICAL(&s_webAdmissionMux);
    if (s_activeReservedWebRequests > 0) --s_activeReservedWebRequests;
    portEXIT_CRITICAL(&s_webAdmissionMux);
}

void releaseLogWebRequest() {
    portENTER_CRITICAL(&s_webAdmissionMux);
    if (s_activeNormalWebRequests > 0) --s_activeNormalWebRequests;
    s_logRequestActive = false;
    portEXIT_CRITICAL(&s_webAdmissionMux);
}

bool acquireWebRequest(AsyncWebServerRequest* request) {
    const String& url = request->url();
    const bool reserved = isReservedWebPath(url);
    const bool logRequest = url == "/api/logs";
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t largestBlock = static_cast<uint32_t>(heap_caps_get_largest_free_block(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT
    ));
    const bool memoryConstrained = freeHeap < WEB_LOW_FREE_HEAP_BYTES ||
                                   largestBlock < WEB_LOW_LARGEST_BLOCK_BYTES;
    const uint8_t normalLimit = memoryConstrained
        ? LOW_MEMORY_NORMAL_WEB_REQUESTS
        : MAX_NORMAL_WEB_REQUESTS;
    const uint8_t reservedLimit = memoryConstrained
        ? LOW_MEMORY_RESERVED_WEB_REQUESTS
        : MAX_RESERVED_WEB_REQUESTS;

    bool admitted = false;
    portENTER_CRITICAL(&s_webAdmissionMux);
    if (reserved) {
        if (s_activeReservedWebRequests < reservedLimit) {
            ++s_activeReservedWebRequests;
            admitted = true;
        }
    } else if (s_activeNormalWebRequests < normalLimit &&
               (!logRequest || !s_logRequestActive)) {
        ++s_activeNormalWebRequests;
        if (logRequest) s_logRequestActive = true;
        admitted = true;
    }
    portEXIT_CRITICAL(&s_webAdmissionMux);

    if (!admitted) return false;
    if (reserved) request->onDisconnect(releaseReservedWebRequest);
    else if (logRequest) request->onDisconnect(releaseLogWebRequest);
    else request->onDisconnect(releaseNormalWebRequest);
    return true;
}

void sendWebBusy(AsyncWebServerRequest* request) {
    AsyncWebServerResponse* response = request->beginResponse(
        503,
        "application/json",
        "{\"success\":false,\"error\":\"MeshGate is busy; retry shortly\"}"
    );
    response->addHeader("Retry-After", "2");
    response->addHeader("Cache-Control", "no-store");
    response->addHeader("Connection", "close");
    request->send(response);
}
} // namespace

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

static bool isValidBlePin(const String& pin) {
    if (pin.length() != 6) {
        return false;
    }
    for (size_t i = 0; i < pin.length(); ++i) {
        if (pin[i] < '0' || pin[i] > '9') {
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

static bool scheduleRestart() {
    return reliability_manager_request_restart(1000);
}

struct HttpPhoneApiBody {
    size_t expected = 0;
    size_t received = 0;
    bool invalid = false;
    uint8_t data[MESHTASTIC_MAX_PACKET_SIZE] = {};
};

static void addApiCorsHeaders(AsyncWebServerResponse* response, const char* methods) {
    response->addHeader("Access-Control-Allow-Origin", "*");
    response->addHeader("Access-Control-Allow-Methods", methods);
    response->addHeader("Access-Control-Allow-Headers", "Content-Type, Accept");
    // Chromium's Private Network Access preflight requires this when a local
    // browser application reaches a LAN or Tailscale address.
    response->addHeader("Access-Control-Allow-Private-Network", "true");
    response->addHeader("Access-Control-Max-Age", "600");
    response->addHeader("Cache-Control", "no-store");
}

static void addHttpPhoneApiHeaders(AsyncWebServerResponse* response, const char* methods) {
    addApiCorsHeaders(response, methods);
    response->addHeader(
        "X-Protobuf-Schema",
        "https://raw.githubusercontent.com/meshtastic/protobufs/master/meshtastic/mesh.proto"
    );
}

static void sendHttpPhoneApiError(AsyncWebServerRequest* request, int status, const char* message) {
    AsyncWebServerResponse* response = request->beginResponse(status, "text/plain; charset=utf-8", message);
    addHttpPhoneApiHeaders(response, "PUT, GET, OPTIONS");
    request->send(response);
}

static void writeJsonBytes(Print& out, const char* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        const uint8_t c = static_cast<uint8_t>(data[i]);
        switch (c) {
            case '"': out.print("\\\""); break;
            case '\\': out.print("\\\\"); break;
            case '\b': out.print("\\b"); break;
            case '\f': out.print("\\f"); break;
            case '\n': out.print("\\n"); break;
            case '\r': out.print("\\r"); break;
            case '\t': out.print("\\t"); break;
            default:
                if (c < 0x20) {
                    out.print("\\u00");
                    out.print(hex[c >> 4]);
                    out.print(hex[c & 0x0f]);
                } else {
                    out.write(c);
                }
        }
    }
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
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MeshGate Control</title>
<link rel="icon" type="image/svg+xml" href="/assets/meshgate-logo.svg?v=)rawliteral" FIRMWARE_VERSION R"rawliteral(">
<style>
  :root { --bg:#0a0c10; --panel:#0f1318; --panel2:#111a24; --border:#1e2a38; --accent:#5eed97; --accent2:#ff6b35; --green:#39ff14; --yellow:#ffd60a; --red:#ff3860; --muted:#7d8daa; --text:#c8d6e5; --dim:#8ea2bb; --mono:"Cascadia Code","JetBrains Mono",Consolas,monospace; --sans:"Segoe UI","Exo 2",sans-serif; }
  * { box-sizing: border-box; }
  body { font-family:var(--sans); padding:16px; max-width:680px; margin:auto; background:var(--bg); color:var(--text); }
  .brand-link { display:block; width:100%; max-width:560px; margin:8px auto 6px; }
  .brand-logo { display:block; width:100%; height:auto; aspect-ratio:900/296; object-fit:contain; }
  .brand-subtitle { margin:0 0 20px; color:var(--dim); text-align:center; font:11px var(--mono); letter-spacing:2px; text-transform:uppercase; }
  .tab-nav { position:sticky; top:0; z-index:20; display:grid; grid-template-columns:repeat(4,1fr); gap:6px; margin:0 0 16px; padding:8px; border:1px solid var(--border); border-radius:4px; background:rgba(10,12,16,.96); box-shadow:0 8px 22px rgba(0,0,0,.28); backdrop-filter:blur(8px); }
  .tab-button { min-width:0; margin:0; padding:10px 6px; border-color:transparent; background:transparent; color:var(--dim); font-size:11px; text-transform:uppercase; }
  .tab-button:hover { color:var(--text); border-color:var(--border); opacity:1; }
  .tab-button.active { color:#07110b; background:var(--accent); border-color:var(--accent); }
  .tab-panel { display:none; }
  .tab-panel.active { display:block; animation:tab-in .16s ease-out; }
  @keyframes tab-in { from { opacity:.35; transform:translateY(3px); } to { opacity:1; transform:none; } }
  .card { background:linear-gradient(135deg,var(--panel),var(--panel2)); padding:18px; border:1px solid var(--border); border-radius:4px; box-shadow:0 8px 28px rgba(0,0,0,.25); margin-bottom:16px; }
  .card-header { display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid var(--border); padding-bottom:10px; margin-bottom:14px; }
  .card-title { color:var(--accent); font:700 14px var(--mono); letter-spacing:1px; text-transform:uppercase; display:flex; align-items:center; gap:8px; }
  .info-row { display: flex; justify-content: space-between; margin: 8px 0; font-size: 14px; }
  .info-label { color:var(--dim); font-weight:500; }
  .info-val { font-family:var(--mono); font-weight:600; color:var(--text); word-break:break-all; }
  .version-link { color:var(--accent); text-decoration:none; border-bottom:1px dotted var(--accent); }
  .version-link:hover { color:var(--green); border-bottom-color:var(--green); }
  label { font-weight:600; margin-top:12px; display:block; color:var(--text); font-size:13px; }
  input, select, textarea { width:100%; padding:10px; margin:6px 0 12px; border:1px solid var(--border)!important; border-radius:3px!important; color:var(--text); font:14px var(--mono); background:var(--bg)!important; }
  input:focus, select:focus, textarea:focus { outline:2px solid var(--accent); outline-offset:1px; }
  button { width:100%; padding:10px; margin:6px 0; border:1px solid var(--border); border-radius:3px; cursor:pointer; font:600 13px var(--mono); letter-spacing:.3px; transition:background-color .2s,opacity .2s,border-color .2s; }
  button:hover { opacity: 0.92; }
  button:disabled { opacity: 0.55 !important; cursor: not-allowed; }
  .btn-primary,.btn-success { background:var(--accent); color:#07110b; border-color:var(--accent); }
  .btn-secondary { background:transparent; color:var(--text); }
  .btn-warning { background:transparent; color:var(--yellow); border-color:var(--yellow); }
  .btn-danger { background:transparent; color:var(--red); border-color:var(--red); margin-top:8px; }
  .btn-edit { width:auto; padding:6px 14px; font-size:12px; margin:0; background:transparent; color:var(--accent); border:1px solid var(--accent); border-radius:3px; font-weight:600; }
  .btn-edit:hover { background:rgba(94,237,151,.08); opacity:1; }
  .btn-group { display: flex; gap: 8px; margin-top: 8px; }
  .btn-group button { flex: 1; }
  .password-field { position: relative; display: flex; align-items: center; margin-bottom: 12px; }
  .password-field input { padding-right: 65px; margin: 0; }
  .btn-toggle-pass { position:absolute; right:5px; width:auto; padding:6px 10px; font-size:12px; margin:0; background:var(--panel2); color:var(--accent); border-radius:3px; }
  .spinner { display:none; margin:10px 0; color:var(--accent); font-style:italic; font-size:13px; text-align:center; }
  .alert { display:none; padding:10px; border-radius:3px; font-size:13px; margin:10px 0; }
  .alert-success { background:rgba(57,255,20,.06); color:var(--green); border:1px solid var(--green); }
  .alert-error { background:rgba(255,56,96,.06); color:var(--red); border:1px solid var(--red); }
  .alert-warning { background:rgba(255,214,10,.06); color:var(--yellow); border:1px solid var(--yellow); }
  .alert-info { background:rgba(94,237,151,.05); color:var(--text); border:1px solid var(--accent); }
  .status-pill { display: inline-block; padding: 2px 8px; border-radius: 12px; font-size: 12px; font-weight: 600; }
  .status-pill.disabled { background:rgba(125,141,170,.12); color:var(--muted); }
  .status-pill.connected { background:rgba(57,255,20,.08); color:var(--green); }
  .status-pill.waiting { background:rgba(255,214,10,.08); color:var(--yellow); }
  .status-pill.connecting { background:rgba(94,237,151,.08); color:var(--accent); }
  .status-pill.error { background:rgba(255,56,96,.08); color:var(--red); }
  .progress-track { width:100%; background:var(--border); border-radius:2px; height:6px; overflow:hidden; margin-top:4px; }
  .progress-fill { height: 100%; border-radius: 4px; transition: width 0.3s ease, background-color 0.3s ease; }
  .progress-green { background-color:var(--green); }
  .progress-yellow { background-color:var(--yellow); }
  .progress-red { background-color:var(--red); }
  .queue-box { margin: 10px 0; }
  .queue-header { display:flex; justify-content:space-between; font-size:13px; font-weight:500; color:var(--text); }
  .queue-stats { font-family:var(--mono); font-size:12px; color:var(--accent); }
  .section-divider { border-top:1px solid var(--border); margin:14px 0 10px; padding-top:10px; font-weight:bold; font-size:14px; color:var(--text); }
  .wizard { display:grid; gap:10px; margin:10px 0 14px; }
  .wizard-step { display:grid; grid-template-columns:30px 1fr; gap:10px; padding:11px; border:1px solid var(--border); background:rgba(10,12,16,.45); border-radius:3px; }
  .wizard-number { width:28px; height:28px; display:flex; align-items:center; justify-content:center; border:1px solid var(--accent); border-radius:50%; color:var(--accent); font:700 13px var(--mono); }
  .wizard-title { color:var(--text); font:700 13px var(--mono); margin:1px 0 4px; }
  .wizard-copy { color:var(--dim); font-size:12px; line-height:1.45; }
  .wizard-actions { display:flex; flex-wrap:wrap; gap:6px; margin-top:8px; }
  .wizard-actions a,.wizard-actions button { width:auto; margin:0; padding:7px 10px; border:1px solid var(--accent); border-radius:3px; color:var(--accent); background:transparent; text-decoration:none; font:600 12px var(--mono); }
  .wizard-actions a:hover,.wizard-actions button:hover { background:rgba(94,237,151,.08); opacity:1; }
  .connection-box { display:none; margin-top:14px; padding:12px; border:1px solid var(--accent); background:rgba(94,237,151,.04); border-radius:3px; }
  .connection-value { display:block; margin:6px 0; color:var(--accent); font:700 14px var(--mono); word-break:break-all; }
  .tailscale-install { display:grid; grid-template-columns:112px 1fr; gap:12px; align-items:center; margin-top:10px; }
  .tailscale-qr { display:block; width:112px; height:112px; padding:7px; background:#fff; border-radius:5px; }
  .platform-links { display:grid; grid-template-columns:1fr 1fr; gap:7px; }
  .platform-link { display:flex; align-items:center; gap:7px; min-height:38px; padding:7px 9px; border:1px solid var(--border); border-radius:3px; color:var(--text); background:var(--panel2); text-decoration:none; font:600 11px var(--mono); }
  .platform-link:hover { border-color:var(--accent); color:var(--accent); }
  .platform-link svg { width:20px; height:20px; flex:0 0 20px; fill:none; stroke:currentColor; stroke-width:1.8; stroke-linecap:round; stroke-linejoin:round; }
  .remote-summary { display:none; margin-bottom:14px; padding:14px; border:1px solid var(--accent); background:rgba(94,237,151,.05); border-radius:3px; }
  .remote-summary h3 { margin:0 0 7px; color:var(--accent); font:700 15px var(--mono); letter-spacing:.4px; }
  .remote-address-row { display:grid; grid-template-columns:145px 1fr; gap:8px; align-items:baseline; margin:10px 0 4px; }
  .remote-address-label { color:var(--text); font:700 12px var(--mono); }
  .remote-address-value { color:var(--green); font:700 18px var(--mono); word-break:break-all; }
  .remote-summary p { margin:7px 0; color:var(--text); font-size:13px; line-height:1.5; }
  .remote-summary code { color:var(--accent); font:700 12px var(--mono); word-break:break-all; }
  .community-card { text-align:center; }
  .visitor-counter { display:none; max-width:100%; height:auto; margin:4px auto 12px; border:0; }
  .community-copy { margin:8px 0; color:var(--dim); font-size:12px; line-height:1.5; }
  .community-meta { margin:10px 0; color:var(--muted); font:10px/1.45 var(--mono); }
  .support-link { display:block; margin-top:12px; padding:10px 12px; border:1px solid var(--accent); border-radius:3px; color:#07110b; background:var(--accent); text-decoration:none; font:700 12px var(--mono); }
  .enrollment-state { margin-top:9px; padding:10px; border-left:3px solid var(--muted); background:rgba(125,141,170,.06); }
  .enrollment-state.active { border-color:var(--yellow); }
  .enrollment-state.success { border-color:var(--green); background:rgba(57,255,20,.05); }
  .enrollment-state.error { border-color:var(--red); background:rgba(255,56,96,.05); }
  .enrollment-state strong { display:block; color:var(--text); font:700 12px var(--mono); margin-bottom:3px; }
  .enrollment-state span { color:var(--dim); font-size:12px; line-height:1.4; }
  .btn-oneclick { margin-top:10px; padding:12px; font-size:14px; }
  @media (max-width:430px) {
    .tailscale-install { grid-template-columns:1fr; }
    .tailscale-qr { margin:auto; }
    .platform-links { grid-template-columns:1fr 1fr; }
    .remote-address-row { grid-template-columns:1fr; gap:2px; }
    .tab-nav { gap:4px; padding:6px; }
    .tab-button { padding:9px 3px; font-size:9px; letter-spacing:0; }
  }
  .card small { color:var(--dim)!important; }
  .footer { text-align:center; font:12px var(--mono); color:var(--muted); margin-top:24px; padding-bottom:20px; }
  .footer a { color:var(--accent); text-decoration:none; font-weight:600; }
  .footer a:hover { text-decoration: underline; }
</style>
</head>
<body>
  <a class="brand-link" href=")rawliteral" PROJECT_GITHUB_URL R"rawliteral(" target="_blank" rel="noopener noreferrer" aria-label="Open the MeshGate project on GitHub">
    <img class="brand-logo" src="/assets/meshgate-logo.svg?v=)rawliteral" FIRMWARE_VERSION R"rawliteral(" width="900" height="296" decoding="sync" alt="MeshGate">
  </a>
  <p class="brand-subtitle">Meshtastic remote access gateway</p>

  <nav class="tab-nav" role="tablist" aria-label="MeshGate settings">
    <button type="button" class="tab-button active" id="tab-button-connection" role="tab" aria-controls="tab-connection" aria-selected="true" onclick="selectTab('connection')">Connection</button>
    <button type="button" class="tab-button" id="tab-button-remote" role="tab" aria-controls="tab-remote" aria-selected="false" onclick="selectTab('remote')">Remote VPN</button>
    <button type="button" class="tab-button" id="tab-button-services" role="tab" aria-controls="tab-services" aria-selected="false" onclick="selectTab('services')">Services</button>
    <button type="button" class="tab-button" id="tab-button-system" role="tab" aria-controls="tab-system" aria-selected="false" onclick="selectTab('system')">System</button>
  </nav>

  <section class="tab-panel active" id="tab-connection" role="tabpanel" aria-labelledby="tab-button-connection">

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
      <input type="password" id="input-ble-pin" inputmode="numeric" pattern="[0-9]{6}" maxlength="6" placeholder="Leave empty to keep stored PIN">

      <div id="ble-edit-alert" class="alert"></div>

      <button type="button" id="btn-test-ble" class="btn-warning" onclick="testBleConnection()">⚡ Test Connection</button>
      <div class="btn-group">
        <button class="btn-success" id="btn-save-ble" onclick="saveBle()">Save Bluetooth</button>
        <button class="btn-secondary" onclick="toggleEditBle(false)">Cancel</button>
      </div>
    </div>
  </div>

  </section>
  <section class="tab-panel" id="tab-remote" role="tabpanel" aria-labelledby="tab-button-remote" hidden>

  <!-- Native Tailscale VPN Card -->
  <div class="card" id="card-tailscale">
    <div class="card-header">
      <div class="card-title">Tailscale (Remote VPN)</div>
      <button class="btn-edit" id="btn-edit-tailscale" onclick="toggleEditTailscale(true)">Setup</button>
    </div>
    <div id="tailscale-view">
      <div id="tailscale-connection-box" class="remote-summary">
        <h3>Tailscale IP</h3>
        <div class="remote-address-row">
          <span class="remote-address-label">Meshtastic App:</span>
          <span class="remote-address-value" id="view-tailscale-app-address"></span>
        </div>
        <p>In the Meshtastic app, add a <strong>Network / TCP</strong> device and enter the address above including port <strong>4403</strong>.</p>
        <div class="remote-address-row">
          <span class="remote-address-label">Meshtastic Client:</span>
          <span class="remote-address-value" id="view-tailscale-client-address"></span>
        </div>
        <p>In the browser-based Meshtastic Client, select <strong>HTTP</strong>, enter only the IP above without a port, and leave <strong>HTTPS disabled</strong>.</p>
        <div class="wizard-actions">
          <a id="link-tailscale-web-button" href="#">Open MeshGate</a>
          <button type="button" onclick="copyTailscaleAppAddress()">Copy App address</button>
          <button type="button" onclick="copyTailscaleClientAddress()">Copy Client IP</button>
        </div>
      </div>
      <div class="info-row">
        <span class="info-label">Tailnet:</span>
        <span class="info-val"><span id="view-tailscale-status" class="status-pill disabled">Disabled</span></span>
      </div>
      <div class="info-row"><span class="info-label">Device name:</span><span class="info-val" id="view-tailscale-name">meshgate</span></div>
      <div class="info-row"><span class="info-label">Remote path:</span><span class="info-val" id="view-tailscale-path">(Not connected)</span></div>
      <div class="info-row"><span class="info-label">Known peers:</span><span class="info-val" id="view-tailscale-peers">0</span></div>
      <div class="info-row"><span class="info-label">Enrollment:</span><span class="info-val" id="view-tailscale-enrollment">Browser login</span></div>
      <button class="btn-secondary" type="button" onclick="toggleEditTailscale(true)">Set up / reconnect Tailscale</button>
      <div id="tailscale-login-box" class="connection-box" style="display:none;">
        <div class="wizard-title">Approve MeshGate in Tailscale</div>
        <div class="wizard-copy">Sign in and approve this device. This MeshGate page remains open and detects the approval automatically.</div>
        <span class="connection-value" id="view-tailscale-auth-url"></span>
        <div class="wizard-actions">
          <a id="link-tailscale-auth" href="#" target="_blank" rel="noopener noreferrer">Add This Device to Tailscale</a>
          <button type="button" onclick="copyTailscaleLoginUrl()">Copy login link</button>
        </div>
      </div>
      <div id="tailscale-use-alert" class="alert"></div>
    </div>
    <div id="tailscale-edit" style="display:none;">
      <div class="alert alert-info" style="display:block; margin-bottom:12px;">
        Native Tailscale creates an outbound connection, so no dynamic DNS or router port forwarding is required. Once connected, this page and Meshtastic TCP port 4403 are reachable through the displayed 100.x address. Tailscale and classic WireGuard are mutually exclusive.
      </div>
      <div class="wizard">
        <div class="wizard-step">
          <div class="wizard-number">1</div>
          <div>
            <div class="wizard-title">Install Tailscale on your phone or computer</div>
            <div class="wizard-copy">The app is needed only on devices that should reach MeshGate remotely. Install it and sign in with your Tailscale account. No admin console or command line is required.</div>
            <div class="tailscale-install">
              <a href="https://tailscale.com/download" target="_blank" rel="noopener noreferrer" aria-label="Open Tailscale download page">
                <svg id="tailscale-download-qr" class="tailscale-qr" viewBox="0 0 33 33" role="img" aria-label="QR code for Tailscale download">
                  <rect width="33" height="33" rx="1" fill="#fff"/><path id="tailscale-download-qr-path" d="M2 2h7v1H2zM10 2h1v1H10zM12 2h1v1H12zM15 2h3v1H15zM20 2h3v1H20zM24 2h7v1H24zM2 3h1v1H2zM8 3h1v1H8zM10 3h3v1H10zM14 3h2v1H14zM18 3h3v1H18zM24 3h1v1H24zM30 3h1v1H30zM2 4h1v1H2zM4 4h3v1H4zM8 4h1v1H8zM11 4h3v1H11zM18 4h1v1H18zM22 4h1v1H22zM24 4h1v1H24zM26 4h3v1H26zM30 4h1v1H30zM2 5h1v1H2zM4 5h3v1H4zM8 5h1v1H8zM10 5h1v1H10zM14 5h1v1H14zM16 5h2v1H16zM19 5h1v1H19zM21 5h1v1H21zM24 5h1v1H24zM26 5h3v1H26zM30 5h1v1H30zM2 6h1v1H2zM4 6h3v1H4zM8 6h1v1H8zM12 6h2v1H12zM16 6h1v1H16zM18 6h1v1H18zM21 6h2v1H21zM24 6h1v1H24zM26 6h3v1H26zM30 6h1v1H30zM2 7h1v1H2zM8 7h1v1H8zM12 7h1v1H12zM15 7h1v1H15zM17 7h6v1H17zM24 7h1v1H24zM30 7h1v1H30zM2 8h7v1H2zM10 8h1v1H10zM12 8h1v1H12zM14 8h1v1H14zM16 8h1v1H16zM18 8h1v1H18zM20 8h1v1H20zM22 8h1v1H22zM24 8h7v1H24zM10 9h3v1H10zM17 9h4v1H17zM2 10h1v1H2zM4 10h2v1H4zM7 10h3v1H7zM15 10h1v1H15zM17 10h1v1H17zM19 10h2v1H19zM24 10h1v1H24zM27 10h1v1H27zM29 10h2v1H29zM7 11h1v1H7zM11 11h1v1H11zM14 11h4v1H14zM21 11h2v1H21zM24 11h3v1H24zM30 11h1v1H30zM2 12h1v1H2zM4 12h2v1H4zM7 12h2v1H7zM12 12h2v1H12zM15 12h1v1H15zM18 12h1v1H18zM20 12h1v1H20zM25 12h1v1H25zM28 12h2v1H28zM2 13h1v1H2zM5 13h1v1H5zM7 13h1v1H7zM12 13h1v1H12zM18 13h1v1H18zM20 13h1v1H20zM22 13h2v1H22zM25 13h2v1H25zM30 13h1v1H30zM3 14h2v1H3zM6 14h5v1H6zM12 14h1v1H12zM14 14h1v1H14zM16 14h2v1H16zM19 14h1v1H19zM21 14h1v1H21zM27 14h2v1H27zM2 15h1v1H2zM4 15h2v1H4zM7 15h1v1H7zM11 15h2v1H11zM16 15h1v1H16zM18 15h1v1H18zM21 15h1v1H21zM24 15h1v1H24zM28 15h3v1H28zM2 16h1v1H2zM4 16h1v1H4zM6 16h5v1H6zM13 16h1v1H13zM15 16h1v1H15zM17 16h6v1H17zM24 16h2v1H24zM28 16h3v1H28zM2 17h1v1H2zM5 17h1v1H5zM7 17h1v1H7zM10 17h1v1H10zM12 17h1v1H12zM14 17h1v1H14zM16 17h1v1H16zM18 17h1v1H18zM20 17h2v1H20zM24 17h1v1H24zM29 17h1v1H29zM3 18h1v1H3zM8 18h2v1H8zM11 18h1v1H11zM13 18h1v1H13zM16 18h3v1H16zM20 18h1v1H20zM22 18h2v1H22zM25 18h3v1H25zM29 18h1v1H29zM3 19h3v1H3zM9 19h6v1H9zM16 19h1v1H16zM18 19h2v1H18zM22 19h1v1H22zM25 19h1v1H25zM27 19h3v1H27zM2 20h1v1H2zM4 20h2v1H4zM8 20h1v1H8zM10 20h1v1H10zM15 20h4v1H15zM23 20h1v1H23zM25 20h2v1H25zM28 20h1v1H28zM4 21h3v1H4zM9 21h2v1H9zM13 21h4v1H13zM19 21h1v1H19zM21 21h1v1H21zM23 21h1v1H23zM25 21h1v1H25zM28 21h1v1H28zM3 22h2v1H3zM7 22h2v1H7zM11 22h1v1H11zM13 22h4v1H13zM18 22h3v1H18zM22 22h7v1H22zM10 23h1v1H10zM12 23h3v1H12zM17 23h4v1H17zM22 23h1v1H22zM26 23h5v1H26zM2 24h7v1H2zM10 24h1v1H10zM13 24h1v1H13zM16 24h2v1H16zM20 24h3v1H20zM24 24h1v1H24zM26 24h2v1H26zM29 24h1v1H29zM2 25h1v1H2zM8 25h1v1H8zM10 25h3v1H10zM15 25h3v1H15zM20 25h3v1H20zM26 25h2v1H26zM2 26h1v1H2zM4 26h3v1H4zM8 26h1v1H8zM12 26h1v1H12zM14 26h1v1H14zM19 26h1v1H19zM22 26h5v1H22zM28 26h1v1H28zM30 26h1v1H30zM2 27h1v1H2zM4 27h3v1H4zM8 27h1v1H8zM10 27h2v1H10zM15 27h1v1H15zM19 27h1v1H19zM21 27h3v1H21zM25 27h3v1H25zM29 27h1v1H29zM2 28h1v1H2zM4 28h3v1H4zM8 28h1v1H8zM10 28h2v1H10zM15 28h1v1H15zM18 28h1v1H18zM25 28h1v1H25zM28 28h1v1H28zM30 28h1v1H30zM2 29h1v1H2zM8 29h1v1H8zM16 29h3v1H16zM22 29h1v1H22zM24 29h1v1H24zM26 29h2v1H26zM29 29h1v1H29zM2 30h7v1H2zM10 30h1v1H10zM12 30h2v1H12zM15 30h2v1H15zM20 30h3v1H20zM27 30h1v1H27zM29 30h1v1H29z" fill="#111"/>
                </svg>
              </a>
              <div class="platform-links">
                <a class="platform-link" href="https://apps.apple.com/app/tailscale/id1470499037" target="_blank" rel="noopener noreferrer">
                  <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="3" width="18" height="18" rx="4"/><path d="M8 17 12 7l4 10M9.2 14h5.6M7 17h2M15 17h2"/></svg><span>iPhone / iPad<br>App Store</span>
                </a>
                <a class="platform-link" href="https://play.google.com/store/apps/details?id=com.tailscale.ipn" target="_blank" rel="noopener noreferrer">
                  <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M7 9h10v8a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2V9ZM9 9 7.5 6.5M15 9l1.5-2.5M9.5 12h.01M14.5 12h.01M5 10v6M19 10v6M9 19v2M15 19v2"/></svg><span>Android<br>Google Play</span>
                </a>
                <a class="platform-link" href="https://tailscale.com/download/windows" target="_blank" rel="noopener noreferrer">
                  <svg viewBox="0 0 24 24" aria-hidden="true"><path d="M3 5.5 10.5 4v7H3v-5.5ZM13 3.5 21 2v9h-8V3.5ZM3 13h7.5v7L3 18.5V13ZM13 13h8v9l-8-1.5V13Z"/></svg><span>Windows</span>
                </a>
                <a class="platform-link" href="https://tailscale.com/download/mac" target="_blank" rel="noopener noreferrer">
                  <svg viewBox="0 0 24 24" aria-hidden="true"><rect x="3" y="4" width="18" height="13" rx="2"/><path d="M8 21h8M12 17v4"/></svg><span>macOS</span>
                </a>
              </div>
            </div>
          </div>
        </div>
        <div class="wizard-step">
          <div class="wizard-number">2</div>
          <div>
            <div class="wizard-title">Add MeshGate to Tailscale</div>
            <div class="wizard-copy">Choose a name, then press the single button below. MeshGate saves the setting, restarts, obtains its one-time browser login and opens it automatically. No auth key is copied.</div>
            <label>Tailnet device name:</label>
            <input type="text" id="input-tailscale-name" maxlength="63" placeholder="meshgate">
            <input type="checkbox" id="input-tailscale-enabled" style="display:none;" checked>
            <button class="btn-success btn-oneclick" id="btn-save-tailscale" onclick="startTailscaleEnrollment()">Add This Device to Tailscale</button>
          </div>
        </div>
        <div class="wizard-step">
          <div class="wizard-number">3</div>
          <div>
            <div class="wizard-title">Sign in and finish</div>
            <div class="wizard-copy">Approve MeshGate on the Tailscale page. Keep this page open: it automatically changes to <strong>Your device has been added</strong> and displays the new 100.x address.</div>
            <div id="tailscale-enrollment-state" class="enrollment-state">
              <strong>Ready</strong><span>Install Tailscale first, then add this device.</span>
            </div>
          </div>
        </div>
      </div>
      <div id="tailscale-alert" class="alert"></div>
      <div class="btn-group">
        <button class="btn-secondary" onclick="toggleEditTailscale(false)">Cancel</button>
        <button class="btn-danger" id="btn-disable-tailscale" onclick="disableTailscale()">Disable Remote VPN</button>
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
      <div class="info-row"><span class="info-label">Remote LAN:</span><span class="info-val" id="view-wireguard-remote">(Not configured)</span></div>
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
      <label>WireGuard endpoint (host or public IP):</label>
      <input type="text" id="input-wireguard-endpoint" placeholder="vpn.example.net">
      <label>Endpoint UDP port:</label>
      <input type="number" id="input-wireguard-endpoint-port" min="1" max="65535" value="51820">
      <label>ESP32 private key:</label>
      <div class="password-field">
        <input type="password" id="input-wireguard-private-key" maxlength="44" placeholder="Leave empty to keep stored key">
        <button type="button" class="btn-toggle-pass" onclick="togglePassVisibility('input-wireguard-private-key', this)">Show</button>
      </div>
      <label>Remote peer public key:</label>
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

  </section>
  <section class="tab-panel" id="tab-services" role="tabpanel" aria-labelledby="tab-button-services" hidden>

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
        The factory standard is <strong>meshradar.pl:8883</strong>, TLS enabled, root <strong>msh</strong>.
        A complete custom broker configuration received from the radio replaces it.<br/>
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

  </section>
  <section class="tab-panel" id="tab-system" role="tabpanel" aria-labelledby="tab-button-system" hidden>

  <!-- Official one-click OTA Card -->
  <div class="card" id="card-ota">
    <div class="card-header">
      <div class="card-title">Firmware OTA Update</div>
      <span id="view-ota-status" class="status-pill waiting">Loading</span>
    </div>
    <div class="info-row"><span class="info-label">Firmware:</span><a class="info-val version-link" id="view-ota-version" href=")rawliteral" PROJECT_GITHUB_URL R"rawliteral(/blob/main/CHANGELOG.md" target="_blank" rel="noopener noreferrer" title="Open technical changelog">Loading...</a></div>
    <div class="info-row"><span class="info-label">Latest release:</span><a class="info-val version-link" id="view-ota-latest" href=")rawliteral" PROJECT_GITHUB_URL R"rawliteral(/blob/main/CHANGELOG.md" target="_blank" rel="noopener noreferrer" title="Open technical changelog">Not checked</a></div>
    <div class="info-row"><span class="info-label">Running slot:</span><span class="info-val" id="view-ota-running">Loading...</span></div>
    <div class="info-row"><span class="info-label">Update slot:</span><span class="info-val" id="view-ota-next">Loading...</span></div>
    <div class="alert alert-info" style="display:block; margin-bottom:12px;">
      MeshGate downloads only the latest official release from GitHub, verifies its board, size and SHA-256 checksum, installs it into the inactive slot and reboots. WiFi, BLE, VPN and MQTT settings are preserved.
    </div>
    <div class="progress-track" style="height:8px; margin:8px 0 12px;"><div id="ota-progress-bar" class="progress-fill progress-green" style="width:0%;"></div></div>
    <div id="ota-alert" class="alert"></div>
    <button class="btn-primary" id="btn-ota-update" onclick="startOfficialOta()">Check &amp; Install Latest Release</button>
  </div>

  <!-- System Actions Card -->
  <div class="card" id="card-system">
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

  <!-- Shared GitHub Pages and device visitor counter -->
  <div class="card community-card" id="card-community">
    <div class="card-header">
      <div class="card-title">MeshGate Community</div>
      <span class="status-pill connecting">Live</span>
    </div>
    <a href="https://flagcounter.me/details/kOa" target="_blank" rel="noopener noreferrer">
      <img id="visitor-counter-image" class="visitor-counter" alt="MeshGate visitors by country" referrerpolicy="no-referrer">
    </a>
    <p id="visitor-counter-status" class="community-copy">The country counter loads when MeshGate has normal internet access.</p>
    <p class="community-copy">GitHub Pages and MeshGate device visits share this counter, producing one combined country view.</p>
    <p class="community-meta">Privacy: loading the counter sends the visitor's public IP address to flagcounter.me for country-level geolocation and counting. It is never loaded in setup AP mode.</p>
    <a class="support-link" href="https://paypal.me/p1337/3EUR" target="_blank" rel="noopener noreferrer">Buy me a coffee · Donate €3 with PayPal</a>
  </div>

  </section>

  <div class="footer">
    Firmware Version: )rawliteral" FIRMWARE_VERSION R"rawliteral(<br>
    MeshGate extensions &copy; 2026 Robert Stefanowicz (p1337)<br>
    <a href=")rawliteral" PROJECT_GITHUB_URL R"rawliteral(" target="_blank">Project, source and MIT license on GitHub</a>
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
    let currentConfig = { wifi_ssid: '', wifi_has_pass: false, ble_mac: '', ble_has_pin: false };
    let pollInterval = null;
    let wifiPollInterval = null;
    let tailscaleEnrollmentWindow = null;
    let tailscaleEnrollmentTimer = null;
    let tailscaleEnrollmentActive = false;
    let tailscaleAuthOpenedUrl = '';
    let visitorCounterLoaded = false;
    let otaAutomaticCheckRetries = 0;

    const meshGateTabs = ['connection', 'remote', 'services', 'system'];

    function selectTab(tabName, updateHash = true) {
      const selected = meshGateTabs.includes(tabName) ? tabName : 'connection';
      meshGateTabs.forEach(name => {
        const panel = document.getElementById('tab-' + name);
        const button = document.getElementById('tab-button-' + name);
        const active = name === selected;
        if (panel) {
          panel.classList.toggle('active', active);
          panel.hidden = !active;
        }
        if (button) {
          button.classList.toggle('active', active);
          button.setAttribute('aria-selected', active ? 'true' : 'false');
          button.tabIndex = active ? 0 : -1;
        }
      });
      if (updateHash && window.history && window.history.replaceState) {
        window.history.replaceState(null, '', '#' + selected);
        window.scrollTo({ top: 0, behavior: 'smooth' });
      }
    }

    function loadVisitorCounter(wifiStatus) {
      const image = document.getElementById('visitor-counter-image');
      const status = document.getElementById('visitor-counter-status');
      if (!image || !status || visitorCounterLoaded) return;
      if (!wifiStatus || !wifiStatus.connected || wifiStatus.is_ap_mode) {
        status.innerText = 'The country counter is inactive in setup AP mode.';
        return;
      }
      visitorCounterLoaded = true;
      image.onload = function() {
        image.style.display = 'block';
        status.innerText = 'Live visitors by country — shared with the MeshGate GitHub Page.';
      };
      image.onerror = function() {
        status.innerText = 'Country counter is temporarily unavailable.';
      };
      image.src = 'https://flagcounter.me/kOa';
    }

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
          document.getElementById('view-ble-pin').innerText = cfg.ble_has_pin ? '•••••• (stored)' : '(Not set)';

          document.getElementById('input-wifi-ssid').value = cfg.wifi_ssid || '';
          document.getElementById('input-ble-name').value = cfg.ble_name || '';
          document.getElementById('input-ble-mac').value = cfg.ble_mac || '';
          document.getElementById('input-ble-pin').value = '';
          document.getElementById('sys-debug-logs').checked = !!cfg.debug_logs;

          // Tailscale fields. Interactive browser enrollment is keyless.
          document.getElementById('view-tailscale-name').innerText = cfg.tailscale_device_name || 'meshgate';
          document.getElementById('input-tailscale-enabled').checked = !!cfg.tailscale_enabled;
          document.getElementById('input-tailscale-name').value = cfg.tailscale_device_name || 'meshgate';
          document.getElementById('btn-edit-tailscale').disabled = !cfg.tailscale_supported;
          document.getElementById('btn-edit-tailscale').innerText = cfg.tailscale_enabled ? 'Edit' : 'Setup';
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
      const connectionBox = document.getElementById('tailscale-connection-box');
      const loginBox = document.getElementById('tailscale-login-box');
      return fetch('/tailscale_status')
        .then(r => r.json())
        .then(status => {
          document.getElementById('view-tailscale-peers').innerText = status.peer_count || 0;
          document.getElementById('view-tailscale-path').innerText = status.connected ? (status.derp_connected ? 'Ready (DERP relay)' : 'Connected (path negotiating)') : '(Not connected)';
          connectionBox.style.display = status.connected && status.vpn_ip ? 'block' : 'none';
          loginBox.style.display = !status.connected && status.auth_url ? 'block' : 'none';
          document.getElementById('view-tailscale-enrollment').innerText = status.connected ? 'Approved' : (status.auth_url ? 'Approval required' : 'Browser login');
          if (status.auth_url) {
            document.getElementById('view-tailscale-auth-url').innerText = status.auth_url;
            document.getElementById('link-tailscale-auth').href = status.auth_url;
            if (tailscaleEnrollmentActive && tailscaleAuthOpenedUrl !== status.auth_url) {
              tailscaleAuthOpenedUrl = status.auth_url;
              setTailscaleEnrollmentState('active', 'Tailscale login is ready', 'Sign in and approve MeshGate in the opened browser tab. This page keeps checking automatically.');
              if (tailscaleEnrollmentWindow && !tailscaleEnrollmentWindow.closed) {
                try { tailscaleEnrollmentWindow.location.replace(status.auth_url); } catch (e) {
                  tailscaleEnrollmentWindow.location.href = status.auth_url;
                }
              }
            }
          }
          if (status.connected && status.vpn_ip) {
            const webAddress = 'http://' + status.vpn_ip + '/';
            const appAddress = status.vpn_ip + ':4403';
            document.getElementById('view-tailscale-app-address').innerText = appAddress;
            document.getElementById('view-tailscale-client-address').innerText = status.vpn_ip;
            document.getElementById('link-tailscale-web-button').href = webAddress;
            if (tailscaleEnrollmentActive) {
              redirectTailscaleWaitingWindowToDevice();
              tailscaleEnrollmentActive = false;
              if (tailscaleEnrollmentTimer) {
                clearInterval(tailscaleEnrollmentTimer);
                tailscaleEnrollmentTimer = null;
              }
              setTailscaleEnrollmentState('success', 'Your device has been added', 'Remote access is connected. Your connection addresses are ready.');
              const button = document.getElementById('btn-save-tailscale');
              if (button) button.disabled = false;
              if (currentConfig) currentConfig.tailscale_enabled = true;
              setTimeout(() => {
                toggleEditTailscale(false);
                loadConfig();
              }, 1400);
            }
          }
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
          pill.className = tailscaleEnrollmentActive ? 'status-pill connecting' : 'status-pill error';
          pill.innerText = tailscaleEnrollmentActive ? 'Restarting / reconnecting' : 'Status unavailable';
          connectionBox.style.display = 'none';
          loginBox.style.display = 'none';
        });
    }

    function copyTailscaleText(text, label) {
      const alertId = document.getElementById('tailscale-edit').style.display === 'none' ? 'tailscale-use-alert' : 'tailscale-alert';
      const copied = () => showAlert(alertId, 'success', label + ' copied.');
      const failed = () => showAlert(alertId, 'error', 'Could not copy automatically. Select and copy it manually: ' + text);
      if (navigator.clipboard && window.isSecureContext) {
        navigator.clipboard.writeText(text).then(copied).catch(failed);
        return;
      }
      const field = document.createElement('textarea');
      field.value = text;
      field.setAttribute('readonly', '');
      field.style.position = 'fixed';
      field.style.opacity = '0';
      document.body.appendChild(field);
      field.select();
      let ok = false;
      try { ok = document.execCommand('copy'); } catch (e) {}
      document.body.removeChild(field);
      if (ok) copied(); else failed();
    }

    function copyTailscaleAppAddress() {
      const value = document.getElementById('view-tailscale-app-address').innerText;
      copyTailscaleText(value, 'Meshtastic App address');
    }

    function copyTailscaleClientAddress() {
      const value = document.getElementById('view-tailscale-client-address').innerText;
      copyTailscaleText(value, 'Meshtastic Client IP');
    }

    function copyTailscaleLoginUrl() {
      const value = document.getElementById('view-tailscale-auth-url').innerText;
      copyTailscaleText(value, 'Tailscale login link');
    }

    function toggleEditTailscale(edit) {
      if (edit && currentConfig && !currentConfig.tailscale_supported) {
        return;
      }
      if (edit && currentConfig) {
        document.getElementById('input-tailscale-enabled').checked = true;
        document.getElementById('input-tailscale-name').value = currentConfig.tailscale_device_name || 'meshgate';
        document.getElementById('btn-disable-tailscale').style.display = currentConfig.tailscale_enabled ? 'block' : 'none';
      }
      document.getElementById('tailscale-view').style.display = edit ? 'none' : 'block';
      document.getElementById('tailscale-edit').style.display = edit ? 'block' : 'none';
      document.getElementById('btn-edit-tailscale').style.display = edit ? 'none' : 'block';
      hideAlert('tailscale-alert');
    }

    function setTailscaleEnrollmentState(kind, title, message) {
      const state = document.getElementById('tailscale-enrollment-state');
      if (!state) return;
      state.className = 'enrollment-state' + (kind ? ' ' + kind : '');
      state.innerHTML = '<strong></strong><span></span>';
      state.querySelector('strong').innerText = title;
      state.querySelector('span').innerText = message;
    }

    function openTailscaleWaitingWindow() {
      try {
        const popup = window.open('about:blank', 'meshgate-tailscale-enrollment');
        if (!popup) return null;
        const statusUrl = window.location.origin + '/tailscale_status';
        const deviceUrl = window.location.origin + '/#remote';
        popup.document.title = 'MeshGate · Tailscale';
        popup.document.body.style.cssText = 'margin:0;padding:32px;background:#0a0c10;color:#c8d6e5;font:16px Segoe UI,sans-serif;';
        popup.document.body.innerHTML = '<h2 style="color:#5eed97">MeshGate is preparing Tailscale…</h2><p>The device is restarting and requesting its secure one-time login. Keep this tab open.</p>';
        const script = popup.document.createElement('script');
        script.text = '(function poll(){fetch(' + JSON.stringify(statusUrl) + ',{cache:"no-store"}).then(function(r){return r.json();}).then(function(s){if(s.connected&&s.vpn_ip){location.replace(' + JSON.stringify(deviceUrl) + ');return;}if(s.auth_url){location.replace(s.auth_url);return;}setTimeout(poll,2000);}).catch(function(){setTimeout(poll,2000);});})();';
        popup.document.head.appendChild(script);
        return popup;
      } catch (e) {
        return null;
      }
    }

    function redirectTailscaleWaitingWindowToDevice() {
      if (!tailscaleEnrollmentWindow || tailscaleEnrollmentWindow.closed) return;
      const deviceUrl = window.location.origin + '/#remote';
      try { tailscaleEnrollmentWindow.location.replace(deviceUrl); } catch (e) {
        tailscaleEnrollmentWindow.location.href = deviceUrl;
      }
    }

    function pollTailscaleEnrollment() {
      if (tailscaleEnrollmentTimer) clearInterval(tailscaleEnrollmentTimer);
      tailscaleEnrollmentTimer = setInterval(updateTailscaleStatus, 2500);
      updateTailscaleStatus();
    }

    function startTailscaleEnrollment() {
      const deviceName = document.getElementById('input-tailscale-name').value.trim().toLowerCase();

      if (!/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(deviceName)) {
        showAlert('tailscale-alert', 'error', 'Enter a valid lowercase device name.');
        return;
      }

      const button = document.getElementById('btn-save-tailscale');
      button.disabled = true;
      tailscaleEnrollmentActive = true;
      tailscaleAuthOpenedUrl = '';
      tailscaleEnrollmentWindow = openTailscaleWaitingWindow();
      setTailscaleEnrollmentState('active', 'Preparing MeshGate', 'Saving the device name and restarting. This page reconnects automatically.');
      fetch('/save_tailscale', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'enabled=true&device_name=' + encodeURIComponent(deviceName)
      })
      .then(async r => {
        const result = await r.json();
        if (!r.ok) throw new Error(result.error || 'Failed to save Tailscale');
        return result;
      })
      .then(() => {
        setTailscaleEnrollmentState('active', 'MeshGate is restarting', 'Please wait. The login opens automatically as soon as the device is back online.');
        pollTailscaleEnrollment();
        return fetch('/reboot', { method: 'POST' })
          .then(r => {
            if (!r.ok && r.status !== 202) throw new Error('Settings were saved, but reboot could not be scheduled. Use Reboot & Start Bridge below.');
          })
          .catch(() => {
            // A connection reset is expected when the restart wins the race
            // against the HTTP response. Enrollment polling determines the
            // real result after the device returns.
          });
      })
      .catch(err => {
        button.disabled = false;
        tailscaleEnrollmentActive = false;
        if (tailscaleEnrollmentTimer) {
          clearInterval(tailscaleEnrollmentTimer);
          tailscaleEnrollmentTimer = null;
        }
        setTailscaleEnrollmentState('error', 'Setup could not start', err.message || 'Network error saving Tailscale.');
        showAlert('tailscale-alert', 'error', err.message || 'Network error saving Tailscale.');
      });
    }

    function disableTailscale() {
      const deviceName = document.getElementById('input-tailscale-name').value.trim().toLowerCase() || 'meshgate';
      const button = document.getElementById('btn-disable-tailscale');
      button.disabled = true;
      fetch('/save_tailscale', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'enabled=false&device_name=' + encodeURIComponent(deviceName)
      })
      .then(r => {
        if (!r.ok) throw new Error('Could not disable Tailscale.');
        showAlert('tailscale-alert', 'success', 'Tailscale disabled. MeshGate is restarting.');
        return fetch('/reboot', { method: 'POST' });
      })
      .catch(err => {
        button.disabled = false;
        showAlert('tailscale-alert', 'error', err.message || 'Network error disabling Tailscale.');
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
            loadVisitorCounter(st.wifi);
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
          updateTailscaleStatus();
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
        document.getElementById('input-ble-pin').value = '';
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
      if (!pin && !(currentConfig && currentConfig.ble_has_pin)) {
        showAlert('ble-edit-alert', 'error', 'Enter the 6-digit BLE PIN.');
        return;
      }
      if (pin && !/^\d{6}$/.test(pin)) {
        showAlert('ble-edit-alert', 'error', 'The BLE PIN must contain exactly 6 digits.');
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
      if (!pin && !(currentConfig && currentConfig.ble_has_pin)) {
        showAlert('ble-edit-alert', 'error', 'Please enter the 6-digit pairing PIN.');
        return;
      }
      if (pin && !/^\d{6}$/.test(pin)) {
        showAlert('ble-edit-alert', 'error', 'The BLE PIN must contain exactly 6 digits.');
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

    function refreshOtaStatus() {
      return Promise.all([
        fetch('/api/ota', { cache: 'no-store' }).then(r => r.json()),
        fetch('/api/ota/official', { cache: 'no-store' }).then(r => r.json())
      ]).then(results => {
        const slots = results[0];
        const status = results[1];
        const versionLink = document.getElementById('view-ota-version');
        const latestLink = document.getElementById('view-ota-latest');
        versionLink.innerText = status.current_version || slots.firmware_version || '(Unknown)';
        latestLink.innerText = status.latest_version || 'Not checked';
        if (status.changelog_url) {
          versionLink.href = status.changelog_url;
          latestLink.href = status.changelog_url;
        }
        document.getElementById('view-ota-running').innerText = slots.running_partition || '(Unknown)';
        document.getElementById('view-ota-next').innerText = slots.next_partition || '(Unavailable)';
        updateOfficialOtaDisplay(status);
        return status;
      }).catch(() => {
        const pill = document.getElementById('view-ota-status');
        pill.className = 'status-pill error';
        pill.innerText = 'Unavailable';
        throw new Error('Could not read OTA status from the bridge.');
      });
    }

    function updateOfficialOtaDisplay(status) {
      const pill = document.getElementById('view-ota-status');
      const btn = document.getElementById('btn-ota-update');
      const bar = document.getElementById('ota-progress-bar');
      const labels = {
        idle: 'Ready', checking: 'Checking GitHub', current: 'Up to date',
        available: 'Update available',
        downloading: 'Downloading', verifying: 'Verifying', installing: 'Installing',
        rebooting: 'Rebooting', failed: 'Failed', unsupported: 'Unsupported'
      };
      pill.innerText = labels[status.state] || status.state || 'Unknown';
      pill.className = 'status-pill ' + (status.state === 'failed' ? 'error' :
        (status.state === 'available' || status.busy ? 'waiting' : 'connected'));
      btn.disabled = !!status.busy || !status.supported;
      if (status.latest_version) document.getElementById('view-ota-latest').innerText = status.latest_version;
      if (status.changelog_url) {
        document.getElementById('view-ota-version').href = status.changelog_url;
        document.getElementById('view-ota-latest').href = status.changelog_url;
      }
      btn.innerText = status.state === 'available' && status.latest_version
        ? 'Install ' + status.latest_version
        : 'Check & Install Latest Release';
      const total = Number(status.total_bytes || 0);
      const downloaded = Number(status.downloaded_bytes || 0);
      const percent = total > 0 ? Math.min(100, Math.round(downloaded * 100 / total)) : 0;
      bar.style.width = (status.state === 'verifying' || status.state === 'installing' || status.state === 'rebooting' ? 100 : percent) + '%';
      bar.className = 'progress-fill ' + (status.state === 'failed' ? 'progress-red' : 'progress-green');
      if (status.state === 'failed') showAlert('ota-alert', 'error', status.error || 'Official update failed.');
      if (status.state === 'current') showAlert('ota-alert', 'success', 'MeshGate is already running the latest official release.');
      if (status.state === 'available') showAlert('ota-alert', 'warning', 'A newer official MeshGate release is available. Select its version to read the technical changelog before installing.');
      if (status.state === 'rebooting') showAlert('ota-alert', 'success', 'Official release verified and installed. MeshGate is rebooting now.');
    }

    function pollOfficialOta() {
      fetch('/api/ota/official', { cache: 'no-store' }).then(r => r.json()).then(status => {
        updateOfficialOtaDisplay(status);
        if (status.busy) setTimeout(pollOfficialOta, 1000);
      }).catch(() => {
        showAlert('ota-alert', 'info', 'MeshGate may be rebooting. Reopen this page in about 15 seconds.');
      });
    }

    function startOfficialOta() {
      const btn = document.getElementById('btn-ota-update');
      btn.disabled = true;
      document.getElementById('ota-progress-bar').style.width = '0%';
      showAlert('ota-alert', 'info', 'Checking the official GitHub release...');
      fetch('/api/ota/official', { method: 'POST' })
        .then(r => r.json().then(body => ({ ok: r.ok, body: body })))
        .then(result => {
          if (!result.ok || !result.body.success) throw new Error(result.body.error || 'The update could not be started.');
          pollOfficialOta();
        })
        .catch(err => {
          btn.disabled = false;
          showAlert('ota-alert', 'error', err.message || 'The update could not be started.');
        });
    }

    function checkOfficialReleaseManually(event) {
      const latestLink = document.getElementById('view-ota-latest');
      if (latestLink.innerText.trim() !== 'Not checked') return;
      event.preventDefault();
      latestLink.innerText = 'Checking...';
      showAlert('ota-alert', 'info', 'Checking the latest official GitHub release...');
      fetch('/api/ota/official?check_only=true', { method: 'POST' })
        .then(r => r.json().then(body => ({ ok: r.ok, status: r.status, body: body })))
        .then(result => {
          if (result.status === 409) {
            pollOfficialOta();
            return;
          }
          if (!result.ok || !result.body.success) {
            throw new Error(result.body.error || 'The release check could not be started.');
          }
          pollOfficialOta();
        })
        .catch(err => {
          latestLink.innerText = 'Not checked';
          showAlert('ota-alert', 'error', err.message || 'The latest release could not be checked.');
        });
    }

    function checkOfficialReleaseOnConnect() {
      return Promise.all([
        refreshOtaStatus(),
        fetch('/status', { cache: 'no-store' }).then(r => r.json())
      ]).then(results => {
        const otaStatus = results[0];
        const runtimeStatus = results[1];
        const wifi = runtimeStatus && runtimeStatus.wifi;
        if (!otaStatus.supported || !wifi || !wifi.connected || wifi.is_ap_mode) return;
        if (otaStatus.busy) {
          pollOfficialOta();
          return;
        }
        showAlert('ota-alert', 'info', 'Checking the latest official GitHub release...');
        return fetch('/api/ota/official?check_only=true', { method: 'POST' })
          .then(r => r.json().then(body => ({ ok: r.ok, status: r.status, body: body })))
          .then(result => {
            if (result.status === 409) {
              pollOfficialOta();
              return;
            }
            if (result.status === 503) {
              if (otaAutomaticCheckRetries < 3) {
                otaAutomaticCheckRetries += 1;
                showAlert('ota-alert', 'info', 'Release check postponed while VPN and MQTT finish starting. Retrying automatically...');
                setTimeout(checkOfficialReleaseOnConnect, 15000);
              } else {
                showAlert('ota-alert', 'info', 'Automatic release check postponed. Use the update button later to check manually.');
              }
              return;
            }
            if (!result.ok || !result.body.success) {
              throw new Error(result.body.error || 'The release check could not be started.');
            }
            otaAutomaticCheckRetries = 0;
            pollOfficialOta();
          });
      }).catch(err => {
        showAlert('ota-alert', 'error', err.message || 'The latest release could not be checked.');
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
      document.getElementById('view-ota-latest').addEventListener('click', checkOfficialReleaseManually);
      selectTab(window.location.hash.replace('#', ''), false);
      loadConfig();
      updateStatus();
      checkOfficialReleaseOnConnect();
    };

    window.addEventListener('hashchange', function() {
      selectTab(window.location.hash.replace('#', ''), false);
    });
  </script>
  <img src="/p.gif" alt="" width="1" height="1" hidden>
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
    cfg.tailscale_device_name = getString("ts_name", "meshgate");
    cfg.tailscale_auth_key = getString("ts_auth");

    // MQTT preferences
    cfg.mqtt_enabled = getBool("mqtt_enabled", true);
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
    // Protect the constrained internal heap from parallel browser tabs and
    // network scanners. Recovery, OTA and PhoneAPI requests use independent
    // reserved slots, so normal UI traffic cannot lock out remote recovery.
    server.addMiddleware([](AsyncWebServerRequest* request, ArMiddlewareNext next) {
        if (!acquireWebRequest(request)) {
            sendWebBusy(request);
            return;
        }
        next();
    });

    // Meshtastic's browser transport uses naked protobuf messages over HTTP.
    // It intentionally shares the BLE PhoneAPI session with TCP 4403 without
    // applying the TCP stream header (0x94 0xC3 + length).
    server.on("/api/v1/toradio", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        bridge_http_phoneapi_mark_active();
        AsyncWebServerResponse* response = request->beginResponse(204);
        addHttpPhoneApiHeaders(response, "PUT, OPTIONS");
        request->send(response);
    });

    server.on(
        "/api/v1/toradio",
        HTTP_PUT,
        [](AsyncWebServerRequest* request) {
            bridge_http_phoneapi_mark_active();
            HttpPhoneApiBody* body = static_cast<HttpPhoneApiBody*>(request->_tempObject);
            request->_tempObject = nullptr;
            if (!body) {
                sendHttpPhoneApiError(
                    request,
                    request->contentLength() == 0 ? 400 : 500,
                    request->contentLength() == 0 ? "Empty ToRadio protobuf" : "Could not buffer ToRadio protobuf"
                );
                return;
            }
            if (body->invalid || body->expected == 0 || body->received != body->expected) {
                const bool tooLarge = body->expected > MESHTASTIC_MAX_PACKET_SIZE;
                free(body);
                sendHttpPhoneApiError(
                    request,
                    tooLarge ? 413 : 400,
                    tooLarge ? "ToRadio protobuf is too large" : "Incomplete ToRadio protobuf"
                );
                return;
            }
            if (!bridge_enqueue_http_to_ble(body->data, body->received)) {
                free(body);
                sendHttpPhoneApiError(request, 503, "Meshtastic radio queue is unavailable or full");
                return;
            }
            free(body);

            AsyncWebServerResponse* response = request->beginResponse(204);
            addHttpPhoneApiHeaders(response, "PUT, OPTIONS");
            request->send(response);
        },
        nullptr,
        [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
            if (index == 0) {
                HttpPhoneApiBody* body = static_cast<HttpPhoneApiBody*>(calloc(1, sizeof(HttpPhoneApiBody)));
                request->_tempObject = body;
                if (!body) {
                    return;
                }
                body->expected = total;
                body->invalid = total == 0 || total > MESHTASTIC_MAX_PACKET_SIZE;
            }

            HttpPhoneApiBody* body = static_cast<HttpPhoneApiBody*>(request->_tempObject);
            if (!body || body->invalid) {
                return;
            }
            if (index != body->received || index + len > body->expected || index + len > sizeof(body->data)) {
                body->invalid = true;
                return;
            }
            memcpy(body->data + index, data, len);
            body->received += len;
        }
    );

    server.on("/api/v1/fromradio", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        bridge_http_phoneapi_mark_active();
        AsyncWebServerResponse* response = request->beginResponse(204);
        addHttpPhoneApiHeaders(response, "GET, OPTIONS");
        request->send(response);
    });

    server.on("/api/v1/fromradio", HTTP_GET, [](AsyncWebServerRequest* request) {
        bridge_http_phoneapi_mark_active();
        const bool drainAll = request->hasParam("all") &&
                              request->getParam("all")->value().equalsIgnoreCase("true");
        AsyncResponseStream* response = request->beginResponseStream("application/x-protobuf");
        addHttpPhoneApiHeaders(response, "GET, OPTIONS");

        uint8_t packet[MESHTASTIC_MAX_PACKET_SIZE];
        size_t drained = 0;
        do {
            const size_t len = bridge_dequeue_http_from_ble(packet, sizeof(packet));
            if (len == 0) {
                break;
            }
            response->write(packet, len);
            ++drained;
        } while (drainAll && drained < HTTP_PHONEAPI_QUEUE_SIZE);

        request->send(response);
    });

    server.on("/api/mailbox", HTTP_OPTIONS, [](AsyncWebServerRequest* request) {
        AsyncWebServerResponse* response = request->beginResponse(204);
        addApiCorsHeaders(response, "GET, OPTIONS");
        request->send(response);
    });

    // Persistent decoded text-message history. The default response contains
    // the newest page in chronological order. `after` is an exclusive
    // sequence cursor for efficient polling; `limit` is capped at ring size.
    server.on("/api/mailbox", HTTP_GET, [](AsyncWebServerRequest* request) {
        const MessageMailboxStats stats = message_mailbox_get_stats();
        size_t limit = MESSAGE_MAILBOX_API_PAGE_MAX;
        if (request->hasParam("limit")) {
            const long requested = request->getParam("limit")->value().toInt();
            if (requested > 0) {
                limit = static_cast<size_t>(requested);
            }
        }
        if (limit > MESSAGE_MAILBOX_API_PAGE_MAX) {
            limit = MESSAGE_MAILBOX_API_PAGE_MAX;
        }

        const bool hasAfter = request->hasParam("after");
        uint32_t cursor = stats.oldest_sequence;
        if (hasAfter) {
            const String value = request->getParam("after")->value();
            bool digitsOnly = !value.isEmpty();
            for (size_t i = 0; digitsOnly && i < value.length(); ++i) {
                digitsOnly = value[i] >= '0' && value[i] <= '9';
            }
            char* endPtr = nullptr;
            const unsigned long parsed = strtoul(value.c_str(), &endPtr, 10);
            if (!digitsOnly || endPtr == value.c_str() || *endPtr != '\0') {
                AsyncWebServerResponse* error = request->beginResponse(
                    400,
                    "application/json",
                    "{\"success\":false,\"error\":\"after must be an unsigned sequence number\"}"
                );
                addApiCorsHeaders(error, "GET, OPTIONS");
                request->send(error);
                return;
            }
            cursor = parsed == ULONG_MAX ? stats.next_sequence : static_cast<uint32_t>(parsed + 1UL);
            if (cursor < stats.oldest_sequence) {
                cursor = stats.oldest_sequence;
            }
        } else if (stats.count > limit) {
            cursor = stats.next_sequence - static_cast<uint32_t>(limit);
        }

        AsyncResponseStream* response = request->beginResponseStream("application/json; charset=utf-8");
        addApiCorsHeaders(response, "GET, OPTIONS");
        response->print("{\"ready\":");
        response->print(stats.ready ? "true" : "false");
        response->print(",\"count\":"); response->print(stats.count);
        response->print(",\"capacity\":"); response->print(stats.capacity);
        response->print(",\"oldest_sequence\":"); response->print(stats.oldest_sequence);
        response->print(",\"next_sequence\":"); response->print(stats.next_sequence);
        response->print(",\"overwritten\":"); response->print(stats.overwritten);
        response->print(",\"duplicates\":"); response->print(stats.duplicates);
        response->print(",\"invalid\":"); response->print(stats.invalid);
        response->print(",\"messages\":[");

        bool first = true;
        size_t emitted = 0;
        MessageMailboxEntry entry = {};
        while (emitted < limit && message_mailbox_next_entry(&cursor, stats.next_sequence, &entry)) {
            if (!first) response->print(',');
            first = false;
            ++emitted;
            char fromId[11] = {};
            char toId[11] = {};
            snprintf(fromId, sizeof(fromId), "!%08lx", static_cast<unsigned long>(entry.from));
            snprintf(toId, sizeof(toId), "!%08lx", static_cast<unsigned long>(entry.to));
            response->print("{\"sequence\":"); response->print(entry.sequence);
            response->print(",\"from\":\""); response->print(fromId);
            response->print("\",\"to\":\""); response->print(toId);
            response->print("\",\"packet_id\":"); response->print(entry.packet_id);
            response->print(",\"channel\":"); response->print(entry.channel);
            response->print(",\"rx_time\":"); response->print(entry.rx_time);
            response->print(",\"rx_rssi\":"); response->print(entry.rx_rssi);
            response->print(",\"rx_snr\":"); response->print(entry.rx_snr_x4 / 4.0f, 2);
            response->print(",\"text\":\"");
            writeJsonBytes(*response, entry.text, entry.text_len);
            response->print("\"}");
        }
        response->print("]}");
        request->send(response);
    });

    // A fresh public image cannot contain a shared OTA secret. Permit one-time
    // or replacement key provisioning only from a client connected directly
    // to the local setup AP. The secret is write-only and mirrored in NVS.
    server.on("/api/ota/token", HTTP_POST, [](AsyncWebServerRequest* request) {
        const IPAddress clientIp = request->client()->remoteIP();
        const IPAddress apIp = WiFi.softAPIP();
        const bool directSetupClient = wifi_net_is_ap_mode() &&
            clientIp[0] == apIp[0] && clientIp[1] == apIp[1] && clientIp[2] == apIp[2];
        if (!directSetupClient) {
            request->send(403, "application/json", "{\"success\":false,\"error\":\"OTA keys can only be provisioned through the local setup access point\"}");
            return;
        }
        if (!request->hasParam("token", true)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Missing OTA key\"}");
            return;
        }

        String token = request->getParam("token", true)->value();
        token.trim();
        token.toLowerCase();
        if (!isValidOtaToken(token)) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"OTA key must contain exactly 64 hexadecimal characters\"}");
            return;
        }

        const size_t primaryWritten = preferences.putString("ota_token", token);
        const size_t backupWritten = backupPreferences.putString("ota_token", token);
        if (primaryWritten != token.length() || backupWritten != token.length()) {
            request->send(500, "application/json", "{\"success\":false,\"error\":\"Could not persist the OTA key redundantly\"}");
            return;
        }
        ota_update_set_token(token);
        log_i("[OTA] Authentication key provisioned from local setup AP");
        AsyncWebServerResponse* response = request->beginResponse(200, "application/json", "{\"success\":true}");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    // ESPAsyncWebServer treats a handler URI as matching its slash-delimited
    // child paths too. Register the specific /api/ota/token route before the
    // parent /api/ota upload handler so provisioning cannot be shadowed.
    ota_update_register(server, config_ui_get_ota_token());

    // Machine-readable diagnostics endpoint used for remote support. It mirrors
    // the bounded serial log ring without exposing pairing PIN values. A tiny
    // cursor is retained per request while response chunks are copied directly
    // from the ring; no second 16 KiB String is allocated.
    server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request){
        WebLogSnapshotCursor* cursor = static_cast<WebLogSnapshotCursor*>(calloc(1, sizeof(WebLogSnapshotCursor)));
        if (cursor == nullptr) {
            request->send(503, "text/plain; charset=utf-8", "Log stream unavailable: insufficient memory");
            return;
        }
        web_log_snapshot_begin(*cursor);
        request->_tempObject = cursor;
        AsyncWebServerResponse* response = request->beginResponse(
            "text/plain; charset=utf-8",
            cursor->size,
            [cursor](uint8_t* buffer, size_t capacity, size_t offset) {
                return web_log_snapshot_read(*cursor, offset, buffer, capacity);
            }
        );
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
    });

    server.on("/p.gif", HTTP_GET, [](AsyncWebServerRequest *request){
        static const uint8_t gif[] PROGMEM = {
            0x47,0x49,0x46,0x38,0x39,0x61,0x01,0x00,0x01,0x00,0x80,0x00,
            0x00,0x00,0x00,0x00,0xff,0xff,0xff,0x21,0xf9,0x04,0x01,0x00,
            0x00,0x00,0x00,0x2c,0x00,0x00,0x00,0x00,0x01,0x00,0x01,0x00,
            0x00,0x02,0x02,0x44,0x01,0x00,0x3b
        };
        device_presence_request_now();
        AsyncWebServerResponse* response = request->beginResponse(200, "image/gif", gif, sizeof(gif));
        response->addHeader("Cache-Control", "no-store, max-age=0");
        request->send(response);
    });

    // Serve the official logo from the same flash bytes used by the repository
    // and Web Flasher. No external request is made in setup AP mode.
    server.on("/assets/meshgate-logo.svg", HTTP_GET, [](AsyncWebServerRequest *request){
        const size_t logoSize = static_cast<size_t>(meshgate_logo_svg_end - meshgate_logo_svg_start);
        AsyncWebServerResponse* response = request->beginResponse(
            200,
            "image/svg+xml; charset=utf-8",
            meshgate_logo_svg_start,
            logoSize
        );
        response->addHeader("Cache-Control", "public, max-age=31536000, immutable");
        request->send(response);
    });

    // Modern browsers receive a pre-compressed flash image. This cuts the
    // control page transfer over constrained DERP paths without allocating a
    // compression buffer at runtime. Non-gzip clients retain the raw fallback.
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        log_d("[WebUI] GET / requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        const bool acceptsGzip = request->hasHeader("Accept-Encoding") &&
                                 request->header("Accept-Encoding").indexOf("gzip") >= 0;
        if (acceptsGzip) {
            const size_t gzipSize = static_cast<size_t>(index_html_gz_end - index_html_gz_start);
            AsyncWebServerResponse* response = request->beginResponse(
                200,
                "text/html; charset=utf-8",
                index_html_gz_start,
                gzipSize
            );
            response->addHeader("Content-Encoding", "gzip");
            response->addHeader("Vary", "Accept-Encoding");
            response->addHeader("Cache-Control", "no-store");
            request->send(response);
            return;
        }
        AsyncWebServerResponse* response = request->beginResponse(
            200,
            "text/html; charset=utf-8",
            (const uint8_t*)index_html,
            sizeof(index_html) - 1
        );
        response->addHeader("Vary", "Accept-Encoding");
        response->addHeader("Cache-Control", "no-store");
        request->send(response);
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
        response->print("\",\"ble_has_pin\":");
        response->print(cfg.ble_pin.isEmpty() ? "false" : "true");
        response->print(",\"debug_logs\":");
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
        response->print("\",\"auth_url\":\"");
        utils_escape_json_print(*response, tailscale_net_auth_url());
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

    // Save native Tailscale configuration. Interactive enrollment obtains a
    // one-time browser URL from the control plane; no auth/API key is needed.
    // A legacy stored auth key is preserved but no longer transmitted.
    server.on("/save_tailscale", HTTP_POST, [](AsyncWebServerRequest *request){
        const bool enabled = request->hasParam("enabled", true) && request->getParam("enabled", true)->value() == "true";
        String deviceName = request->hasParam("device_name", true) ? request->getParam("device_name", true)->value() : "";
        deviceName.trim();

        bool validName = !deviceName.isEmpty() && deviceName.length() <= 63;
        for (size_t i = 0; validName && i < deviceName.length(); ++i) {
            const char c = deviceName[i];
            validName = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        }
        if (validName) {
            validName = deviceName[0] != '-' && deviceName[deviceName.length() - 1] != '-';
        }
        if (enabled && !validName) {
            request->send(400, "application/json", "{\"success\":false,\"error\":\"Tailscale requires a valid lowercase hostname.\"}");
            return;
        }

        preferences.putBool("ts_enabled", enabled);
        preferences.putString("ts_name", deviceName);
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
            if (pin.isEmpty()) {
                pin = config_ui_load().ble_pin;
            }
            if (!isValidBlePin(pin)) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"BLE PIN must contain exactly 6 digits.\"}");
                return;
            }
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
            if (pin.isEmpty()) {
                pin = config_ui_load().ble_pin;
            }
            if (mac.isEmpty() || !isValidBlePin(pin)) {
                request->send(400, "application/json", "{\"success\":false,\"error\":\"A target MAC and a 6-digit BLE PIN are required.\"}");
                return;
            }
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
        if (ota_update_is_busy()) {
            request->send(409, "application/json", "{\"success\":false,\"message\":\"Reboot blocked while OTA update is active\"}");
            return;
        }
        if (!scheduleRestart()) {
            request->send(503, "application/json", "{\"success\":false,\"message\":\"Unable to schedule reboot\"}");
            return;
        }
        request->send(202, "application/json", "{\"success\":true,\"message\":\"Reboot scheduled\"}");
    });

    server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *request){
        log_i("[API] Remote reboot requested from %s", request->client()->remoteIP().toString().c_str());
        if (ota_update_is_busy()) {
            request->send(409, "application/json", "{\"success\":false,\"message\":\"Reboot blocked while OTA update is active\"}");
            return;
        }
        if (!scheduleRestart()) {
            request->send(503, "application/json", "{\"success\":false,\"message\":\"Unable to schedule reboot\"}");
            return;
        }
        request->send(202, "application/json", "{\"success\":true,\"message\":\"Reboot scheduled\"}");
    });

    // Reset all settings and reboot
    server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request){
        log_d("[WebUI] POST /reset requested (Client: %s)", request->client()->remoteIP().toString().c_str());
        if (ota_update_is_busy()) {
            request->send(409, "application/json", "{\"success\":false,\"message\":\"Factory reset blocked while OTA update is active\"}");
            return;
        }
        tailscale_net_factory_reset();
        preferences.clear();
        backupPreferences.clear();
        message_mailbox_clear();
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
