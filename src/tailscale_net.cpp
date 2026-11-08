#include "tailscale_net.h"
#include "web_log.h"

#include <atomic>

#ifdef ESP_IDF5_MIGRATION_BUILD
#include <microlink.h>
#endif

namespace {

std::atomic<bool> s_enabled{false};
std::atomic<int> s_state{0};
std::atomic<uint32_t> s_vpnIp{0};
std::atomic<int> s_peerCount{0};
String s_authKey;
String s_deviceName;

#ifdef ESP_IDF5_MIGRATION_BUILD
microlink_t* s_microlink = nullptr;

void stateChanged(microlink_t* handle, microlink_state_t state, void*) {
    s_state.store(static_cast<int>(state), std::memory_order_relaxed);
    if (state == ML_STATE_CONNECTED) {
        const uint32_t ip = microlink_get_vpn_ip(handle);
        s_vpnIp.store(ip, std::memory_order_relaxed);
        char ipText[16] = {};
        microlink_ip_to_str(ip, ipText);
        log_i("[Tailscale] Connected as %s (%s)", s_deviceName.c_str(), ipText);
    } else {
        log_i("[Tailscale] State changed to %d", static_cast<int>(state));
    }
}

void peerChanged(microlink_t* handle, const microlink_peer_info_t*, void*) {
    s_peerCount.store(microlink_get_peer_count(handle), std::memory_order_relaxed);
}
#endif

} // namespace

void tailscale_net_start(const BridgeConfig& cfg) {
    s_enabled.store(cfg.tailscale_enabled, std::memory_order_relaxed);
    if (!cfg.tailscale_enabled) {
        log_i("[Tailscale] Disabled");
        return;
    }

#ifndef ESP_IDF5_MIGRATION_BUILD
    log_e("[Tailscale] Requires the arduino_nano_esp32_idf5 firmware");
    s_state.store(6, std::memory_order_relaxed);
#else
    if (cfg.tailscale_auth_key.isEmpty()) {
        log_e("[Tailscale] Enabled but no auth key is stored");
        s_state.store(ML_STATE_ERROR, std::memory_order_relaxed);
        return;
    }

    // MicroLink stores pointers to these values, so keep private static copies
    // for the complete lifetime of the asynchronous client.
    s_authKey = cfg.tailscale_auth_key;
    s_deviceName = cfg.tailscale_device_name;
    if (s_deviceName.isEmpty()) {
        s_deviceName = microlink_default_device_name();
    }

    microlink_config_t config = {};
    config.auth_key = s_authKey.c_str();
    config.device_name = s_deviceName.c_str();
    config.enable_derp = true;
    config.enable_stun = true;
    config.enable_disco = true;
    config.max_peers = 16;

    s_microlink = microlink_init(&config);
    if (s_microlink == nullptr) {
        log_e("[Tailscale] MicroLink initialization failed");
        s_state.store(ML_STATE_ERROR, std::memory_order_relaxed);
        return;
    }
    microlink_set_state_callback(s_microlink, stateChanged, nullptr);
    microlink_set_peer_callback(s_microlink, peerChanged, nullptr);
    const esp_err_t result = microlink_start(s_microlink);
    if (result != ESP_OK) {
        log_e("[Tailscale] Start failed: %s", esp_err_to_name(result));
        s_state.store(ML_STATE_ERROR, std::memory_order_relaxed);
    } else {
        log_i("[Tailscale] Starting as %s", s_deviceName.c_str());
    }
#endif
}

bool tailscale_net_is_enabled() {
    return s_enabled.load(std::memory_order_relaxed);
}

bool tailscale_net_is_connected() {
#ifdef ESP_IDF5_MIGRATION_BUILD
    return s_microlink != nullptr && microlink_is_connected(s_microlink);
#else
    return false;
#endif
}

String tailscale_net_state_name() {
    static const char* names[] = {
        "idle", "wifi_wait", "connecting", "registering",
        "connected", "reconnecting", "error"
    };
    const int state = s_state.load(std::memory_order_relaxed);
    return (state >= 0 && state < 7) ? String(names[state]) : String("unknown");
}

String tailscale_net_vpn_ip() {
    const uint32_t ip = s_vpnIp.load(std::memory_order_relaxed);
    if (ip == 0) return String();
#ifdef ESP_IDF5_MIGRATION_BUILD
    char ipText[16] = {};
    microlink_ip_to_str(ip, ipText);
    return String(ipText);
#else
    return String();
#endif
}

int tailscale_net_peer_count() {
#ifdef ESP_IDF5_MIGRATION_BUILD
    if (s_microlink != nullptr) {
        return microlink_get_peer_count(s_microlink);
    }
#endif
    return s_peerCount.load(std::memory_order_relaxed);
}

void tailscale_net_factory_reset() {
#ifdef ESP_IDF5_MIGRATION_BUILD
    if (s_microlink != nullptr) {
        microlink_stop(s_microlink);
        microlink_destroy(s_microlink);
        s_microlink = nullptr;
    }
    microlink_factory_reset();
#endif
    s_enabled.store(false, std::memory_order_relaxed);
    s_state.store(0, std::memory_order_relaxed);
    s_vpnIp.store(0, std::memory_order_relaxed);
    s_peerCount.store(0, std::memory_order_relaxed);
    s_authKey = "";
    s_deviceName = "";
}
