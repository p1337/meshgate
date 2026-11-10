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

// Mounts the existing data partition and opens (or creates) the persistent
// bounded mailbox. It never touches NVS, Wi-Fi, BLE, VPN or OTA data.
bool message_mailbox_init();
bool message_mailbox_is_ready();

// Inspects a raw FromRadio protobuf and stores decoded text messages. Returns
// true only when a new message was committed; non-message envelopes are ignored.
bool message_mailbox_capture_from_radio(const uint8_t* data, size_t len);

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

// Removes mailbox contents only. This is used by the explicit factory-reset
// path; ordinary reboots and OTA updates preserve the file.
bool message_mailbox_clear();
