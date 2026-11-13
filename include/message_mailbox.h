#pragma once

#include <Arduino.h>

#include "build_options.h"

struct MessageMailboxStats {
    bool ready;
    uint32_t count;
    uint32_t capacity;
    uint32_t oldest_sequence;
    uint32_t next_sequence;
    uint32_t overwritten;
    uint32_t duplicates;
    uint32_t invalid;
};

struct MessageMailboxEntry {
    uint32_t sequence;
    bool outgoing;
    uint32_t from;
    uint32_t to;
    uint32_t packet_id;
    uint32_t rx_time;
    int16_t rx_rssi;
    int16_t rx_snr_x4;
    uint8_t channel;
    uint16_t text_len;
    char text[MESSAGE_MAILBOX_TEXT_MAX + 1];
};

constexpr size_t MESSAGE_MAILBOX_CHANNEL_NAME_MAX = 24;
constexpr size_t MESSAGE_MAILBOX_NODE_LONG_NAME_MAX = 40;
constexpr size_t MESSAGE_MAILBOX_NODE_SHORT_NAME_MAX = 5;

// Runtime metadata learned from the radio's normal configuration stream.
// Channel keys are deliberately never copied into or exposed by this API.
struct MessageMailboxChannel {
    uint8_t index;
    bool primary;
    bool public_default;
    char name[MESSAGE_MAILBOX_CHANNEL_NAME_MAX];
};

struct MessageMailboxNode {
    uint32_t num;
    char long_name[MESSAGE_MAILBOX_NODE_LONG_NAME_MAX];
    char short_name[MESSAGE_MAILBOX_NODE_SHORT_NAME_MAX];
};

// Mounts the existing data partition and opens (or creates) the persistent
// bounded mailbox. It never touches NVS, Wi-Fi, BLE, VPN or OTA data.
bool message_mailbox_init();
bool message_mailbox_is_ready();

// Inspects a raw FromRadio protobuf and stores decoded text messages. Returns
// true only when a new message was committed; non-message envelopes are ignored.
bool message_mailbox_capture_from_radio(const uint8_t* data, size_t len);

// Inspects a raw ToRadio protobuf after it was accepted by the BLE
// characteristic and stores decoded text messages as replay-only sent
// history. The generated FromRadio envelope is returned to PhoneAPI clients;
// it is never written back to the radio or transmitted over LoRa.
bool message_mailbox_capture_to_radio(const uint8_t* data, size_t len);

MessageMailboxStats message_mailbox_get_stats();

// A replay is a stable half-open sequence range [cursor, end). New packets
// arriving after get_replay_bounds() are left for the next app connection.
void message_mailbox_get_replay_bounds(uint32_t* cursor, uint32_t* end);
bool message_mailbox_next_raw(
    uint32_t* cursor,
    uint32_t end,
    uint8_t* data,
    size_t capacity,
    size_t* len
);
bool message_mailbox_next_entry(uint32_t* cursor, uint32_t end, MessageMailboxEntry* entry);

// Bounded snapshots used by the local channel-aware chat UI. These values are
// repopulated after every BLE configuration handshake and are not persisted.
size_t message_mailbox_list_channels(MessageMailboxChannel* channels, size_t capacity);
bool message_mailbox_get_channel(uint8_t index, MessageMailboxChannel* channel);
bool message_mailbox_get_node(uint32_t num, MessageMailboxNode* node);
bool message_mailbox_get_local_node(MessageMailboxNode* node);

// Builds a normal Meshtastic ToRadio TEXT_MESSAGE_APP broadcast. The caller
// still has to enqueue it through bridge_enqueue_http_to_ble(); this function
// does not transmit anything and never handles channel keys.
bool message_mailbox_build_text_to_radio(
    uint8_t channel,
    const uint8_t* text,
    size_t text_len,
    uint8_t* output,
    size_t output_capacity,
    size_t* output_len,
    uint32_t* packet_id
);

// Removes mailbox contents only. This is used by the explicit factory-reset
// path; ordinary reboots and OTA updates preserve the file.
bool message_mailbox_clear();
