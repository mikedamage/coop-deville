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

// Packet structure constants
static const size_t MAX_PACKET_SIZE = 255;
static const size_t POLL_REQUEST_SIZE = 3;
static const size_t POLL_RESPONSE_HEADER_SIZE = 5;
static const size_t TIME_SYNC_PACKET_SIZE = 7;
static const size_t ACK_PACKET_SIZE = 3;
static const size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - POLL_RESPONSE_HEADER_SIZE;

// Packet structure offsets
static const size_t OFFSET_SRC_ADDR = 0;
static const size_t OFFSET_DST_ADDR = 1;
static const size_t OFFSET_COMMAND = 2;
static const size_t OFFSET_PACKET_NUM = 3;
static const size_t OFFSET_TOTAL_PACKETS = 4;
static const size_t OFFSET_PAYLOAD = 5;

// Time sync packet offsets
static const size_t OFFSET_TIME_SYNC_TIMESTAMP = 3;

}  // namespace lora_protocol
}  // namespace esphome
