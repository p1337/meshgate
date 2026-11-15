#include "mqtt_net.h"
#include "web_log.h"
#include "bridge.h"
#include "build_options.h"
#include "tailscale_net.h"
#include "device_presence.h"
#include <mqtt_client.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <mbedtls/platform.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "meshtastic/admin.pb.h"
#include "meshtastic/channel.pb.h"
#include "meshtastic/config.pb.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/module_config.pb.h"
#include "meshtastic/portnums.pb.h"
#include "utils.h"
#include <atomic>

// MeshRadar publishes these shared credentials publicly for Meshtastic map
// uplinks. They are the factory default, not a private deployment secret.
// A complete custom MQTT configuration received from the radio can replace it.
static constexpr const char* MQTT_DEFAULT_SERVER = "meshradar.pl";
static constexpr uint16_t MQTT_DEFAULT_PORT = 8883;
static constexpr const char* MQTT_DEFAULT_USER = "meshradar";
static constexpr const char* MQTT_DEFAULT_PASSWORD = "meshradar";
static constexpr const char* MQTT_DEFAULT_ROOT = "msh";
static constexpr bool MQTT_DEFAULT_TLS = true;
static constexpr const char* MESHTASTIC_PUBLIC_SERVER = "mqtt.meshtastic.org";
static constexpr const char* MESHTASTIC_PUBLIC_USER = "meshdev";
static constexpr const char* MESHTASTIC_PUBLIC_PASSWORD = "large4cats";
static constexpr size_t TLS_PSRAM_THRESHOLD = 4096;

#ifdef ESP_IDF5_MIGRATION_BUILD
// The Nano ESP32 has 8 MB PSRAM but relatively little contiguous internal
// memory. Two simultaneous TLS clients (Tailscale DERP and MQTT) otherwise
// compete for 16 KB record buffers. Data buffers are safe in PSRAM; FreeRTOS
// task stacks deliberately remain in internal RAM.
static void* tls_psram_calloc(size_t count, size_t size) {
    if (count != 0 && size > SIZE_MAX / count) {
        return nullptr;
    }

    const size_t total = count * size;
    const bool preferPsram = total >= TLS_PSRAM_THRESHOLD;
    const uint32_t preferredCaps = preferPsram
        ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
        : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const uint32_t alternateCaps = preferPsram
        ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    void* result = heap_caps_calloc(count, size, preferredCaps);
    return result != nullptr ? result : heap_caps_calloc(count, size, alternateCaps);
}

static void tls_psram_free(void* ptr) {
    heap_caps_free(ptr);
}
#endif

/**
 * Embedded Mozilla Root CA Bundle
 * Generated during build process.
 */
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_data_cert_x509_crt_bundle_bin_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_data_cert_x509_crt_bundle_bin_end");

/**
 * Atomic flag for wait-free status queries in fast loops.
 * True if mqtt subsystem feature flag is enabled, false if disabled.
 * NOTE: Does NOT tell you the state of the client (connected vs disconnected etc...)
 */
static std::atomic<bool> s_mqtt_enabled{false};

/**
 * Atomic message telemetry counters (wait-free, zero lock contention).
 */
static std::atomic<uint32_t> s_msgs_published{0};
static std::atomic<uint32_t> s_msgs_received{0};
static std::atomic<uint32_t> s_msgs_invalid{0};

void mqtt_net_record_invalid() {
    s_msgs_invalid.fetch_add(1, std::memory_order_relaxed);
}

/**
 * Callback to disable certificate validation for self-signed or insecure TLS
 */
static esp_err_t mqtt_tls_insecure_attach(void *conf) {
    if (conf) {
        mbedtls_ssl_config *ssl_conf = (mbedtls_ssl_config *)conf;
        mbedtls_ssl_conf_authmode(ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    return ESP_OK;
}

/**
 * Active Broker Session Definition for change detection.
 */
struct ActiveBrokerSession {
    String server = "";
    uint16_t port = 0;
    String user = "";
    String pass = "";
    String root = "";
    bool tls = false;
    bool tls_insecure = false;
    String custom_ca = "";
    bool is_active = false;
};


/**
 * MQTT Proxy Settings from WebUI.
 */
static MqttConfig currentConfig;

/**
 * MqttStatus contains the live telemetry properties (radio_server, radio_user, radio_root, active_server, etc.)
 * queried for /status.
 */
static MqttStatus makeDefaultMqttStatus() {
    MqttStatus status;
    // The broker profile is only a local fallback until a real radio
    // ModuleConfig arrives. Never assume the radio proxy is enabled.
    status.radio_proxy_enabled = false;
    status.radio_server = MQTT_DEFAULT_SERVER;
    status.radio_port = MQTT_DEFAULT_PORT;
    status.radio_tls = MQTT_DEFAULT_TLS;
    status.radio_user = MQTT_DEFAULT_USER;
    status.radio_root = MQTT_DEFAULT_ROOT;
    return status;
}

static MqttStatus currentStatus = makeDefaultMqttStatus();

/**
 * The mqtt broker password configured on the radio is excluded from MqttStatus explicitly
 * to avoid leaking it via the telemetry end points.
 */
static String radioPassword = MQTT_DEFAULT_PASSWORD;

/**
 * Details about the currently connected mqtt client.
 * Generally, this matches the values in MqttStatus, but if the user
 * updates the mqtt settings on the radio, this is how we detect that change.
 */
static ActiveBrokerSession currentSession;

/**
 * Internal State & Recursive Mutex
 */
static SemaphoreHandle_t mqttMutex = NULL;
// Serializes the comparatively slow ESP-IDF client start/stop lifecycle.
// MQTT event callbacks never take this mutex, so esp_mqtt_client_stop() may
// wait for them without deadlocking against mqttMutex.
static SemaphoreHandle_t mqttLifecycleMutex = NULL;
static esp_mqtt_client_handle_t mqttClient = NULL;
static MqttDownlinkCallback downlinkCallback = nullptr;

/**
 * Bluetooth Disconnect Grace Period Tracking
 */
static std::atomic<uint32_t> s_ble_disconnected_at{0};
static std::atomic<bool> s_in_grace_period{false};
static std::atomic<bool> s_start_deferred{false};
static std::atomic<bool> s_paused_for_ota{false};
static MqttConfig s_config_before_ota;

namespace {
constexpr uint8_t RADIO_SETUP_CHANNEL_COUNT = 8;
constexpr uint32_t RADIO_SETUP_STEP_INTERVAL_MS = 300;
constexpr uint32_t RADIO_SETUP_CONFIG_TIMEOUT_MS = 30000;

enum class RadioSetupPhase : uint8_t {
    Idle,
    WaitingConfig,
    RequestSessionKey,
    WaitingSessionKey,
    BeginEdit,
    SetMqtt,
    SetTelemetry,
    SetLora,
    SetChannels,
    Commit,
    Verifying,
    Complete,
    Error,
};

enum class RadioSetupOperation : uint8_t {
    None,
    Mqtt,
    DeviceTelemetry,
};

RadioSetupPhase s_radio_setup_phase = RadioSetupPhase::Idle;
RadioSetupOperation s_radio_setup_operation = RadioSetupOperation::None;
uint32_t s_radio_setup_config_id = 0;
uint32_t s_radio_setup_deadline = 0;
uint32_t s_radio_setup_next_action = 0;
uint32_t s_radio_setup_node_num = 0;
uint8_t s_radio_setup_uplink_mask = 0;
uint8_t s_radio_setup_downlink_mask = 0;
uint8_t s_radio_setup_channel_cursor = 0;
bool s_radio_setup_have_mqtt = false;
bool s_radio_setup_have_lora = false;
bool s_radio_setup_have_telemetry = false;
bool s_radio_setup_have_session_key = false;
bool s_radio_setup_have_channel[RADIO_SETUP_CHANNEL_COUNT] = {};
meshtastic_ModuleConfig_MQTTConfig s_radio_setup_mqtt = meshtastic_ModuleConfig_MQTTConfig_init_default;
meshtastic_Config_LoRaConfig s_radio_setup_lora = meshtastic_Config_LoRaConfig_init_default;
meshtastic_ModuleConfig_TelemetryConfig s_radio_setup_telemetry = meshtastic_ModuleConfig_TelemetryConfig_init_default;
uint32_t s_radio_setup_telemetry_interval = 1800;
meshtastic_Channel s_radio_setup_channels[RADIO_SETUP_CHANNEL_COUNT] = {};
meshtastic_AdminMessage_session_passkey_t s_radio_setup_session_key = {};

// These generated protobuf unions are deliberately static. The network task
// has very little stack headroom on the Nano ESP32 and radio setup must not
// turn a configuration convenience into another heap/stack pressure source.
meshtastic_AdminMessage s_radio_setup_admin = meshtastic_AdminMessage_init_default;
meshtastic_AdminMessage s_radio_setup_admin_response = meshtastic_AdminMessage_init_default;
meshtastic_ToRadio s_radio_setup_to_radio = meshtastic_ToRadio_init_default;
uint8_t s_radio_setup_admin_payload[meshtastic_Constants_DATA_PAYLOAD_LEN] = {};
uint8_t s_radio_setup_packet[MESHTASTIC_MAX_PACKET_SIZE] = {};

const char* radioSetupPhaseName(RadioSetupPhase phase) {
    switch (phase) {
        case RadioSetupPhase::WaitingConfig: return "Reading radio";
        case RadioSetupPhase::RequestSessionKey:
        case RadioSetupPhase::WaitingSessionKey: return "Authorizing";
        case RadioSetupPhase::BeginEdit:
        case RadioSetupPhase::SetMqtt:
        case RadioSetupPhase::SetTelemetry:
        case RadioSetupPhase::SetLora:
        case RadioSetupPhase::SetChannels:
        case RadioSetupPhase::Commit: return "Applying";
        case RadioSetupPhase::Verifying: return "Verifying";
        case RadioSetupPhase::Complete: return "Complete";
        case RadioSetupPhase::Error: return "Error";
        default: return "Idle";
    }
}

bool radioSetupActive() {
    return s_radio_setup_phase != RadioSetupPhase::Idle &&
           s_radio_setup_phase != RadioSetupPhase::Complete &&
           s_radio_setup_phase != RadioSetupPhase::Error;
}

void resetRadioSetupSnapshot() {
    s_radio_setup_node_num = 0;
    s_radio_setup_have_mqtt = false;
    s_radio_setup_have_lora = false;
    s_radio_setup_have_telemetry = false;
    s_radio_setup_have_session_key = false;
    memset(s_radio_setup_have_channel, 0, sizeof(s_radio_setup_have_channel));
    s_radio_setup_mqtt = meshtastic_ModuleConfig_MQTTConfig_init_default;
    s_radio_setup_lora = meshtastic_Config_LoRaConfig_init_default;
    s_radio_setup_telemetry = meshtastic_ModuleConfig_TelemetryConfig_init_default;
    memset(s_radio_setup_channels, 0, sizeof(s_radio_setup_channels));
    memset(&s_radio_setup_session_key, 0, sizeof(s_radio_setup_session_key));
}

void setRadioSetupState(RadioSetupPhase phase, const char* detail) {
    s_radio_setup_phase = phase;
    currentStatus.radio_setup_state = radioSetupPhaseName(phase);
    currentStatus.radio_setup_operation = s_radio_setup_operation == RadioSetupOperation::Mqtt
        ? "MQTT"
        : (s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry ? "Device telemetry" : "");
    currentStatus.radio_setup_detail = detail ? detail : "";
    currentStatus.radio_setup_uplink_mask = s_radio_setup_uplink_mask;
    currentStatus.radio_setup_downlink_mask = s_radio_setup_downlink_mask;
    if (phase == RadioSetupPhase::Complete || phase == RadioSetupPhase::Error) {
        bridge_end_radio_control();
    }
}
} // namespace

/**
 * Resets grace timer state (assumes caller holds mqttMutex / MqttLockGuard).
 */
static inline void mqtt_net_reset_grace_timer() {
    s_in_grace_period.store(false, std::memory_order_relaxed);
    s_ble_disconnected_at.store(0, std::memory_order_relaxed);
}

/**
 * RAII Lock Guard for deterministic recursive mutex ownership with optional timeout
 */
class MqttLockGuard {
public:
    MqttLockGuard(TickType_t timeout = portMAX_DELAY) {
        if (mqttMutex) {
            acquired = (xSemaphoreTakeRecursive(mqttMutex, timeout) == pdTRUE);
        }
    }
    ~MqttLockGuard() {
        if (mqttMutex && acquired) {
            xSemaphoreGiveRecursive(mqttMutex);
        }
    }
    bool is_locked() const { return acquired; }
private:
    bool acquired = false;
};

class MqttLifecycleGuard {
public:
    MqttLifecycleGuard() {
        if (mqttLifecycleMutex) {
            acquired = (xSemaphoreTake(mqttLifecycleMutex, portMAX_DELAY) == pdTRUE);
        }
    }
    ~MqttLifecycleGuard() {
        if (mqttLifecycleMutex && acquired) {
            xSemaphoreGive(mqttLifecycleMutex);
        }
    }
    bool is_locked() const { return acquired; }
private:
    bool acquired = false;
};

static bool queueRadioSetupAdmin(const meshtastic_AdminMessage& admin, bool wantResponse = true) {
    pb_ostream_t adminStream = pb_ostream_from_buffer(
        s_radio_setup_admin_payload,
        sizeof(s_radio_setup_admin_payload)
    );
    if (!pb_encode(&adminStream, meshtastic_AdminMessage_fields, &admin)) {
        log_e("[MQTT Setup] Could not encode AdminMessage: %s", PB_GET_ERROR(&adminStream));
        return false;
    }

    s_radio_setup_to_radio = meshtastic_ToRadio_init_default;
    s_radio_setup_to_radio.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket& packet = s_radio_setup_to_radio.packet;
    packet.to = s_radio_setup_node_num;
    packet.channel = 0;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.decoded.portnum = meshtastic_PortNum_ADMIN_APP;
    packet.decoded.want_response = wantResponse;
    packet.decoded.payload.size = adminStream.bytes_written;
    memcpy(packet.decoded.payload.bytes, s_radio_setup_admin_payload, adminStream.bytes_written);
    packet.id = esp_random();
    if (packet.id == 0) packet.id = 1;
    packet.want_ack = true;
    packet.priority = meshtastic_MeshPacket_Priority_RELIABLE;
    packet.pki_encrypted = true;

    pb_ostream_t packetStream = pb_ostream_from_buffer(
        s_radio_setup_packet,
        sizeof(s_radio_setup_packet)
    );
    if (!pb_encode(&packetStream, meshtastic_ToRadio_fields, &s_radio_setup_to_radio)) {
        log_e("[MQTT Setup] Could not encode ToRadio packet: %s", PB_GET_ERROR(&packetStream));
        return false;
    }
    return bridge_enqueue_control_to_ble(s_radio_setup_packet, packetStream.bytes_written);
}

static bool queueRadioSetupStep() {
    s_radio_setup_admin = meshtastic_AdminMessage_init_default;
    switch (s_radio_setup_phase) {
        case RadioSetupPhase::RequestSessionKey:
            s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_get_config_request_tag;
            s_radio_setup_admin.get_config_request = meshtastic_AdminMessage_ConfigType_SESSIONKEY_CONFIG;
            return queueRadioSetupAdmin(s_radio_setup_admin, true);

        case RadioSetupPhase::BeginEdit:
            s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_begin_edit_settings_tag;
            s_radio_setup_admin.begin_edit_settings = true;
            s_radio_setup_admin.session_passkey = s_radio_setup_session_key;
            return queueRadioSetupAdmin(s_radio_setup_admin);

        case RadioSetupPhase::SetTelemetry:
            s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
            s_radio_setup_admin.set_module_config.which_payload_variant = meshtastic_ModuleConfig_telemetry_tag;
            s_radio_setup_admin.set_module_config.payload_variant.telemetry = s_radio_setup_telemetry;
            s_radio_setup_admin.set_module_config.payload_variant.telemetry.device_telemetry_enabled = true;
            s_radio_setup_admin.set_module_config.payload_variant.telemetry.device_update_interval =
                s_radio_setup_telemetry_interval;
            s_radio_setup_admin.session_passkey = s_radio_setup_session_key;
            return queueRadioSetupAdmin(s_radio_setup_admin);

        case RadioSetupPhase::SetMqtt:
            s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_set_module_config_tag;
            s_radio_setup_admin.set_module_config.which_payload_variant = meshtastic_ModuleConfig_mqtt_tag;
            s_radio_setup_admin.set_module_config.payload_variant.mqtt = s_radio_setup_mqtt;
            {
                auto& mqtt = s_radio_setup_admin.set_module_config.payload_variant.mqtt;
                mqtt.enabled = true;
                snprintf(mqtt.address, sizeof(mqtt.address), "%s", MQTT_DEFAULT_SERVER);
                snprintf(mqtt.username, sizeof(mqtt.username), "%s", MQTT_DEFAULT_USER);
                snprintf(mqtt.password, sizeof(mqtt.password), "%s", MQTT_DEFAULT_PASSWORD);
                mqtt.encryption_enabled = true;
                mqtt.json_enabled = false;
                mqtt.tls_enabled = MQTT_DEFAULT_TLS;
                snprintf(mqtt.root, sizeof(mqtt.root), "%s", MQTT_DEFAULT_ROOT);
                mqtt.proxy_to_client_enabled = true;
            }
            s_radio_setup_admin.session_passkey = s_radio_setup_session_key;
            return queueRadioSetupAdmin(s_radio_setup_admin);

        case RadioSetupPhase::SetLora:
            s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_set_config_tag;
            s_radio_setup_admin.set_config.which_payload_variant = meshtastic_Config_lora_tag;
            s_radio_setup_admin.set_config.payload_variant.lora = s_radio_setup_lora;
            s_radio_setup_admin.set_config.payload_variant.lora.config_ok_to_mqtt = true;
            s_radio_setup_admin.session_passkey = s_radio_setup_session_key;
            return queueRadioSetupAdmin(s_radio_setup_admin);

        case RadioSetupPhase::SetChannels: {
            while (s_radio_setup_channel_cursor < RADIO_SETUP_CHANNEL_COUNT) {
                const uint8_t channel = s_radio_setup_channel_cursor;
                const uint8_t bit = static_cast<uint8_t>(1u << channel);
                if (((s_radio_setup_uplink_mask | s_radio_setup_downlink_mask) & bit) == 0) {
                    ++s_radio_setup_channel_cursor;
                    continue;
                }
                s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_set_channel_tag;
                s_radio_setup_admin.set_channel = s_radio_setup_channels[channel];
                if (s_radio_setup_uplink_mask & bit) {
                    s_radio_setup_admin.set_channel.settings.uplink_enabled = true;
                }
                if (s_radio_setup_downlink_mask & bit) {
                    s_radio_setup_admin.set_channel.settings.downlink_enabled = true;
                }
                s_radio_setup_admin.session_passkey = s_radio_setup_session_key;
                if (queueRadioSetupAdmin(s_radio_setup_admin)) {
                    ++s_radio_setup_channel_cursor;
                    return true;
                }
                return false;
            }
            return true;
        }

        case RadioSetupPhase::Commit:
            s_radio_setup_admin.which_payload_variant = meshtastic_AdminMessage_commit_edit_settings_tag;
            s_radio_setup_admin.commit_edit_settings = true;
            s_radio_setup_admin.session_passkey = s_radio_setup_session_key;
            return queueRadioSetupAdmin(s_radio_setup_admin);

        default:
            return false;
    }
}

/**
 * State mapping helper.
 * @param state The state to get the string/human readable representation from
 * @return The human readable state value.
 */
static const char* mqtt_state_to_str(MqttState state) {
    switch (state) {
        case MQTT_STATE_DISABLED: return "Disabled";
        case MQTT_STATE_WAITING_RADIO_CONFIG: return "Waiting for Radio Config";
        case MQTT_STATE_WAITING_BLE: return "Waiting for Bluetooth";
        case MQTT_STATE_WAITING_REMOTE_ACCESS: return "Waiting for Remote Access";
        case MQTT_STATE_CONNECTING: return "Connecting...";
        case MQTT_STATE_CONNECTED: return "Connected";
        case MQTT_STATE_DISCONNECTED: return "Disconnected";
        case MQTT_STATE_ERROR: return "Error";
        default: return "Unknown";
    }
}

// Update state and log transition
static void mqtt_net_set_state(MqttState newState, const String& errorMsg = "") {
    MqttLockGuard lock;
    currentStatus.state = newState;
    currentStatus.state_str = mqtt_state_to_str(newState);
    if (!errorMsg.isEmpty()) {
        currentStatus.last_error = errorMsg;
    } else if (newState == MQTT_STATE_CONNECTED) {
        currentStatus.last_error = "";
    }
    log_i("[MQTT] State -> %s %s", currentStatus.state_str.c_str(), errorMsg.isEmpty() ? "" : errorMsg.c_str());
}

/**
 * Parses detailed error diagnostics from an MQTT_EVENT_ERROR event.
 * Logs hex codes to serial and returns a user-friendly status message.
 */
static String mqtt_net_parse_error_details(esp_mqtt_event_handle_t event) {
    String errorMsg = "Socket error / Broker unreachable";
    if (!event || !event->error_handle) {
        return errorMsg;
    }

    if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        log_e(
            "[MQTT] Transport/TLS error (esp_tls_last_esp_err: 0x%x, esp_tls_stack_err: 0x%x, cert_flags: 0x%x)",
            event->error_handle->esp_tls_last_esp_err,
            event->error_handle->esp_tls_stack_err,
            event->error_handle->esp_tls_cert_verify_flags
        );
        if (event->error_handle->esp_tls_cert_verify_flags != 0) {
            errorMsg = "TLS Certificate validation failed";
        } else if (event->error_handle->esp_tls_last_esp_err != 0) {
            errorMsg = "TLS handshake / Socket error";
        }
    } else if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        log_e("[MQTT] Broker refused connection (code: %d)", event->error_handle->connect_return_code);
        switch (event->error_handle->connect_return_code) {
            case MQTT_CONNECTION_REFUSE_BAD_USERNAME:
                errorMsg = "Auth failed (Bad Username/Password)";
                break;
            case MQTT_CONNECTION_REFUSE_NOT_AUTHORIZED:
                errorMsg = "Rejected by broker (Not Authorized)";
                break;
            case MQTT_CONNECTION_REFUSE_SERVER_UNAVAILABLE:
                errorMsg = "Broker server unavailable";
                break;
            case MQTT_CONNECTION_REFUSE_PROTOCOL:
                errorMsg = "Broker rejected MQTT protocol version";
                break;
            case MQTT_CONNECTION_REFUSE_ID_REJECTED:
                errorMsg = "Broker rejected Client ID";
                break;
            default:
                errorMsg = "Broker connection refused";
                break;
        }
    }
    return errorMsg;
}

/**
 * Event handler for ESP-IDF MQTT Client.
 * @param event The event to process.
 * @return
 */
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event) {
    switch (event->event_id) {
        // On Connect.
        case MQTT_EVENT_CONNECTED: {
            log_i("[MQTT] Connected to broker successfully.");
            mqtt_net_set_state(MQTT_STATE_CONNECTED);

            // Subscribe to synced root topic: <root>/# (e.g., "msh/#" or "msh/JP/#" etc...)
            String root = currentStatus.active_root.isEmpty() ? "msh" : currentStatus.active_root;
            String subTopic = root.endsWith("/") ? (root + "#") : (root + "/#");
            int msg_id = esp_mqtt_client_subscribe(event->client, subTopic.c_str(), 0);
            log_i("[MQTT] Subscribed to topic '%s' (msg_id: %d)", subTopic.c_str(), msg_id);
            break;
        }

        // On Disconnect.
        case MQTT_EVENT_DISCONNECTED: {
            log_w("[MQTT] Disconnected from broker.");
            bool isEnabled = false;
            {
                MqttLockGuard lock;
                isEnabled = currentConfig.enabled;
            }
            mqtt_net_set_state(isEnabled ? MQTT_STATE_DISCONNECTED : MQTT_STATE_DISABLED);
            break;
        }

        case MQTT_EVENT_SUBSCRIBED: {
            log_d("[MQTT] Subscription confirmed (msg_id: %d)", event->msg_id);
            break;
        }

        case MQTT_EVENT_DATA: {
            // Log and increment received counter
            log_d("[MQTT] Received message on topic '%.*s' (%d bytes)", event->topic_len, event->topic, event->data_len);
            s_msgs_received.fetch_add(1, std::memory_order_relaxed);

            if (!downlinkCallback) {
                log_w("[MQTT] Received MQTT message but no downlink callback is defined!");
                break;
            }

            // Construct Meshtastic ToRadio envelope with MqttClientProxyMessage
            meshtastic_ToRadio to_radio = meshtastic_ToRadio_init_default;
            to_radio.which_payload_variant = meshtastic_ToRadio_mqttClientProxyMessage_tag;

            meshtastic_MqttClientProxyMessage* proxy_msg = &to_radio.mqttClientProxyMessage;

            if (event->topic_len >= (int)sizeof(proxy_msg->topic)) {
                log_e("[MQTT] ERROR: Downlink topic exceeds protobuf capacity (%d >= %zu bytes). Dropping packet.",
                      event->topic_len, sizeof(proxy_msg->topic));
                mqtt_net_record_invalid();
                break;
            }
            if ((size_t)event->data_len > sizeof(proxy_msg->payload_variant.data.bytes)) {
                log_e("[MQTT] ERROR: Downlink payload exceeds protobuf capacity (%d > %zu bytes). Dropping packet.",
                      event->data_len, sizeof(proxy_msg->payload_variant.data.bytes));
                mqtt_net_record_invalid();
                break;
            }

            size_t topicLen = event->topic_len;
            memcpy(proxy_msg->topic, event->topic, topicLen);
            proxy_msg->topic[topicLen] = '\0';
            proxy_msg->retained = (event->retain != 0);

            proxy_msg->which_payload_variant = meshtastic_MqttClientProxyMessage_data_tag;
            size_t copyLen = (size_t)event->data_len;
            memcpy(proxy_msg->payload_variant.data.bytes, event->data, copyLen);
            proxy_msg->payload_variant.data.size = copyLen;

            /*
             * Encode into stack buffer.
             * Sized to MESHTASTIC_MAX_PACKET_SIZE to comfortably fit the full ToRadio envelope:
             *   - Max payload: 512 bytes
             *   - Topic string: up to 32 bytes
             *   - Retained flag: 2 bytes
             *   - Protobuf tags & length headers: ~10 bytes
             * Total encoded ToRadio envelope can reach ~556 bytes.
             */
            uint8_t to_radio_buf[MESHTASTIC_MAX_PACKET_SIZE];
            pb_ostream_t stream = pb_ostream_from_buffer(to_radio_buf, sizeof(to_radio_buf));
            if (pb_encode(&stream, meshtastic_ToRadio_fields, &to_radio)) {
                downlinkCallback(to_radio_buf, stream.bytes_written);
            } else {
                log_e("[MQTT] ERROR: Failed to encode ToRadio protobuf for downlink (%s)", PB_GET_ERROR(&stream));
                mqtt_net_record_invalid();
            }
            break;
        }

        // On Errors
        case MQTT_EVENT_ERROR: {
            String errorMsg = mqtt_net_parse_error_details(event);
            log_e("[MQTT] Connection error: %s", errorMsg.c_str());
            mqtt_net_set_state(MQTT_STATE_ERROR, errorMsg);
            break;
        }

        // All other event types ignored.
        default:
            break;
    }
    return ESP_OK;
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void mqtt_event_handler_idf5(void*, esp_event_base_t, int32_t, void* event_data) {
    mqtt_event_handler(static_cast<esp_mqtt_event_handle_t>(event_data));
}
#endif

/**
 * Safely stops and destroys the active ESP-IDF MQTT client handle.
 */
// Caller owns mqttLifecycleMutex. The handle is detached while holding the
// state mutex, but ESP-IDF stop/destroy deliberately runs without mqttMutex:
// the MQTT task can emit a final event whose callback also needs mqttMutex.
static void mqtt_net_destroy_client_with_lifecycle_lock() {
    esp_mqtt_client_handle_t clientToDestroy = NULL;
    {
        MqttLockGuard lock;
        if (mqttClient) {
            clientToDestroy = mqttClient;
            mqttClient = NULL;
        }
    }
    if (clientToDestroy) {
        log_i("[MQTT] Stopping and destroying MQTT client...");
        esp_mqtt_client_stop(clientToDestroy);
        esp_mqtt_client_destroy(clientToDestroy);
    }
}

// Client starter (releases lock before starting MQTT task to prevent deadlocks)
static void mqtt_net_start_client(const String& server, uint16_t port, const String& user, const String& pass, bool tls, bool tls_insecure, const String& custom_ca) {
    MqttLifecycleGuard lifecycleLock;
    if (!lifecycleLock.is_locked()) {
        mqtt_net_set_state(MQTT_STATE_ERROR, "MQTT lifecycle lock unavailable");
        return;
    }

    // Re-check after taking the lifecycle mutex. Two callers may decide to
    // start at the same time (for example DERP readiness and BLE reconnect),
    // but the second one must not tear down the matching client just started
    // by the first.
    {
        MqttLockGuard lock;
        const String desiredRoot = currentStatus.radio_root.isEmpty() ? "msh" : currentStatus.radio_root;
        if (mqttClient && currentSession.is_active &&
            currentSession.server == server &&
            currentSession.port == port &&
            currentSession.user == user &&
            currentSession.pass == pass &&
            currentSession.root == desiredRoot &&
            currentSession.tls == tls &&
            currentSession.tls_insecure == tls_insecure &&
            currentSession.custom_ca == custom_ca) {
            log_d("[MQTT] Matching broker client won the lifecycle race; keeping it.");
            return;
        }
    }

    mqtt_net_destroy_client_with_lifecycle_lock();

    esp_mqtt_client_config_t mqtt_cfg = {};
    esp_mqtt_client_handle_t newClient = NULL;
    {
        MqttLockGuard lock;

        // Sanity check config.
        if (server.isEmpty()) {
            log_e("[MQTT] No broker address is configured on radio, cannot start.");
            mqtt_net_set_state(MQTT_STATE_ERROR, "No broker address configured on radio");
            return;
        }

        // Copy detail into the current status entry
        currentStatus.active_server = server;
        currentStatus.active_port = port;
        currentStatus.active_tls = tls;
        currentStatus.active_root = currentStatus.radio_root.isEmpty() ? "msh" : currentStatus.radio_root;

        // Track active session for seamless reconnect comparison
        currentSession.server = server;
        currentSession.port = port;
        currentSession.user = user;
        currentSession.pass = pass;
        currentSession.root = currentStatus.active_root;
        currentSession.tls = tls;
        currentSession.tls_insecure = tls_insecure;
        currentSession.custom_ca = custom_ca;
        currentSession.is_active = true;

        // Reset the disconnect grace timer on connect.
        mqtt_net_reset_grace_timer();

        // Update state to connecting...
        mqtt_net_set_state(MQTT_STATE_CONNECTING);

        // Build out the underlying mqtt client
#if ESP_IDF_VERSION_MAJOR >= 5
        mqtt_cfg.broker.address.hostname = server.c_str();
        mqtt_cfg.broker.address.port = port;
        if (!user.isEmpty()) {
            mqtt_cfg.credentials.username = user.c_str();
        }
        if (!pass.isEmpty()) {
            mqtt_cfg.credentials.authentication.password = pass.c_str();
        }
        mqtt_cfg.broker.address.transport = tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
#else
        mqtt_cfg.host = server.c_str();
        mqtt_cfg.port = port;
        if (!user.isEmpty()) {
            mqtt_cfg.username = user.c_str();
        }
        if (!pass.isEmpty()) {
            mqtt_cfg.password = pass.c_str();
        }
        mqtt_cfg.transport = tls ? MQTT_TRANSPORT_OVER_SSL : MQTT_TRANSPORT_OVER_TCP;
#endif

        // 3-Tier TLS Security Configuration
        if (tls) {
            if (tls_insecure) {
                // Tier 3: Insecure bypass (self-signed, local IP, or CN mismatch)
#if ESP_IDF_VERSION_MAJOR >= 5
                mqtt_cfg.broker.verification.crt_bundle_attach = mqtt_tls_insecure_attach;
                mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
#else
                mqtt_cfg.crt_bundle_attach = mqtt_tls_insecure_attach;
                mqtt_cfg.skip_cert_common_name_check = true;
#endif
            } else if (!custom_ca.isEmpty()) {
                // Tier 2: Custom / Private Root CA or Server Certificate PEM
#if ESP_IDF_VERSION_MAJOR >= 5
                mqtt_cfg.broker.verification.certificate = custom_ca.c_str();
                mqtt_cfg.broker.verification.crt_bundle_attach = NULL;
#else
                mqtt_cfg.cert_pem = custom_ca.c_str();
                mqtt_cfg.crt_bundle_attach = NULL;
#endif
            } else {
                // Tier 1: Embedded Mozilla Root CA Bundle (Let's Encrypt, DigiCert, etc.)
#if ESP_IDF_VERSION_MAJOR >= 5
                mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#else
                mqtt_cfg.crt_bundle_attach = arduino_esp_crt_bundle_attach;
#endif
            }
        }

#if ESP_IDF_VERSION_MAJOR >= 5
        mqtt_cfg.buffer.size = MQTT_CLIENT_BUFFER_SIZE;
        mqtt_cfg.buffer.out_size = MQTT_CLIENT_BUFFER_SIZE;
        mqtt_cfg.session.keepalive = MQTT_CLIENT_KEEPALIVE_SECONDS;
        mqtt_cfg.network.reconnect_timeout_ms = MQTT_RECONNECT_TIME_MS;
        mqtt_cfg.task.priority = 2; // Keep below AsyncTCP (3) and LwIP (18) so WebUI and network sockets are never starved
        mqtt_cfg.task.stack_size = 6144; // Balanced for TLS/Nanopb while preserving internal RAM for DERP
#else
        mqtt_cfg.buffer_size = MQTT_CLIENT_BUFFER_SIZE;
        mqtt_cfg.out_buffer_size = MQTT_CLIENT_BUFFER_SIZE;
        mqtt_cfg.keepalive = MQTT_CLIENT_KEEPALIVE_SECONDS;
        mqtt_cfg.reconnect_timeout_ms = MQTT_RECONNECT_TIME_MS;
        mqtt_cfg.task_prio = 2; // Keep below AsyncTCP (3) and LwIP (18) so WebUI and network sockets are never starved
        mqtt_cfg.task_stack = 6144; // Balanced for TLS/Nanopb while preserving internal RAM for remote access
        mqtt_cfg.event_handle = mqtt_event_handler;
#endif

        log_i(
            "[MQTT] Starting client -> %s:%u (TLS: %s, Insecure: %s, Custom CA: %s)",
            server.c_str(),
            port,
            tls ? "Yes" : "No",
            tls_insecure ? "Yes" : "No",
            custom_ca.isEmpty() ? "No" : "Yes"
        );

        newClient = esp_mqtt_client_init(&mqtt_cfg);
#if ESP_IDF_VERSION_MAJOR >= 5
        if (newClient) {
            esp_mqtt_client_register_event(newClient, MQTT_EVENT_ANY, mqtt_event_handler_idf5, nullptr);
        }
#endif
        mqttClient = newClient;
    } // Lock is safely released HERE before esp_mqtt_client_start!

    if (newClient) {
        esp_err_t err = esp_mqtt_client_start(newClient);
        if (err != ESP_OK) {
            log_e("[MQTT] Failed to start MQTT client (error %d)", err);
            mqtt_net_set_state(MQTT_STATE_ERROR, "Failed to start client");
        }
    } else {
        log_e("[MQTT] Failed to initialize MQTT client struct");
        mqtt_net_set_state(MQTT_STATE_ERROR, "Init failed");
    }
}

/**
 * Stops and destroys the connected client and updates all relevant state.
 */
static void mqtt_net_stop_client() {
    MqttLifecycleGuard lifecycleLock;
    if (!lifecycleLock.is_locked()) {
        return;
    }
    {
        MqttLockGuard lock;
        currentSession.is_active = false;
        mqtt_net_reset_grace_timer();
        mqtt_net_set_state(MQTT_STATE_DISABLED);
    }
    mqtt_net_destroy_client_with_lifecycle_lock();
    mqtt_net_set_state(MQTT_STATE_DISABLED);
}

// Starts or restarts the MQTT client using the active synced radio configuration
static void mqtt_net_start_synced_client() {
    String server;
    uint16_t port = 1883;
    String user = "";
    String pass = "";
    bool tls = false;
    bool tls_insec = false;
    String custom_ca = "";
    bool matchingClientActive = false;

    {
        MqttLockGuard lock;
        if (!currentConfig.enabled) {
            log_i("[MQTT] Refusing to start MQTT client, MQTT Gateway feature is disabled.");
            return;
        }
        if (!currentStatus.radio_proxy_enabled) {
            log_i("[MQTT] Refusing to start MQTT client, MQTT Proxy is disabled on radio.");
            return;
        }
        server = currentStatus.radio_server.isEmpty() ? MQTT_DEFAULT_SERVER : currentStatus.radio_server;
        port = currentStatus.radio_port;
        user = currentStatus.radio_user;
        pass = radioPassword;
        tls = currentStatus.radio_tls;
        tls_insec = currentConfig.tls_insecure;
        custom_ca = currentConfig.custom_ca;
        matchingClientActive = mqttClient && currentSession.is_active &&
            currentSession.server == server &&
            currentSession.port == port &&
            currentSession.user == user &&
            currentSession.pass == pass &&
            currentSession.root == currentStatus.radio_root &&
            currentSession.tls == tls &&
            currentSession.tls_insecure == tls_insec &&
            currentSession.custom_ca == custom_ca;
    }

    if (matchingClientActive) {
        log_d("[MQTT] Matching broker client is already active; keeping the existing lifecycle.");
        return;
    }
    mqtt_net_start_client(server, port, user, pass, tls, tls_insec, custom_ca);
}

// Remote administration is the primary service. When Tailscale is enabled,
// let its coordination/DERP TLS session settle before allocating a second TLS
// client for MQTT. Installations without Tailscale start MQTT immediately.
static void mqtt_net_start_or_defer() {
    if (tailscale_net_is_enabled() && !tailscale_net_is_derp_connected()) {
        s_start_deferred.store(true, std::memory_order_relaxed);
        mqtt_net_set_state(MQTT_STATE_WAITING_REMOTE_ACCESS);
        log_i("[MQTT] Waiting for Tailscale DERP before starting broker TLS...");
        return;
    }

    s_start_deferred.store(false, std::memory_order_relaxed);
    mqtt_net_start_synced_client();
}

void mqtt_net_init() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    initialized = true;

    if (!mqttMutex) {
        mqttMutex = xSemaphoreCreateRecursiveMutex();
    }
    if (!mqttLifecycleMutex) {
        mqttLifecycleMutex = xSemaphoreCreateMutex();
    }
#ifdef ESP_IDF5_MIGRATION_BUILD
    const int allocatorResult = mbedtls_platform_set_calloc_free(tls_psram_calloc, tls_psram_free);
    if (allocatorResult == 0) {
        log_i("[TLS] Large mbedTLS buffers use PSRAM; task stacks remain internal");
    } else {
        log_w("[TLS] Unable to install PSRAM-aware allocator (%d)", allocatorResult);
    }
#endif
#if ESP_IDF_VERSION_MAJOR >= 5
    esp_crt_bundle_set(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
#else
    arduino_esp_crt_bundle_set(rootca_crt_bundle_start);
#endif
}

static uint32_t mqtt_net_request_radio_config();

static void mqtt_net_radio_setup_loop() {
    MqttLockGuard lock(0);
    if (!lock.is_locked() || !radioSetupActive()) {
        return;
    }

    const uint32_t now = millis();
    if ((s_radio_setup_phase == RadioSetupPhase::WaitingConfig ||
         s_radio_setup_phase == RadioSetupPhase::WaitingSessionKey ||
         s_radio_setup_phase == RadioSetupPhase::Verifying) &&
        s_radio_setup_deadline != 0 &&
        static_cast<int32_t>(now - s_radio_setup_deadline) >= 0) {
        setRadioSetupState(RadioSetupPhase::Error, "The radio did not answer the setup verification in time.");
        log_e("[MQTT Setup] Timed out waiting for the radio");
        return;
    }

    if (s_radio_setup_phase == RadioSetupPhase::WaitingConfig ||
        s_radio_setup_phase == RadioSetupPhase::WaitingSessionKey) {
        return;
    }

    if (s_radio_setup_phase == RadioSetupPhase::Verifying) {
        if (s_radio_setup_config_id == 0 &&
            static_cast<int32_t>(now - s_radio_setup_next_action) >= 0 &&
            bridge_is_ble_connected()) {
            resetRadioSetupSnapshot();
            s_radio_setup_config_id = mqtt_net_request_radio_config();
            if (s_radio_setup_config_id != 0) {
                s_radio_setup_deadline = now + RADIO_SETUP_CONFIG_TIMEOUT_MS;
                currentStatus.radio_setup_detail = "Reading the radio back to verify every requested change.";
            }
        }
        return;
    }

    if (!bridge_is_ble_connected()) {
        setRadioSetupState(RadioSetupPhase::Error, "Bluetooth disconnected before the radio setup transaction was committed. Retry when connected.");
        return;
    }
    if (static_cast<int32_t>(now - s_radio_setup_next_action) < 0) {
        return;
    }

    const RadioSetupPhase sentPhase = s_radio_setup_phase;
    if (!queueRadioSetupStep()) {
        s_radio_setup_next_action = now + RADIO_SETUP_STEP_INTERVAL_MS;
        return;
    }

    switch (sentPhase) {
        case RadioSetupPhase::RequestSessionKey:
            setRadioSetupState(RadioSetupPhase::WaitingSessionKey, "Waiting for the radio's short-lived local admin session key.");
            s_radio_setup_deadline = now + RADIO_SETUP_CONFIG_TIMEOUT_MS;
            break;
        case RadioSetupPhase::BeginEdit:
            if (s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry) {
                setRadioSetupState(RadioSetupPhase::SetTelemetry, "Enabling built-in device telemetry while preserving all sensor telemetry settings.");
            } else {
                setRadioSetupState(RadioSetupPhase::SetMqtt, "Enabling MQTT and Proxy to Client while preserving broker settings.");
            }
            break;
        case RadioSetupPhase::SetMqtt:
            setRadioSetupState(RadioSetupPhase::SetLora, "Allowing outgoing packets to be marked for MQTT.");
            break;
        case RadioSetupPhase::SetTelemetry:
            setRadioSetupState(RadioSetupPhase::Commit, "Committing the device telemetry setting as one transaction.");
            break;
        case RadioSetupPhase::SetLora:
            s_radio_setup_channel_cursor = 0;
            setRadioSetupState(RadioSetupPhase::SetChannels, "Enabling the approved channel uplink/downlink selections.");
            break;
        case RadioSetupPhase::SetChannels:
            if (s_radio_setup_channel_cursor >= RADIO_SETUP_CHANNEL_COUNT) {
                setRadioSetupState(RadioSetupPhase::Commit, "Committing the radio settings as one transaction.");
            }
            break;
        case RadioSetupPhase::Commit:
            setRadioSetupState(RadioSetupPhase::Verifying, "Settings committed; waiting for the radio and verifying the read-back.");
            s_radio_setup_config_id = 0;
            s_radio_setup_deadline = now + RADIO_SETUP_CONFIG_TIMEOUT_MS;
            s_radio_setup_next_action = now + 1500;
            log_i("[MQTT Setup] Radio configuration transaction committed; verifying read-back");
            return;
        default:
            break;
    }
    s_radio_setup_next_action = now + RADIO_SETUP_STEP_INTERVAL_MS;
}

void mqtt_net_loop() {
    mqtt_net_radio_setup_loop();

    if (s_start_deferred.load(std::memory_order_relaxed)) {
        if (!tailscale_net_is_enabled() || tailscale_net_is_derp_connected()) {
            s_start_deferred.store(false, std::memory_order_relaxed);
            log_i("[MQTT] Remote access is ready. Starting broker connection...");
            mqtt_net_start_synced_client();
        } else {
            return;
        }
    }

    // Fast lock-free check: if not in grace period or 60s has not elapsed, return immediately.
    if (!s_in_grace_period.load(std::memory_order_relaxed) ||
        (millis() - s_ble_disconnected_at.load(std::memory_order_relaxed) < (uint32_t)MQTT_BLE_GRACE_PERIOD_SECONDS * 1000)) {
        return;
    }

    /*
     * Lock acquired only on expiration to serialize against simultaneous BLE reconnect
     */
    bool graceExpired = false;
    {
        MqttLockGuard lock;
        if (s_in_grace_period.load(std::memory_order_relaxed) && !bridge_is_ble_connected() &&
            millis() - s_ble_disconnected_at.load(std::memory_order_relaxed) >= (uint32_t)MQTT_BLE_GRACE_PERIOD_SECONDS * 1000) {
            log_w("[MQTT] BLE device has been disconnected for longer than grace period (%ds). Disconnecting from MQTT broker and clearing queue.", MQTT_BLE_GRACE_PERIOD_SECONDS);
            mqtt_net_reset_grace_timer();
            graceExpired = true;
        }
    }
    if (graceExpired) {
        mqtt_net_stop_client();
        bridge_clear_mqtt_to_ble_queue();
        mqtt_net_set_state(MQTT_STATE_WAITING_BLE, "BLE disconnected (grace expired)");
    }
}

/**
 * Asks the connected Meshtastic radio to send its configuration (ModuleConfig, NodeInfo, Channels)
 * by sending a ToRadio packet with a random want_config_id nonce.
 */
static uint32_t mqtt_net_request_radio_config() {
    meshtastic_ToRadio req = meshtastic_ToRadio_init_default;
    req.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    uint32_t nonce = esp_random();
    req.want_config_id = (nonce != 0) ? nonce : 1;

    uint8_t buf[16];
    pb_ostream_t stream = pb_ostream_from_buffer(buf, sizeof(buf));
    if (pb_encode(&stream, meshtastic_ToRadio_fields, &req)) {
        log_i("[MQTT] Requesting radio configuration (want_config_id: %u)...", req.want_config_id);
        if (bridge_enqueue_control_to_ble(buf, stream.bytes_written)) {
            return req.want_config_id;
        }
        log_w("[MQTT] Cannot request radio config: BLE control queue is unavailable or full.");
    } else {
        log_e("[MQTT] Failed to encode want_config_id protobuf: %s", PB_GET_ERROR(&stream));
    }
    return 0;
}

/**
 * Updates the MQTT Gateway configuration from the webui.
 * @param cfg The configuration to apply.
 */
void mqtt_net_apply_config(const MqttConfig& cfg) {
    mqtt_net_init();
    s_mqtt_enabled.store(cfg.enabled, std::memory_order_relaxed);
    bool hasRadioServer = false;
    bool radioProxyEnabled = false;
    bool setupInProgress = false;
    String radioServer;
    uint16_t radioPort = 1883;
    String radioRoot;
    {
        MqttLockGuard lock;
        currentConfig = cfg;
        setupInProgress = radioSetupActive();
        hasRadioServer = !currentStatus.radio_server.isEmpty();
        radioProxyEnabled = currentStatus.radio_proxy_enabled;
        radioServer = currentStatus.radio_server;
        radioPort = currentStatus.radio_port;
        radioRoot = currentStatus.radio_root;
    }

    if (!cfg.enabled) {
        s_start_deferred.store(false, std::memory_order_relaxed);
        log_i("[MQTT] Feature disabled in configuration, shutting down mqtt client...");
        mqtt_net_stop_client();
        bridge_clear_mqtt_to_ble_queue();
        return;
    }

    if (setupInProgress) {
        s_start_deferred.store(false, std::memory_order_relaxed);
        mqtt_net_stop_client();
        bridge_clear_mqtt_to_ble_queue();
        mqtt_net_set_state(MQTT_STATE_WAITING_RADIO_CONFIG,
                           "Verifying paired node configuration");
        return;
    }

    // The public MeshRadar standard is preloaded, so a fresh installation can
    // start without duplicating those shared values in the Web UI. A radio
    // ModuleConfig received later remains authoritative.
    if (!hasRadioServer) {
        log_i("[MQTT] Enabled. Waiting for radio ModuleConfig.mqtt packets over Bluetooth...");
        mqtt_net_set_state(MQTT_STATE_WAITING_RADIO_CONFIG);
        if (bridge_is_ble_connected()) {
            mqtt_net_request_radio_config();
        }
    } else if (!radioProxyEnabled) {
        s_start_deferred.store(false, std::memory_order_relaxed);
        log_w("[MQTT] Radio configuration detected, but MQTT proxy is inactive on the radio.");
        mqtt_net_stop_client();
        bridge_clear_mqtt_to_ble_queue();
        mqtt_net_set_state(MQTT_STATE_DISABLED, "Radio Proxy Disabled (Enable 'MQTT' & 'Proxy' in App)");
    } else if (bridge_is_ble_connected()) {
        log_i(
            "[MQTT] Applying Auto-Sync MQTT configuration -> %s:%u (Root: %s)",
            radioServer.c_str(), radioPort, radioRoot.c_str()
        );
        mqtt_net_start_or_defer();
    } else {
        log_i("[MQTT] Factory broker profile ready; Bluetooth is not required for broker startup.");
        mqtt_net_start_or_defer();
    }
}

bool mqtt_net_request_radio_setup(uint8_t uplink_mask, uint8_t downlink_mask, String& error) {
    mqtt_net_init();
    MqttLockGuard lock(pdMS_TO_TICKS(250));
    if (!lock.is_locked()) {
        error = "MQTT subsystem is busy; try again.";
        return false;
    }
    if (!bridge_is_ble_connected()) {
        error = "The Meshtastic radio is not connected over Bluetooth.";
        return false;
    }
    if (radioSetupActive()) {
        error = "Radio setup is already running.";
        return false;
    }
    if (uplink_mask == 0 && downlink_mask == 0) {
        error = "Select at least one channel for MQTT uplink or downlink.";
        return false;
    }
    if (!bridge_begin_radio_control()) {
        error = "PhoneAPI is busy with another maintenance operation; try again.";
        return false;
    }

    s_radio_setup_uplink_mask = uplink_mask;
    s_radio_setup_downlink_mask = downlink_mask;
    s_radio_setup_operation = RadioSetupOperation::Mqtt;
    s_radio_setup_channel_cursor = 0;
    resetRadioSetupSnapshot();
    setRadioSetupState(RadioSetupPhase::WaitingConfig, "Reading the current radio configuration before making changes.");
    s_radio_setup_config_id = mqtt_net_request_radio_config();
    if (s_radio_setup_config_id == 0) {
        setRadioSetupState(RadioSetupPhase::Error, "Could not queue the radio configuration request.");
        error = currentStatus.radio_setup_detail;
        return false;
    }
    s_radio_setup_deadline = millis() + RADIO_SETUP_CONFIG_TIMEOUT_MS;
    log_i(
        "[MQTT Setup] User-approved radio preparation started (uplink mask 0x%02x, downlink mask 0x%02x)",
        uplink_mask,
        downlink_mask
    );
    return true;
}

bool mqtt_net_request_radio_telemetry_setup(uint32_t interval_seconds, String& error) {
    mqtt_net_init();
    MqttLockGuard lock(pdMS_TO_TICKS(250));
    if (!lock.is_locked()) {
        error = "Radio configurator is busy; try again.";
        return false;
    }
    if (!bridge_is_ble_connected()) {
        error = "The Meshtastic radio is not connected over Bluetooth.";
        return false;
    }
    if (radioSetupActive()) {
        error = "Another radio setup is already running.";
        return false;
    }
    if (interval_seconds < 60 || interval_seconds > 86400) {
        error = "Telemetry interval must be between 60 and 86400 seconds.";
        return false;
    }
    if (!bridge_begin_radio_control()) {
        error = "PhoneAPI is busy with another maintenance operation; try again.";
        return false;
    }

    s_radio_setup_operation = RadioSetupOperation::DeviceTelemetry;
    s_radio_setup_telemetry_interval = interval_seconds;
    s_radio_setup_uplink_mask = 0;
    s_radio_setup_downlink_mask = 0;
    s_radio_setup_channel_cursor = 0;
    resetRadioSetupSnapshot();
    setRadioSetupState(RadioSetupPhase::WaitingConfig, "Reading the current telemetry configuration before making changes.");
    s_radio_setup_config_id = mqtt_net_request_radio_config();
    if (s_radio_setup_config_id == 0) {
        setRadioSetupState(RadioSetupPhase::Error, "Could not queue the radio configuration request.");
        error = currentStatus.radio_setup_detail;
        return false;
    }
    s_radio_setup_deadline = millis() + RADIO_SETUP_CONFIG_TIMEOUT_MS;
    log_i("[Radio Setup] User-approved device telemetry activation started (%lu seconds)",
          static_cast<unsigned long>(interval_seconds));
    return true;
}

/**
 * Check state of the mqtt subsystem.
 * NOTE: only tells if its enabled/disabled, not the connected state or anything like that.
 *
 * @return Returns true if MQTT Gateway feature is enabled (atomic, wait-free fast path)
 */
bool mqtt_net_is_enabled() {
    return s_mqtt_enabled.load(std::memory_order_relaxed);
}

void mqtt_net_pause_for_ota() {
    mqtt_net_init();
    bool expected = false;
    if (!s_paused_for_ota.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    {
        MqttLockGuard lock;
        s_config_before_ota = currentConfig;
        currentConfig.enabled = false;
        s_mqtt_enabled.store(false, std::memory_order_relaxed);
        s_start_deferred.store(false, std::memory_order_relaxed);
    }
    log_i("[MQTT] Pausing broker session to reserve TLS/heap resources for OTA...");
    mqtt_net_stop_client();
    bridge_clear_mqtt_to_ble_queue();
}

void mqtt_net_resume_after_ota() {
    bool expected = true;
    if (!s_paused_for_ota.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
        return;
    }

    MqttConfig saved;
    {
        MqttLockGuard lock;
        saved = s_config_before_ota;
    }
    log_i("[MQTT] OTA did not reboot; restoring broker configuration...");
    mqtt_net_apply_config(saved);
}

/**
 * Called when the BLE device is connected.
 */
void mqtt_net_on_ble_connected() {
    // The same Meshtastic configuration response supplies the attached node
    // number used by the installation-presence worker. Request it even
    // when the optional MQTT gateway itself is disabled.
    {
        MqttLockGuard lock;
        if (radioSetupActive()) {
            // A reconnect invalidates an in-flight complete-config stream. The
            // transaction owns PhoneAPI, so issue one fresh nonce instead of
            // racing the normal background discovery request.
            if (s_radio_setup_phase == RadioSetupPhase::WaitingConfig) {
                resetRadioSetupSnapshot();
                s_radio_setup_config_id = mqtt_net_request_radio_config();
                s_radio_setup_deadline = millis() + RADIO_SETUP_CONFIG_TIMEOUT_MS;
                currentStatus.radio_setup_detail =
                    "Bluetooth reconnected; reading a fresh radio configuration snapshot.";
            }
            return;
        }
    }
    mqtt_net_request_radio_config();

    // Fast wait-free check: no broker work if MQTT Gateway is disabled.
    if (!mqtt_net_is_enabled()) {
        return;
    }

    bool preserveSession = false;
    bool shouldStart = false;
    String server;
    uint16_t port = 1883;
    {
        MqttLockGuard lock;
        mqtt_net_reset_grace_timer();
        preserveSession = mqttClient && currentSession.is_active;
        shouldStart = !preserveSession && currentStatus.radio_proxy_enabled &&
            !currentStatus.radio_server.isEmpty() && !mqttClient;
        server = currentStatus.radio_server;
        port = currentStatus.radio_port;
    }

    // If we are already connected to the broker (during grace period) and session is active, keep it!
    if (preserveSession) {
        log_i("[MQTT] Bluetooth radio reconnected within grace period! Preserving active broker session.");
    } else if (shouldStart) {
        // Connect to broker if radio configuration is ready and proxy is enabled
        log_i(
            "[MQTT] Bluetooth radio connected! Connecting to Synced MQTT broker -> %s:%u",
            server.c_str(), port
        );
        mqtt_net_start_or_defer();
    }
}

/**
 * Called when the BLE device is disconnected.
 * It starts a timer where we queue messages coming from the mqtt broker while
 * waiting for the BLE device to reconnect.
 *
 * If the queue fills, we start dropping mqtt packets.
 * If the BLE device reconnects before the timer runs out, we push those messages to the BLE device.
 * If the timer expires, we disconnect from the mqtt broker and clear the queue.
 */
void mqtt_net_on_ble_disconnected() {
    MqttLockGuard lock;
    if (!currentConfig.enabled || !mqttClient) {
        return;
    }
    log_w("[MQTT] BLE disconnected. Starting %d-second grace timer...", MQTT_BLE_GRACE_PERIOD_SECONDS);
    s_ble_disconnected_at.store(millis(), std::memory_order_relaxed);
    s_in_grace_period.store(true, std::memory_order_relaxed);
}

/**
 * Sets the callback fired when a mqtt message comes in.
 * @param cb The callback to register
 */
void mqtt_net_set_downlink_callback(MqttDownlinkCallback cb) {
    MqttLockGuard lock;
    downlinkCallback = cb;
}

bool mqtt_net_publish(const char* topic, const uint8_t* payload, size_t len, bool retained) {
    MqttLockGuard lock;
    if (!mqttClient) {
        log_w("[MQTT] Unable to publish message, no mqttClient!");
        return false;
    }

    int msg_id = esp_mqtt_client_publish(mqttClient, topic, (const char*)payload, len, 0, retained ? 1 : 0);
    if (msg_id >= 0) {
        s_msgs_published.fetch_add(1, std::memory_order_relaxed);
        log_d("[MQTT] Published %zu bytes to '%s' (retained: %d)", len, topic, retained);
        return true;
    } else {
        log_e("[MQTT] Failed to publish message to topic '%s'", topic);
        return false;
    }
}

bool mqtt_net_handle_from_radio(const uint8_t* data, size_t len) {
    if (!data || len == 0) {
        return false;
    }

    // Decode FromRadio protobuf using Nanopb
    meshtastic_FromRadio radio_msg = meshtastic_FromRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data, len);

    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &radio_msg)) {
        // Not a valid FromRadio packet or decoding failed
        return false;
    }
    bool startVerifiedMqttClient = false;

    if (radio_msg.which_payload_variant == meshtastic_FromRadio_my_info_tag) {
        device_presence_set_meshtastic_node(radio_msg.my_info.my_node_num);
        MqttLockGuard lock;
        if (radioSetupActive()) {
            s_radio_setup_node_num = radio_msg.my_info.my_node_num;
        }
    }

    if (radio_msg.which_payload_variant == meshtastic_FromRadio_config_tag &&
        radio_msg.config.which_payload_variant == meshtastic_Config_lora_tag) {
        MqttLockGuard lock;
        if (radioSetupActive()) {
            s_radio_setup_lora = radio_msg.config.payload_variant.lora;
            s_radio_setup_have_lora = true;
        }
    }

    if (radio_msg.which_payload_variant == meshtastic_FromRadio_channel_tag &&
        radio_msg.channel.index >= 0 &&
        radio_msg.channel.index < RADIO_SETUP_CHANNEL_COUNT) {
        MqttLockGuard lock;
        if (radioSetupActive()) {
            const uint8_t channel = static_cast<uint8_t>(radio_msg.channel.index);
            s_radio_setup_channels[channel] = radio_msg.channel;
            s_radio_setup_have_channel[channel] = radio_msg.channel.has_settings &&
                radio_msg.channel.role != meshtastic_Channel_Role_DISABLED;
        }
    }

    if (radio_msg.which_payload_variant == meshtastic_FromRadio_moduleConfig_tag &&
        radio_msg.moduleConfig.which_payload_variant == meshtastic_ModuleConfig_telemetry_tag) {
        MqttLockGuard lock;
        if (radioSetupActive()) {
            s_radio_setup_telemetry = radio_msg.moduleConfig.payload_variant.telemetry;
            s_radio_setup_have_telemetry = true;
        }
    }

    if (radio_msg.which_payload_variant == meshtastic_FromRadio_packet_tag &&
        radio_msg.packet.which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
        radio_msg.packet.decoded.portnum == meshtastic_PortNum_ADMIN_APP &&
        radio_msg.packet.decoded.payload.size > 0) {
        MqttLockGuard lock;
        if (s_radio_setup_phase == RadioSetupPhase::WaitingSessionKey) {
            s_radio_setup_admin_response = meshtastic_AdminMessage_init_default;
            pb_istream_t adminStream = pb_istream_from_buffer(
                radio_msg.packet.decoded.payload.bytes,
                radio_msg.packet.decoded.payload.size
            );
            if (pb_decode(&adminStream, meshtastic_AdminMessage_fields, &s_radio_setup_admin_response) &&
                s_radio_setup_admin_response.session_passkey.size > 0) {
                s_radio_setup_session_key = s_radio_setup_admin_response.session_passkey;
                s_radio_setup_have_session_key = true;
                s_radio_setup_next_action = millis();
                setRadioSetupState(RadioSetupPhase::BeginEdit, "Authorized locally; starting one transactional radio update.");
                log_i("[MQTT Setup] Received short-lived radio admin session key");
            }
        }
    }

    if (radio_msg.which_payload_variant == meshtastic_FromRadio_config_complete_id_tag) {
        MqttLockGuard lock;
        if (radioSetupActive() && radio_msg.config_complete_id == s_radio_setup_config_id) {
            const uint8_t selectedMask = static_cast<uint8_t>(s_radio_setup_uplink_mask | s_radio_setup_downlink_mask);
            bool channelsReady = true;
            for (uint8_t channel = 0; channel < RADIO_SETUP_CHANNEL_COUNT; ++channel) {
                if ((selectedMask & static_cast<uint8_t>(1u << channel)) && !s_radio_setup_have_channel[channel]) {
                    channelsReady = false;
                }
            }

            if (s_radio_setup_phase == RadioSetupPhase::WaitingConfig) {
                const bool snapshotReady = s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry
                    ? (s_radio_setup_node_num != 0 && s_radio_setup_have_telemetry)
                    : (s_radio_setup_node_num != 0 && s_radio_setup_have_mqtt && s_radio_setup_have_lora && channelsReady);
                if (!snapshotReady) {
                    setRadioSetupState(
                        RadioSetupPhase::Error,
                        s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry
                            ? "The radio did not return its telemetry configuration."
                            : "The radio did not return every selected channel and required MQTT/LoRa setting."
                    );
                } else {
                    s_radio_setup_next_action = millis();
                    setRadioSetupState(RadioSetupPhase::RequestSessionKey, "Requesting a short-lived local admin session key from the radio.");
                }
            } else if (s_radio_setup_phase == RadioSetupPhase::Verifying) {
                bool verified = false;
                if (s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry) {
                    verified = s_radio_setup_have_telemetry &&
                        s_radio_setup_telemetry.device_telemetry_enabled &&
                        s_radio_setup_telemetry.device_update_interval == s_radio_setup_telemetry_interval;
                } else {
                    verified = s_radio_setup_have_mqtt &&
                        s_radio_setup_mqtt.enabled &&
                        s_radio_setup_mqtt.proxy_to_client_enabled &&
                        String(s_radio_setup_mqtt.address).equalsIgnoreCase(MQTT_DEFAULT_SERVER) &&
                        String(s_radio_setup_mqtt.username) == MQTT_DEFAULT_USER &&
                        String(s_radio_setup_mqtt.password) == MQTT_DEFAULT_PASSWORD &&
                        s_radio_setup_mqtt.encryption_enabled &&
                        !s_radio_setup_mqtt.json_enabled &&
                        s_radio_setup_mqtt.tls_enabled == MQTT_DEFAULT_TLS &&
                        String(s_radio_setup_mqtt.root) == MQTT_DEFAULT_ROOT &&
                        s_radio_setup_have_lora &&
                        s_radio_setup_lora.config_ok_to_mqtt &&
                        channelsReady;
                    for (uint8_t channel = 0; verified && channel < RADIO_SETUP_CHANNEL_COUNT; ++channel) {
                        const uint8_t bit = static_cast<uint8_t>(1u << channel);
                        if ((s_radio_setup_uplink_mask & bit) && !s_radio_setup_channels[channel].settings.uplink_enabled) {
                            verified = false;
                        }
                        if ((s_radio_setup_downlink_mask & bit) && !s_radio_setup_channels[channel].settings.downlink_enabled) {
                            verified = false;
                        }
                    }
                }
                if (verified) {
                    startVerifiedMqttClient =
                        s_radio_setup_operation == RadioSetupOperation::Mqtt;
                    setRadioSetupState(
                        RadioSetupPhase::Complete,
                        s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry
                            ? "Radio verified: device telemetry is enabled for periodic LoRa broadcasts at the selected interval."
                            : "Radio verified: MQTT, client proxy, LoRa consent and selected channels are ready."
                    );
                    log_i("[Radio Setup] Radio read-back verified successfully");
                } else {
                    setRadioSetupState(
                        RadioSetupPhase::Error,
                        s_radio_setup_operation == RadioSetupOperation::DeviceTelemetry
                            ? "Radio read-back did not confirm device telemetry. No sensor settings were overwritten."
                            : "Radio read-back did not contain every requested MQTT setting. No unrelated settings were changed."
                    );
                    log_e("[Radio Setup] Radio read-back verification failed");
                }
            }
        }
    }
    if (startVerifiedMqttClient) {
        mqtt_net_start_or_defer();
    }

    // 1. Check for ModuleConfig.mqtt packets (Auto-Sync from Radio)
    if (radio_msg.which_payload_variant == meshtastic_FromRadio_moduleConfig_tag) {
        if (radio_msg.moduleConfig.which_payload_variant == meshtastic_ModuleConfig_mqtt_tag) {
            const meshtastic_ModuleConfig_MQTTConfig& mqtt_cfg = radio_msg.moduleConfig.payload_variant.mqtt;
            log_i(
                "[MQTT] Discovered ModuleConfig.mqtt from Radio: enabled=%d, proxy_enabled=%d, server='%s', root='%s', tls=%d",
                mqtt_cfg.enabled,
                mqtt_cfg.proxy_to_client_enabled,
                mqtt_cfg.address,
                mqtt_cfg.root,
                mqtt_cfg.tls_enabled
            );

            bool shouldStart = false;
            bool shouldStop = false;
            const char* stopReason = nullptr;
            {
                MqttLockGuard lock;
                const bool setupInProgress = radioSetupActive();
                if (setupInProgress) {
                    s_radio_setup_mqtt = mqtt_cfg;
                    s_radio_setup_have_mqtt = true;
                }
                currentStatus.radio_config_received = true;
                String rawAddress = String(mqtt_cfg.address);
                const String rawUser = String(mqtt_cfg.username);
                const String rawPassword = String(mqtt_cfg.password);
                const bool meshtasticPublicDefaults =
                    rawAddress.equalsIgnoreCase(MESHTASTIC_PUBLIC_SERVER) &&
                    (rawUser.isEmpty() || rawUser == MESHTASTIC_PUBLIC_USER) &&
                    (rawPassword.isEmpty() || rawPassword == MESHTASTIC_PUBLIC_PASSWORD);
                const bool useMeshRadarPreset = rawAddress.isEmpty() ||
                    rawAddress.equalsIgnoreCase(MQTT_DEFAULT_SERVER) ||
                    meshtasticPublicDefaults;
                if (meshtasticPublicDefaults) {
                    log_i("[MQTT] Meshtastic public broker defaults detected; using the MeshRadar gateway preset.");
                }
                currentStatus.radio_proxy_enabled = (mqtt_cfg.enabled && mqtt_cfg.proxy_to_client_enabled);
                currentStatus.radio_root = useMeshRadarPreset ? MQTT_DEFAULT_ROOT : (String(mqtt_cfg.root).isEmpty() ? "msh" : String(mqtt_cfg.root));
                currentStatus.radio_tls = useMeshRadarPreset ? MQTT_DEFAULT_TLS : mqtt_cfg.tls_enabled;
                currentStatus.radio_user = useMeshRadarPreset ? MQTT_DEFAULT_USER : String(mqtt_cfg.username);
                radioPassword = useMeshRadarPreset ? MQTT_DEFAULT_PASSWORD : String(mqtt_cfg.password);

                String server = useMeshRadarPreset ? MQTT_DEFAULT_SERVER : rawAddress;
                uint16_t port = currentStatus.radio_tls ? 8883 : 1883;

                // If the radio address explicitly specifies a port (e.g. "160.16.104.222:1883" or "broker.local:8883")
                int colonIdx = server.indexOf(':');
                if (colonIdx > 0) {
                    String portStr = server.substring(colonIdx + 1);
                    server = server.substring(0, colonIdx);
                    int parsedPort = portStr.toInt();
                    if (parsedPort > 0 && parsedPort <= 65535) {
                        port = (uint16_t)parsedPort;
                    }
                }

                currentStatus.radio_server = server;
                currentStatus.radio_port = port;

                if (!setupInProgress) {
                    if (!currentStatus.radio_proxy_enabled) {
                        shouldStop = true;
                        stopReason = !mqtt_cfg.enabled
                            ? "MQTT Disabled on Radio (Enable 'MQTT' in App)"
                            : "Radio Proxy Disabled (Enable 'Proxy to Client' in App)";
                    } else if (currentConfig.enabled && bridge_is_ble_connected()) {
                        currentStatus.active_root = currentStatus.radio_root;

                        bool configChanged = (!currentSession.is_active ||
                                              currentSession.server != currentStatus.radio_server ||
                                              currentSession.port != currentStatus.radio_port ||
                                              currentSession.user != currentStatus.radio_user ||
                                              currentSession.pass != radioPassword ||
                                              currentSession.root != currentStatus.active_root ||
                                              currentSession.tls != currentStatus.radio_tls ||
                                              currentSession.tls_insecure != currentConfig.tls_insecure ||
                                              currentSession.custom_ca != currentConfig.custom_ca);

                        if (configChanged || !mqttClient) {
                            if (currentSession.is_active) {
                                log_i("[MQTT] Radio configuration changed while connected. Reconnecting broker...");
                                bridge_clear_mqtt_to_ble_queue();
                            }
                            shouldStart = true;
                        } else {
                            log_d("[MQTT] Received identical radio ModuleConfig.mqtt - maintaining existing session.");
                        }
                    }
                }
            }

            if (shouldStop) {
                log_w("[MQTT] Radio MQTT proxy is inactive (%s). Stopping client.", stopReason);
                mqtt_net_stop_client();
                bridge_clear_mqtt_to_ble_queue();
                mqtt_net_set_state(MQTT_STATE_DISABLED, stopReason);
                return false;
            } else if (shouldStart) {
                mqtt_net_start_or_defer();
            }
        }
    }

    // 2. Check for MqttClientProxyMessage (Uplink from Radio to MQTT)
    if (radio_msg.which_payload_variant == meshtastic_FromRadio_mqttClientProxyMessage_tag) {
        const meshtastic_MqttClientProxyMessage& proxy_msg = radio_msg.mqttClientProxyMessage;
        log_d("[MQTT] Intercepted MqttClientProxyMessage for topic '%s'", proxy_msg.topic);

        if (s_mqtt_enabled.load(std::memory_order_relaxed)) {
            if (proxy_msg.which_payload_variant == meshtastic_MqttClientProxyMessage_data_tag) {
                mqtt_net_publish(proxy_msg.topic, proxy_msg.payload_variant.data.bytes, proxy_msg.payload_variant.data.size, proxy_msg.retained);
            } else if (proxy_msg.which_payload_variant == meshtastic_MqttClientProxyMessage_text_tag) {
                size_t textLen = strlen(proxy_msg.payload_variant.text);
                mqtt_net_publish(proxy_msg.topic, (const uint8_t*)proxy_msg.payload_variant.text, textLen, proxy_msg.retained);
            }
            // Always consume when MQTT gateway is enabled so phone apps never receive raw proxy messages
            return true;
        }
    }

    return false;
}

MqttStatus mqtt_net_get_status() {
    MqttStatus st;
    {
        MqttLockGuard lock(pdMS_TO_TICKS(100));
        if (lock.is_locked()) {
            st = currentStatus;
        }
    }
    st.msgs_published = s_msgs_published.load(std::memory_order_relaxed);
    st.msgs_received = s_msgs_received.load(std::memory_order_relaxed);
    st.msgs_invalid = s_msgs_invalid.load(std::memory_order_relaxed);
    return st;
}
