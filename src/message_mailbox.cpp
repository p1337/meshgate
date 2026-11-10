#include "message_mailbox.h"

#include <climits>
#include <cmath>
#include <atomic>
#include <LittleFS.h>
#include <pb_decode.h>
#include <pb_encode.h>

#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "utils.h"
#include "web_log.h"

namespace {

constexpr char MAILBOX_PATH[] = "/meshgate-mailbox.bin";
constexpr uint32_t MAILBOX_MAGIC = 0x5842474d;  // MGBX
constexpr uint32_t RECORD_MAGIC = 0x3147534d;   // MSG1
constexpr uint16_t MAILBOX_FORMAT_VERSION = 1;

struct __attribute__((packed)) MailboxHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint16_t record_size;
    uint16_t capacity;
    uint16_t head;
    uint16_t count;
    uint32_t next_sequence;
    uint32_t overwritten;
    uint32_t checksum;
};

struct __attribute__((packed)) MailboxRecord {
    uint32_t magic;
    uint32_t sequence;
    uint64_t fingerprint;
    uint32_t from;
    uint32_t to;
    uint32_t packet_id;
    uint32_t rx_time;
    int16_t rx_rssi;
    int16_t rx_snr_x4;
    uint8_t channel;
    uint8_t reserved;
    uint16_t raw_len;
    uint16_t text_len;
    uint8_t raw[MESHTASTIC_MAX_PACKET_SIZE];
    char text[MESSAGE_MAILBOX_TEXT_MAX];
    uint32_t checksum;
};

static_assert(MESSAGE_MAILBOX_CAPACITY > 0, "Mailbox capacity must be positive");
static_assert(MESSAGE_MAILBOX_CAPACITY <= UINT16_MAX, "Mailbox capacity exceeds on-flash format");

SemaphoreHandle_t s_mutex = nullptr;
MailboxHeader s_header = {};
std::atomic<bool> s_ready{false};
uint64_t* s_fingerprints = nullptr;
std::atomic<uint32_t> s_duplicates{0};
std::atomic<uint32_t> s_invalid{0};
MailboxRecord* s_read_record = nullptr;
MailboxRecord* s_write_record = nullptr;

uint32_t fnv1a32(const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

uint64_t fnv1a64Update(uint64_t hash, const void* data, size_t len) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint32_t headerChecksum(const MailboxHeader& header) {
    return fnv1a32(&header, offsetof(MailboxHeader, checksum));
}

uint32_t recordChecksum(const MailboxRecord& record) {
    return fnv1a32(&record, offsetof(MailboxRecord, checksum));
}

size_t expectedFileSize() {
    return sizeof(MailboxHeader) + MESSAGE_MAILBOX_CAPACITY * sizeof(MailboxRecord);
}

size_t recordOffset(uint16_t slot) {
    return sizeof(MailboxHeader) + static_cast<size_t>(slot) * sizeof(MailboxRecord);
}

bool headerValid(const MailboxHeader& header) {
    return header.magic == MAILBOX_MAGIC &&
           header.version == MAILBOX_FORMAT_VERSION &&
           header.header_size == sizeof(MailboxHeader) &&
           header.record_size == sizeof(MailboxRecord) &&
           header.capacity == MESSAGE_MAILBOX_CAPACITY &&
           header.head < MESSAGE_MAILBOX_CAPACITY &&
           header.count <= MESSAGE_MAILBOX_CAPACITY &&
           header.next_sequence != 0 &&
           header.checksum == headerChecksum(header);
}

bool recordValid(const MailboxRecord& record) {
    return record.magic == RECORD_MAGIC &&
           record.sequence != 0 &&
           record.raw_len > 0 &&
           record.raw_len <= MESHTASTIC_MAX_PACKET_SIZE &&
           record.text_len <= MESSAGE_MAILBOX_TEXT_MAX &&
           record.checksum == recordChecksum(record);
}

bool readRecord(File& file, uint16_t slot, MailboxRecord& record) {
    if (!file.seek(recordOffset(slot), SeekSet)) {
        return false;
    }
    return file.read(reinterpret_cast<uint8_t*>(&record), sizeof(record)) == sizeof(record) &&
           recordValid(record);
}

bool writeHeader(File& file) {
    s_header.checksum = headerChecksum(s_header);
    if (!file.seek(0, SeekSet)) {
        return false;
    }
    return file.write(reinterpret_cast<const uint8_t*>(&s_header), sizeof(s_header)) == sizeof(s_header);
}

bool createEmptyMailboxLocked() {
    File file = LittleFS.open(MAILBOX_PATH, "w");
    if (!file) {
        return false;
    }

    memset(&s_header, 0, sizeof(s_header));
    s_header.magic = MAILBOX_MAGIC;
    s_header.version = MAILBOX_FORMAT_VERSION;
    s_header.header_size = sizeof(MailboxHeader);
    s_header.record_size = sizeof(MailboxRecord);
    s_header.capacity = MESSAGE_MAILBOX_CAPACITY;
    s_header.next_sequence = 1;

    bool ok = writeHeader(file);
    MailboxRecord& empty = *s_read_record;
    memset(&empty, 0, sizeof(empty));
    for (uint16_t i = 0; ok && i < MESSAGE_MAILBOX_CAPACITY; ++i) {
        ok = file.write(reinterpret_cast<const uint8_t*>(&empty), sizeof(empty)) == sizeof(empty);
    }
    file.flush();
    file.close();

    memset(s_fingerprints, 0, MESSAGE_MAILBOX_CAPACITY * sizeof(uint64_t));
    return ok;
}

uint16_t oldestSlotLocked() {
    return static_cast<uint16_t>(
        (s_header.head + MESSAGE_MAILBOX_CAPACITY - s_header.count) % MESSAGE_MAILBOX_CAPACITY
    );
}

uint32_t oldestSequenceLocked() {
    return s_header.next_sequence - s_header.count;
}

bool readSequenceLocked(File& file, uint32_t sequence, MailboxRecord& record) {
    if (s_header.count == 0) {
        return false;
    }
    const uint32_t oldest = oldestSequenceLocked();
    if (sequence < oldest || sequence >= s_header.next_sequence) {
        return false;
    }
    const uint32_t offset = sequence - oldest;
    const uint16_t slot = static_cast<uint16_t>((oldestSlotLocked() + offset) % MESSAGE_MAILBOX_CAPACITY);
    return readRecord(file, slot, record) && record.sequence == sequence;
}

bool fingerprintKnownLocked(uint64_t fingerprint) {
    for (uint16_t i = 0; i < MESSAGE_MAILBOX_CAPACITY; ++i) {
        if (s_fingerprints[i] == fingerprint) {
            return true;
        }
    }
    return false;
}

void rebuildFingerprintsLocked(File& file) {
    memset(s_fingerprints, 0, MESSAGE_MAILBOX_CAPACITY * sizeof(uint64_t));
    for (uint16_t slot = 0; slot < MESSAGE_MAILBOX_CAPACITY; ++slot) {
        memset(s_read_record, 0, sizeof(*s_read_record));
        if (readRecord(file, slot, *s_read_record)) {
            s_fingerprints[slot] = s_read_record->fingerprint;
        }
    }
}

uint64_t messageFingerprint(const meshtastic_MeshPacket& packet) {
    uint64_t hash = 14695981039346656037ULL;
    hash = fnv1a64Update(hash, &packet.from, sizeof(packet.from));
    hash = fnv1a64Update(hash, &packet.to, sizeof(packet.to));
    hash = fnv1a64Update(hash, &packet.id, sizeof(packet.id));
    hash = fnv1a64Update(hash, &packet.rx_time, sizeof(packet.rx_time));
    hash = fnv1a64Update(hash, &packet.channel, sizeof(packet.channel));
    hash = fnv1a64Update(hash, &packet.decoded.portnum, sizeof(packet.decoded.portnum));
    hash = fnv1a64Update(hash, packet.decoded.payload.bytes, packet.decoded.payload.size);
    return hash;
}

bool nextRecordLocked(uint32_t* cursor, uint32_t end) {
    if (!cursor) {
        return false;
    }

    bool found = false;
    File file = LittleFS.open(MAILBOX_PATH, "r");
    if (file) {
        uint32_t current = *cursor;
        const uint32_t oldest = oldestSequenceLocked();
        if (current < oldest) {
            current = oldest;
        }
        const uint32_t availableEnd = min(end, s_header.next_sequence);
        while (current < availableEnd) {
            memset(s_read_record, 0, sizeof(*s_read_record));
            if (readSequenceLocked(file, current, *s_read_record)) {
                *cursor = current + 1;
                found = true;
                break;
            }
            ++current;
            *cursor = current;
        }
        file.close();
    }
    return found;
}

}  // namespace

bool message_mailbox_init() {
    if (s_ready.load(std::memory_order_relaxed)) {
        return true;
    }
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
    if (!s_fingerprints) {
        s_fingerprints = static_cast<uint64_t*>(calloc(MESSAGE_MAILBOX_CAPACITY, sizeof(uint64_t)));
    }
    if (!s_read_record) {
        s_read_record = static_cast<MailboxRecord*>(calloc(1, sizeof(MailboxRecord)));
    }
    if (!s_write_record) {
        s_write_record = static_cast<MailboxRecord*>(calloc(1, sizeof(MailboxRecord)));
    }
    if (!s_mutex || !s_fingerprints || !s_read_record || !s_write_record) {
        log_e("[Mailbox] Could not allocate storage state");
        return false;
    }
    if (!LittleFS.begin(true)) {
        log_e("[Mailbox] Could not mount the data partition");
        return false;
    }

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }

    bool valid = false;
    File file = LittleFS.open(MAILBOX_PATH, "r");
    if (file && file.size() == expectedFileSize() &&
        file.read(reinterpret_cast<uint8_t*>(&s_header), sizeof(s_header)) == sizeof(s_header) &&
        headerValid(s_header)) {
        valid = true;
        rebuildFingerprintsLocked(file);
    }
    if (file) {
        file.close();
    }

    if (!valid) {
        log_i("[Mailbox] Creating persistent %u-message ring", MESSAGE_MAILBOX_CAPACITY);
        valid = createEmptyMailboxLocked();
    }
    s_ready.store(valid, std::memory_order_relaxed);
    xSemaphoreGive(s_mutex);

    if (s_ready.load(std::memory_order_relaxed)) {
        log_i("[Mailbox] Ready: %u/%u messages retained", s_header.count, MESSAGE_MAILBOX_CAPACITY);
    } else {
        log_e("[Mailbox] Persistent store initialization failed");
    }
    return s_ready.load(std::memory_order_relaxed);
}

bool message_mailbox_is_ready() {
    return s_ready.load(std::memory_order_relaxed);
}

bool message_mailbox_capture_from_radio(const uint8_t* data, size_t len) {
    if (!s_ready.load(std::memory_order_relaxed) || !data || len == 0 || len > MESHTASTIC_MAX_PACKET_SIZE) {
        return false;
    }

    // This path runs only in bridgeBleTask. Keeping the generated protobuf
    // union and flash record static protects that task's 8 KiB stack.
    meshtastic_FromRadio fromRadio = meshtastic_FromRadio_init_default;
    pb_istream_t stream = pb_istream_from_buffer(data, len);
    if (!pb_decode(&stream, meshtastic_FromRadio_fields, &fromRadio) ||
        fromRadio.which_payload_variant != meshtastic_FromRadio_packet_tag ||
        fromRadio.packet.which_payload_variant != meshtastic_MeshPacket_decoded_tag ||
        fromRadio.packet.decoded.portnum != meshtastic_PortNum_TEXT_MESSAGE_APP) {
        return false;
    }

    const meshtastic_MeshPacket& packet = fromRadio.packet;
    MailboxRecord& record = *s_write_record;
    memset(&record, 0, sizeof(record));
    record.magic = RECORD_MAGIC;
    record.fingerprint = messageFingerprint(packet);
    if (record.fingerprint == 0) {
        record.fingerprint = 1;
    }
    record.from = packet.from;
    record.to = packet.to;
    record.packet_id = packet.id;
    record.rx_time = packet.rx_time;
    record.rx_rssi = static_cast<int16_t>(constrain(packet.rx_rssi, INT16_MIN, INT16_MAX));
    record.rx_snr_x4 = static_cast<int16_t>(constrain(lroundf(packet.rx_snr * 4.0f), INT16_MIN, INT16_MAX));
    record.channel = packet.channel;
    record.text_len = static_cast<uint16_t>(
        packet.decoded.payload.size < MESSAGE_MAILBOX_TEXT_MAX
            ? packet.decoded.payload.size
            : MESSAGE_MAILBOX_TEXT_MAX
    );
    memcpy(record.text, packet.decoded.payload.bytes, record.text_len);

    // FromRadio.id belongs to the radio FIFO session. Replaying an old FIFO id
    // after a fresh config handshake can look like a transport gap to clients.
    // Re-encode the same MeshPacket with id=0; the inner packet id, sender,
    // channel, timestamps and payload remain unchanged for app deduplication.
    fromRadio.id = 0;
    fromRadio.packet.delayed = packet.to == UINT32_MAX
        ? meshtastic_MeshPacket_Delayed_DELAYED_BROADCAST
        : meshtastic_MeshPacket_Delayed_DELAYED_DIRECT;
    pb_ostream_t output = pb_ostream_from_buffer(record.raw, sizeof(record.raw));
    if (!pb_encode(&output, meshtastic_FromRadio_fields, &fromRadio) || output.bytes_written == 0) {
        s_invalid.fetch_add(1, std::memory_order_relaxed);
        log_w("[Mailbox] Could not encode replay-safe FromRadio message");
        return false;
    }
    record.raw_len = static_cast<uint16_t>(output.bytes_written);

    if (xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (fingerprintKnownLocked(record.fingerprint)) {
        s_duplicates.fetch_add(1, std::memory_order_relaxed);
        xSemaphoreGive(s_mutex);
        return false;
    }

    record.sequence = s_header.next_sequence;
    record.checksum = recordChecksum(record);

    bool committed = false;
    const uint16_t writtenSlot = s_header.head;
    File file = LittleFS.open(MAILBOX_PATH, "r+");
    if (file && file.seek(recordOffset(s_header.head), SeekSet) &&
        file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record)) {
        s_header.head = static_cast<uint16_t>((s_header.head + 1) % MESSAGE_MAILBOX_CAPACITY);
        if (s_header.count < MESSAGE_MAILBOX_CAPACITY) {
            ++s_header.count;
        } else {
            ++s_header.overwritten;
        }
        ++s_header.next_sequence;
        if (s_header.next_sequence == 0) {
            // A wrap would make sequence ordering ambiguous. Start a fresh ring;
            // reaching this requires billions of stored messages.
            file.close();
            committed = createEmptyMailboxLocked();
        } else {
            committed = writeHeader(file);
            file.flush();
        }
    }
    if (file) {
        file.close();
    }

    if (committed && s_header.count > 0) {
        s_fingerprints[writtenSlot] = record.fingerprint;
    } else {
        s_invalid.fetch_add(1, std::memory_order_relaxed);
        log_e("[Mailbox] Failed to commit received text message");
    }
    xSemaphoreGive(s_mutex);
    return committed;
}

MessageMailboxStats message_mailbox_get_stats() {
    MessageMailboxStats stats = {};
    stats.ready = s_ready.load(std::memory_order_relaxed);
    stats.capacity = MESSAGE_MAILBOX_CAPACITY;
    stats.duplicates = s_duplicates.load(std::memory_order_relaxed);
    stats.invalid = s_invalid.load(std::memory_order_relaxed);
    if (!stats.ready || !s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return stats;
    }
    stats.count = s_header.count;
    stats.oldest_sequence = s_header.count ? oldestSequenceLocked() : s_header.next_sequence;
    stats.next_sequence = s_header.next_sequence;
    stats.overwritten = s_header.overwritten;
    xSemaphoreGive(s_mutex);
    return stats;
}

void message_mailbox_get_replay_bounds(uint32_t* cursor, uint32_t* end) {
    if (!cursor || !end) {
        return;
    }
    *cursor = 0;
    *end = 0;
    if (!s_ready.load(std::memory_order_relaxed) || !s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return;
    }
    *cursor = s_header.count ? oldestSequenceLocked() : s_header.next_sequence;
    *end = s_header.next_sequence;
    xSemaphoreGive(s_mutex);
}

bool message_mailbox_next_raw(
    uint32_t* cursor,
    uint32_t end,
    uint8_t* data,
    size_t capacity,
    size_t* len
) {
    if (!data || !len) {
        return false;
    }
    if (!cursor || !s_ready.load(std::memory_order_relaxed) || !s_mutex ||
        xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool found = nextRecordLocked(cursor, end) && s_read_record->raw_len <= capacity;
    if (found) {
        memcpy(data, s_read_record->raw, s_read_record->raw_len);
        *len = s_read_record->raw_len;
    }
    xSemaphoreGive(s_mutex);
    return found;
}

bool message_mailbox_next_entry(uint32_t* cursor, uint32_t end, MessageMailboxEntry* entry) {
    if (!entry) {
        return false;
    }
    if (!cursor || !s_ready.load(std::memory_order_relaxed) || !s_mutex ||
        xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    if (!nextRecordLocked(cursor, end)) {
        xSemaphoreGive(s_mutex);
        return false;
    }
    memset(entry, 0, sizeof(*entry));
    entry->sequence = s_read_record->sequence;
    entry->from = s_read_record->from;
    entry->to = s_read_record->to;
    entry->packet_id = s_read_record->packet_id;
    entry->rx_time = s_read_record->rx_time;
    entry->rx_rssi = s_read_record->rx_rssi;
    entry->rx_snr_x4 = s_read_record->rx_snr_x4;
    entry->channel = s_read_record->channel;
    entry->text_len = s_read_record->text_len;
    memcpy(entry->text, s_read_record->text, s_read_record->text_len);
    entry->text[s_read_record->text_len] = '\0';
    xSemaphoreGive(s_mutex);
    return true;
}

bool message_mailbox_clear() {
    if (!s_ready.load(std::memory_order_relaxed) || !s_mutex || xSemaphoreTake(s_mutex, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool ok = createEmptyMailboxLocked();
    s_ready.store(ok, std::memory_order_relaxed);
    s_duplicates.store(0, std::memory_order_relaxed);
    s_invalid.store(0, std::memory_order_relaxed);
    xSemaphoreGive(s_mutex);
    if (ok) {
        log_i("[Mailbox] Persistent message history cleared");
    }
    return ok;
}
