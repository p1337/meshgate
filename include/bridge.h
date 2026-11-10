#pragma once
#include <Arduino.h>

void bridge_init(const String& ble_name, const String& ble_mac, uint32_t ble_pin);
void bridge_start();
bool bridge_is_ble_connected();
bool bridge_enqueue_mqtt_to_ble(const uint8_t* data, size_t len);
void bridge_clear_mqtt_to_ble_queue();

// Meshtastic HTTP PhoneAPI transport. HTTP carries one naked protobuf per
// request/response, unlike TCP which adds the 0x94 0xC3 stream frame.
void bridge_http_phoneapi_mark_active();
bool bridge_http_phoneapi_is_active();
bool bridge_enqueue_http_to_ble(const uint8_t* data, size_t len);
size_t bridge_dequeue_http_from_ble(uint8_t* data, size_t capacity);

struct BridgeDiagStats {
    size_t tcp_to_ble_waiting;
    size_t tcp_to_ble_capacity;
    uint32_t tcp_to_ble_dropped;
    size_t ble_to_tcp_waiting;
    size_t ble_to_tcp_capacity;
    uint32_t ble_to_tcp_dropped;
    size_t mqtt_to_ble_waiting;
    size_t mqtt_to_ble_capacity;
    uint32_t mqtt_to_ble_dropped;
    size_t ble_to_http_waiting;
    size_t ble_to_http_capacity;
    uint32_t ble_to_http_dropped;
    size_t connected_tcp_clients;
    uint32_t ble_task_stack_free_bytes;
    uint32_t net_task_stack_free_bytes;
    bool ble_connected;
    uint32_t ble_rx_count;
    uint32_t ble_tx_count;
    uint32_t ble_invalid_count;
    uint32_t tcp_rx_count;
    uint32_t tcp_tx_count;
    uint32_t tcp_invalid_count;
    bool http_phoneapi_active;
    uint32_t http_rx_count;
    uint32_t http_tx_count;
    uint32_t http_invalid_count;
};

BridgeDiagStats bridge_get_diag_stats();
