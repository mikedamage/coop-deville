#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace lora_protocol {

// Protocol commands
static const uint8_t CMD_POLL_REQUEST = 0x01;
static const uint8_t CMD_POLL_RESPONSE = 0x02;
static const uint8_t CMD_TIME_SYNC = 0x03;
static const uint8_t CMD_ACK = 0x04;

// Payload keys (compatible with packet_transport serialization)
static const uint8_t SENSOR_KEY = 0x01;
static const uint8_t BINARY_SENSOR_KEY = 0x02;

// Special addresses
static const uint8_t BROADCAST_ADDRESS = 0xFF;

// Authentication
static const size_t AUTH_KEY_SIZE = 16;                            // 128-bit SipHash key
static const size_t AUTH_TAG_SIZE = 2;                             // 16-bit truncated SipHash tag
static const size_t SEQ_NUM_SIZE = 2;                              // 16-bit sequence number
static const size_t AUTH_OVERHEAD = SEQ_NUM_SIZE + AUTH_TAG_SIZE;  // 4 bytes total

// Sequence number anti-replay window
static const uint16_t SEQ_WINDOW_SIZE = 256;

// Maximum consecutive missed polls before falling back to continuous RX
static const uint8_t MAX_MISSED_POLLS = 5;

// Packet structure constants
static const size_t MAX_PACKET_SIZE = 255;
static const size_t POLL_RESPONSE_HEADER_SIZE = 5;
static const size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - POLL_RESPONSE_HEADER_SIZE - AUTH_OVERHEAD;  // 246 bytes

// Authenticated packet sizes (body + seq + tag)
static const size_t POLL_REQUEST_SIZE = 3 + AUTH_OVERHEAD;  // 7 bytes
static const size_t ACK_PACKET_SIZE = 3 + AUTH_OVERHEAD;    // 7 bytes

// Packet structure offsets (common header)
static const size_t OFFSET_SRC_ADDR = 0;
static const size_t OFFSET_DST_ADDR = 1;
static const size_t OFFSET_COMMAND = 2;

// Poll response offsets
static const size_t OFFSET_PACKET_NUM = 3;
static const size_t OFFSET_TOTAL_PACKETS = 4;
static const size_t OFFSET_PAYLOAD = 5;

// Time sync packet layout:
// [src][dst][cmd][timestamp:4][poll_interval:4][slot_duration:2][node_count:1][addresses:N][seq:2][tag:2]
static const size_t OFFSET_TIME_SYNC_TIMESTAMP = 3;
static const size_t OFFSET_TIME_SYNC_POLL_INTERVAL = 7;
static const size_t OFFSET_TIME_SYNC_SLOT_DURATION = 11;
static const size_t OFFSET_TIME_SYNC_NODE_COUNT = 13;
static const size_t OFFSET_TIME_SYNC_NODE_LIST = 14;
static const size_t TIME_SYNC_HEADER_SIZE = 14;  // fixed bytes before the variable-length node list

// Scheduled polling: margin added to response_timeout to compute slot duration
static const uint32_t SLOT_MARGIN_MS = 200;

// Default guard window for remote node listen windows (ms)
// Derived from DS3231 ±3.5ppm drift (negligible) + ESP32 loop jitter (~±16ms) + 10ms padding
static const uint32_t DEFAULT_GUARD_WINDOW_MS = 50;

}  // namespace lora_protocol
}  // namespace esphome
