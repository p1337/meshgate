#pragma once
#include <Arduino.h>

void bridge_init(const String& ble_name, const String& ble_mac, uint32_t ble_pin);
void bridge_start();
bool bridge_is_ble_connected();
bool bridge_enqueue_mqtt_to_ble(const uint8_t* data, size_t len);
void bridge_clear_mqtt_to_ble_queue();

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
};

BridgeDiagStats bridge_get_diag_stats();
