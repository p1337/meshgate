/*
 * WireGuard network integration for the Meshtastic BLE/TCP bridge.
 *
 * The low-level WireGuard interface is provided by WireGuard-ESP32, licensed
 * under BSD-3-Clause. Its public Arduino wrapper currently omits the optional
 * WireGuard preshared key, so this integration uses the library's public
 * wireguardif API directly and supplies that key when configured.
 */

#include "wireguard_net.h"
#include "web_log.h"

#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/ip.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

extern "C" {
#include "wireguard.h"
#include "wireguardif.h"
#include "wireguard-platform.h"
}
namespace {

constexpr uint32_t VALID_TIME_THRESHOLD = 1700000000UL;
constexpr uint16_t WIREGUARD_KEEPALIVE_SECONDS = 25;

BridgeConfig s_config;
std::atomic<bool> s_enabled{false};
std::atomic<bool> s_initialized{false};
std::atomic<bool> s_pausedForOta{false};
TaskHandle_t s_wireguardTask = nullptr;

struct netif s_wg_netif_storage = {};
struct netif* s_wg_netif = nullptr;
uint8_t s_peer_index = WIREGUARDIF_INVALID_INDEX;
uint8_t s_preshared_key[WIREGUARD_SESSION_KEY_LEN] = {};

bool parseCidr(const String& cidr, IPAddress& network, IPAddress& mask) {
    int slash = cidr.indexOf('/');
    if (slash <= 0 || slash >= static_cast<int>(cidr.length()) - 1) {
        return false;
    }

    IPAddress suppliedNetwork;
    if (!suppliedNetwork.fromString(cidr.substring(0, slash))) {
        return false;
    }

    int prefix = cidr.substring(slash + 1).toInt();
    if (prefix < 1 || prefix > 32) {
        return false;
    }

    uint32_t maskHost = prefix == 32 ? 0xFFFFFFFFUL : (0xFFFFFFFFUL << (32 - prefix));
    mask = IPAddress(
        static_cast<uint8_t>(maskHost >> 24),
        static_cast<uint8_t>(maskHost >> 16),
        static_cast<uint8_t>(maskHost >> 8),
        static_cast<uint8_t>(maskHost));

    uint32_t supplied = (static_cast<uint32_t>(suppliedNetwork[0]) << 24) |
                        (static_cast<uint32_t>(suppliedNetwork[1]) << 16) |
                        (static_cast<uint32_t>(suppliedNetwork[2]) << 8) |
                        static_cast<uint32_t>(suppliedNetwork[3]);
    uint32_t canonical = supplied & maskHost;
    network = IPAddress(
        static_cast<uint8_t>(canonical >> 24),
        static_cast<uint8_t>(canonical >> 16),
        static_cast<uint8_t>(canonical >> 8),
        static_cast<uint8_t>(canonical));
    return true;
}

bool addressIsInNetwork(const IPAddress& address, const IPAddress& network, const IPAddress& mask) {
    for (int i = 0; i < 4; ++i) {
        if ((address[i] & mask[i]) != (network[i] & mask[i])) {
            return false;
        }
    }
    return true;
}

bool resolveEndpoint(const String& hostname, ip_addr_t& endpointIp) {
    struct addrinfo hint = {};
    hint.ai_family = AF_INET;
    struct addrinfo* result = nullptr;

    for (int attempt = 0; attempt < 5; ++attempt) {
        if (lwip_getaddrinfo(hostname.c_str(), nullptr, &hint, &result) == 0 && result != nullptr) {
            struct in_addr address = reinterpret_cast<struct sockaddr_in*>(result->ai_addr)->sin_addr;
            inet_addr_to_ip4addr(ip_2_ip4(&endpointIp), &address);
            lwip_freeaddrinfo(result);
            return true;
        }
        if (result != nullptr) {
            lwip_freeaddrinfo(result);
            result = nullptr;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    return false;
}

void shutdownTunnel() {
    if (s_wg_netif == nullptr) {
        return;
    }
    if (s_peer_index != WIREGUARDIF_INVALID_INDEX) {
        wireguardif_disconnect(s_wg_netif, s_peer_index);
        wireguardif_remove_peer(s_wg_netif, s_peer_index);
        s_peer_index = WIREGUARDIF_INVALID_INDEX;
    }
    netif_set_down(s_wg_netif);
    netif_remove(s_wg_netif);
    wireguardif_shutdown(s_wg_netif);
    s_wg_netif = nullptr;
    s_initialized.store(false);
}

bool initializeTunnel() {
    IPAddress localIp;
    IPAddress remoteNetwork;
    IPAddress remoteMask;
    if (!localIp.fromString(s_config.wireguard_local_ip) ||
        !parseCidr(s_config.wireguard_remote_cidr, remoteNetwork, remoteMask)) {
        log_e("[WireGuard] Invalid local IP or remote CIDR");
        return false;
    }

    // lwIP selects an interface by its IP/netmask. Keeping the tunnel address
    // inside the configured remote subnet lets replies to the remote LAN use
    // WireGuard while ordinary internet traffic continues over local WiFi.
    if (!addressIsInNetwork(localIp, remoteNetwork, remoteMask)) {
        log_e("[WireGuard] Tunnel IP %s is not inside remote subnet %s",
              s_config.wireguard_local_ip.c_str(), s_config.wireguard_remote_cidr.c_str());
        return false;
    }

    ip_addr_t endpointIp = IPADDR4_INIT_BYTES(0, 0, 0, 0);
    if (!resolveEndpoint(s_config.wireguard_endpoint, endpointIp)) {
        log_e("[WireGuard] Could not resolve endpoint %s", s_config.wireguard_endpoint.c_str());
        return false;
    }

    bool hasPresharedKey = !s_config.wireguard_preshared_key.isEmpty();
    if (hasPresharedKey) {
        size_t decodedLength = sizeof(s_preshared_key);
        if (!wireguard_base64_decode(s_config.wireguard_preshared_key.c_str(), s_preshared_key, &decodedLength) ||
            decodedLength != sizeof(s_preshared_key)) {
            log_e("[WireGuard] Invalid preshared key");
            return false;
        }
    }

    struct wireguardif_init_data interfaceConfig = {};
    interfaceConfig.private_key = s_config.wireguard_private_key.c_str();
    interfaceConfig.listen_port = 0;
    interfaceConfig.bind_netif = netif_default;

    ip_addr_t ipAddress = IPADDR4_INIT(static_cast<uint32_t>(localIp));
    ip_addr_t netmask = IPADDR4_INIT(static_cast<uint32_t>(remoteMask));
    ip_addr_t gateway = IPADDR4_INIT_BYTES(0, 0, 0, 0);

    memset(&s_wg_netif_storage, 0, sizeof(s_wg_netif_storage));
    s_wg_netif = netif_add(&s_wg_netif_storage,
                           ip_2_ip4(&ipAddress),
                           ip_2_ip4(&netmask),
                           ip_2_ip4(&gateway),
                           &interfaceConfig,
                           &wireguardif_init,
                           &ip_input);
    if (s_wg_netif == nullptr) {
        log_e("[WireGuard] Could not create tunnel interface");
        return false;
    }
    netif_set_up(s_wg_netif);

    struct wireguardif_peer peer;
    wireguardif_peer_init(&peer);
    peer.public_key = s_config.wireguard_public_key.c_str();
    peer.preshared_key = hasPresharedKey ? s_preshared_key : nullptr;
    peer.allowed_ip = IPADDR4_INIT(static_cast<uint32_t>(remoteNetwork));
    peer.allowed_mask = IPADDR4_INIT(static_cast<uint32_t>(remoteMask));
    peer.endpoint_ip = endpointIp;
    peer.endport_port = s_config.wireguard_endpoint_port;
    peer.keep_alive = WIREGUARD_KEEPALIVE_SECONDS;

#ifndef ESP_IDF5_MIGRATION_BUILD
    wireguard_platform_init();
#endif
    err_t addResult = wireguardif_add_peer(s_wg_netif, &peer, &s_peer_index);
    if (addResult != ERR_OK || s_peer_index == WIREGUARDIF_INVALID_INDEX) {
        log_e("[WireGuard] Could not add peer (error %d)", addResult);
        shutdownTunnel();
        return false;
    }

    if (wireguardif_connect(s_wg_netif, s_peer_index) != ERR_OK) {
        log_e("[WireGuard] Could not start peer handshake");
        shutdownTunnel();
        return false;
    }

    s_initialized.store(true);
    log_i("[WireGuard] Tunnel started: %s, remote network %s, endpoint %s:%u",
          s_config.wireguard_local_ip.c_str(),
          s_config.wireguard_remote_cidr.c_str(),
          s_config.wireguard_endpoint.c_str(),
          s_config.wireguard_endpoint_port);
    return true;
}

void wireguardTask(void*) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");

    for (;;) {
        if (s_pausedForOta.load(std::memory_order_acquire)) {
            s_wireguardTask = nullptr;
            vTaskDelete(nullptr);
            return;
        }
        if (WiFi.status() != WL_CONNECTED) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
            continue;
        }

        time_t now = time(nullptr);
        if (now < static_cast<time_t>(VALID_TIME_THRESHOLD)) {
            log_i("[WireGuard] Waiting for NTP time synchronization...");
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
            continue;
        }

        if (initializeTunnel()) {
            s_wireguardTask = nullptr;
            vTaskDelete(nullptr);
            return;
        }

        // DNS, internet or endpoint may be temporarily unavailable. Retry
        // without blocking the BLE/TCP bridge.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
    }
}

void shutdownTunnelOnTcpip(void*) {
    shutdownTunnel();
}

} // namespace

void wireguard_net_start(const BridgeConfig& cfg) {
    s_config = cfg;
    s_pausedForOta.store(false, std::memory_order_release);
    s_enabled.store(cfg.wireguard_enabled);
    if (!cfg.wireguard_enabled) {
        log_i("[WireGuard] Disabled");
        return;
    }

    if (cfg.wireguard_local_ip.isEmpty() || cfg.wireguard_remote_cidr.isEmpty() ||
        cfg.wireguard_endpoint.isEmpty() || cfg.wireguard_endpoint_port == 0 ||
        cfg.wireguard_private_key.isEmpty() || cfg.wireguard_public_key.isEmpty()) {
        log_e("[WireGuard] Enabled but configuration is incomplete");
        return;
    }

    if (s_initialized.load(std::memory_order_acquire) || s_wireguardTask != nullptr) return;
    xTaskCreatePinnedToCore(wireguardTask, "wireguard_task", 8192, nullptr, 1, &s_wireguardTask, 0);
}

bool wireguard_net_pause_for_ota() {
    if (!s_enabled.load(std::memory_order_acquire)) return true;
    bool expected = false;
    if (!s_pausedForOta.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return s_wireguardTask == nullptr && !s_initialized.load(std::memory_order_acquire);
    }
    log_i("[OTA] Pausing classic WireGuard service...");
    TaskHandle_t task = s_wireguardTask;
    if (task != nullptr) xTaskNotifyGive(task);
    for (uint16_t attempt = 0; attempt < 1200 && s_wireguardTask != nullptr; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    bool stopped = s_wireguardTask == nullptr;
    if (s_initialized.load(std::memory_order_acquire)) {
        const err_t result = tcpip_callback_wait(shutdownTunnelOnTcpip, nullptr);
        if (result != ERR_OK) {
            log_w("[OTA] WireGuard shutdown callback failed (%d)", result);
            stopped = false;
        }
    }
    stopped = stopped && !s_initialized.load(std::memory_order_acquire);
    if (!stopped) log_e("[OTA] Classic WireGuard did not stop cleanly");
    return stopped;
}

void wireguard_net_resume_after_ota() {
    bool expected = true;
    if (!s_pausedForOta.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) return;
    log_i("[OTA] Restoring classic WireGuard service after failed update...");
    wireguard_net_start(s_config);
}

bool wireguard_net_is_enabled() {
    return s_enabled.load();
}

bool wireguard_net_is_initialized() {
    return s_initialized.load();
}

bool wireguard_net_is_connected() {
    if (!s_initialized.load() || s_wg_netif == nullptr || s_peer_index == WIREGUARDIF_INVALID_INDEX) {
        return false;
    }
    return wireguardif_peer_is_up(s_wg_netif, s_peer_index, nullptr, nullptr) == ERR_OK;
}

