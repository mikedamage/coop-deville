#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace lora_protocol {

// Protocol commands (both unicast; the former CMD_TIME_SYNC/CMD_ACK are removed)
static const uint8_t CMD_POLL_REQUEST = 0x01;
static const uint8_t CMD_POLL_RESPONSE = 0x02;

// Payload record tags
static const uint8_t SENSOR_KEY = 0x01;              // [0x01][index][float32 LE]  (6 bytes)
static const uint8_t BINARY_SENSOR_KEY = 0x02;       // [0x02][index][value]        (3 bytes)
static const uint8_t COMMAND_ACK_KEY = 0x03;         // [0x03][cmd_id:2][result]    (downlink, planned)
static const uint8_t SCHEMA_FINGERPRINT_KEY = 0x04;  // [0x04][fingerprint:2 LE]   (3 bytes)

// Sensor record sizes (tag included)
static const size_t FLOAT_RECORD_SIZE = 6;
static const size_t BINARY_RECORD_SIZE = 3;
static const size_t FINGERPRINT_RECORD_SIZE = 3;

// 0xFF is reserved/invalid as an address — there are no broadcast frames.
static const uint8_t RESERVED_ADDRESS = 0xFF;

// Poll Request flags (byte 3)
static const uint8_t POLL_FLAG_TIME_PRESENT = 0x01;    // epoch-time block follows poll_interval
static const uint8_t POLL_FLAG_RESPONSE_ACKED = 0x02;  // gateway received node's previous response

// Poll Response flags (byte 5)
static const uint8_t RESP_FLAG_TIME_REQUEST = 0x01;  // node requests a wall-clock time update

// Authentication
static const size_t AUTH_KEY_SIZE = 16;  // 128-bit SipHash key
static const size_t EPOCH_SIZE = 2;      // 16-bit boot epoch
static const size_t SEQ_NUM_SIZE = 2;    // 16-bit sequence number
static const size_t AUTH_TAG_SIZE = 4;   // 32-bit truncated SipHash tag
static const size_t AUTH_OVERHEAD =
    EPOCH_SIZE + SEQ_NUM_SIZE + AUTH_TAG_SIZE;  // 8 bytes total: [epoch:2][seq:2][tag:4]

// Anti-replay acceptance uses the modular forward half-space (is_forward_u16);
// there is no fixed window-size constant.

// Maximum consecutive missed polls before falling back to continuous RX
static const uint8_t MAX_MISSED_POLLS = 5;

// Packet structure constants
static const size_t MAX_PACKET_SIZE = 255;
static const size_t POLL_RESPONSE_HEADER_SIZE = 6;  // [src][dst][cmd][pkt_num][total][flags]
static const size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - POLL_RESPONSE_HEADER_SIZE - AUTH_OVERHEAD;  // 241 bytes

// Poll request fixed fields and minimum size.
// Layout: [src][dst][cmd][flags][poll_interval:4] (+[time:4] if TIME_PRESENT) [cmd_count][envelopes...]
static const size_t POLL_INTERVAL_SIZE = 4;
static const size_t TIME_BLOCK_SIZE = 4;
// header(3) + flags(1) + poll_interval(4) + cmd_count(1) + auth(8)
static const size_t POLL_REQUEST_MIN_SIZE = 3 + 1 + POLL_INTERVAL_SIZE + 1 + AUTH_OVERHEAD;  // 17 bytes

// Packet structure offsets (common header)
static const size_t OFFSET_SRC_ADDR = 0;
static const size_t OFFSET_DST_ADDR = 1;
static const size_t OFFSET_COMMAND = 2;

// Poll request offsets
static const size_t OFFSET_POLL_FLAGS = 3;
static const size_t OFFSET_POLL_INTERVAL = 4;  // bytes 4-7 (uint32_t LE)
static const size_t OFFSET_POLL_OPTIONAL = 8;  // time block (if present) or cmd_count begins here

// Poll response offsets
static const size_t OFFSET_PACKET_NUM = 3;
static const size_t OFFSET_TOTAL_PACKETS = 4;
static const size_t OFFSET_RESPONSE_FLAGS = 5;
static const size_t OFFSET_PAYLOAD = 6;

// Scheduled polling: margin added to response_timeout to compute slot duration
static const uint32_t SLOT_MARGIN_MS = 200;

// Default guard window for remote node listen windows (ms)
// Derived from DS3231 ±3.5ppm drift (negligible) + ESP32 loop jitter (~±16ms) + 10ms padding
static const uint32_t DEFAULT_GUARD_WINDOW_MS = 50;

// Modular forward comparison for anti-replay counters (epoch and seq).
// True if `candidate` is strictly forward of `reference` within the uint16
// modular half-space — i.e. (candidate - reference) mod 2^16 is in [1, 2^15].
// Wraparound-safe: handles 0xFFFF -> 0x0000 as an ordinary +1 step. Never use a
// plain `>` on epoch/seq, which would brick replay state at the wrap boundary.
static inline bool is_forward_u16(uint16_t candidate, uint16_t reference) {
  uint16_t delta = static_cast<uint16_t>(candidate - reference);
  return delta >= 1u && delta <= 0x8000u;
}

}  // namespace lora_protocol
}  // namespace esphome
