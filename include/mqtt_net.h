#pragma once

#include <Arduino.h>
#include <functional>

enum MqttState {
    MQTT_STATE_DISABLED = 0,
    MQTT_STATE_WAITING_RADIO_CONFIG,
    MQTT_STATE_WAITING_BLE,
    MQTT_STATE_WAITING_REMOTE_ACCESS,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_DISCONNECTED,
    MQTT_STATE_ERROR
};

/**
 * Configuration for MQTT.
 */
struct MqttConfig {
    bool enabled = true;
    bool tls_insecure = false;
    String custom_ca = "";
};

/**
 * Represents the current status of the MQTT client.
 */
struct MqttStatus {
    // State of the client connection.
    MqttState state = MQTT_STATE_DISABLED;
    String state_str = "Disabled";

    // MQTT settings stored on the radio (cached)
    bool radio_proxy_enabled = false;
    String radio_server = "";
    uint16_t radio_port = 1883;
    bool radio_tls = false;
    String radio_user = "";
    String radio_root = "msh";

    // The currently running MQTT client settings
    String active_server = "";
    uint16_t active_port = 1883;
    String active_root = "msh";
    bool active_tls = false;

    // Counters/State.
    uint32_t msgs_published = 0;
    uint32_t msgs_received = 0;
    uint32_t msgs_dropped = 0;
    uint32_t msgs_invalid = 0;
    String last_error = "";
};

/**
 * Record an invalid or unparseable MQTT downlink message (e.g. oversized topic/payload, protobuf error).
 */
void mqtt_net_record_invalid();

/**
 * Callback to push ToRadio downlink packets into the BLE transmit queue
 * As MQTT messages are received from the client, this call back is fired with the packet
 * to be handled.
 */
typedef std::function<void(const uint8_t* to_radio_buf, size_t len)> MqttDownlinkCallback;

/**
 * Initialize MQTT subsystem.
 */
void mqtt_net_init();

/**
 * Periodic loop for background timers (e.g. 60s BLE disconnect grace period)
 */
void mqtt_net_loop();

/**
 * Apply new configuration (starts, stops, or reconfigures the client)
 * @param cfg the updated configuration.
 */
void mqtt_net_apply_config(const MqttConfig& cfg);

/**
 * Check state of the mqtt subsystem.
 * NOTE: only tells if its enabled/disabled, not the connected state or anything like that.
 *
 * @return Returns true if MQTT Gateway feature is enabled (atomic, wait-free fast path)
 */
bool mqtt_net_is_enabled();

/**
 * Called when the BLE connection to the radio is established and ready.
 */
void mqtt_net_on_ble_connected();

/**
 * Called when the BLE connection to the radio is lost.
 */
void mqtt_net_on_ble_disconnected();

/**
 * Register callback for forwarding downlink MQTT messages to the BLE radio
 * @param cb The callback to register.
 */
void mqtt_net_set_downlink_callback(MqttDownlinkCallback cb);

/**
 * Inspects incoming FromRadio BLE bytes for MqttClientProxyMessage and ModuleConfig.mqtt
 * @return Returns true if the packet was an MQTT proxy message (and should not be broadcast to TCP clients).
 */
bool mqtt_net_handle_from_radio(const uint8_t* data, size_t len);

/**
 * Publishes an MQTT message directly to the active broker
 * @return true on success, false on error.
 */
bool mqtt_net_publish(const char* topic, const uint8_t* payload, size_t len, bool retained);

/**
 * Get current MQTT status and telemetry
 */
MqttStatus mqtt_net_get_status();
