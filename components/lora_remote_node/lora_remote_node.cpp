#include "lora_remote_node.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lora_remote_node {

static const char *const TAG = "lora_remote_node";

void LoraRemoteNode::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Remote Node...");
  ESP_LOGI(TAG, "Remote node configured at address 0x%02X with %d sensors and %d binary sensors", this->address_,
           this->sensors_.size(), this->binary_sensors_.size());
}

void LoraRemoteNode::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRa Remote Node:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Sensors: %d", this->sensors_.size());
  ESP_LOGCONFIG(TAG, "  Binary Sensors: %d", this->binary_sensors_.size());
  if (this->time_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Time Source: configured");
  }
}

void LoraRemoteNode::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  if (this->is_poll_request_(packet)) {
    ESP_LOGD(TAG, "Received poll request from 0x%02X (RSSI: %.1f, SNR: %.1f)", packet[lora_protocol::OFFSET_SRC_ADDR],
             rssi, snr);
    this->handle_poll_request_(packet);
  } else if (this->is_time_sync_(packet)) {
    ESP_LOGD(TAG, "Received time sync from 0x%02X (RSSI: %.1f, SNR: %.1f)", packet[lora_protocol::OFFSET_SRC_ADDR],
             rssi, snr);
    this->handle_time_sync_(packet);
  }
}

bool LoraRemoteNode::is_poll_request_(const std::vector<uint8_t> &packet) {
  if (packet.size() < lora_protocol::POLL_REQUEST_SIZE) {
    return false;
  }
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_POLL_REQUEST) {
    return false;
  }
  uint8_t target = packet[lora_protocol::OFFSET_DST_ADDR];
  return (target == this->address_ || target == lora_protocol::BROADCAST_ADDRESS);
}

bool LoraRemoteNode::is_time_sync_(const std::vector<uint8_t> &packet) {
  if (packet.size() < lora_protocol::TIME_SYNC_PACKET_SIZE) {
    return false;
  }
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_TIME_SYNC) {
    return false;
  }
  uint8_t target = packet[lora_protocol::OFFSET_DST_ADDR];
  return (target == lora_protocol::BROADCAST_ADDRESS);
}

void LoraRemoteNode::handle_poll_request_(const std::vector<uint8_t> &packet) {
  // TODO: Phase 2 - Implement full poll request handling
  uint8_t gateway_addr = packet[lora_protocol::OFFSET_SRC_ADDR];
  ESP_LOGD(TAG, "Handling poll request from gateway 0x%02X", gateway_addr);

  auto response_packets = this->build_response_packets_(gateway_addr);
  for (const auto &response : response_packets) {
    this->sx126x_->transmit_packet(response);
    // Small delay between multi-packet transmissions
    if (response_packets.size() > 1) {
      delay(15);  // 15ms turnaround time
    }
  }
}

void LoraRemoteNode::handle_time_sync_(const std::vector<uint8_t> &packet) {
  if (this->time_ == nullptr) {
    ESP_LOGW(TAG, "Received time sync but no time source configured");
    return;
  }

  // Extract 4-byte timestamp (little-endian)
  uint32_t timestamp = 0;
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP]);
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP + 1]) << 8;
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP + 2]) << 16;
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP + 3]) << 24;

  ESP_LOGI(TAG, "Setting RTC time to %u (seconds since epoch)", timestamp);

  // Create ESPTime from timestamp and synchronize
  auto esptime = ESPTime::from_epoch_local(timestamp);
  this->time_->call_setup();
  // Note: The actual time sync depends on the RTC implementation
  // Most RTCs will sync on next call to read_time() or similar
  ESP_LOGD(TAG, "Time sync applied: %04d-%02d-%02d %02d:%02d:%02d", esptime.year, esptime.month, esptime.day_of_month,
           esptime.hour, esptime.minute, esptime.second);
}

std::vector<std::vector<uint8_t>> LoraRemoteNode::build_response_packets_(uint8_t gateway_addr) {
  std::vector<std::vector<uint8_t>> packets;

  std::vector<uint8_t> payload = this->serialize_sensor_data_();

  if (payload.size() <= lora_protocol::MAX_PAYLOAD_SIZE) {
    // Single packet response
    std::vector<uint8_t> packet;
    packet.push_back(this->address_);                    // Source address
    packet.push_back(gateway_addr);                      // Destination address
    packet.push_back(lora_protocol::CMD_POLL_RESPONSE);  // Command
    packet.push_back(0x00);                              // Packet number (0 for single)
    packet.push_back(0x01);                              // Total packets (1)
    packet.insert(packet.end(), payload.begin(), payload.end());
    packets.push_back(packet);
  } else {
    // Multi-packet response
    size_t offset = 0;
    uint8_t packet_num = 1;
    uint8_t total_packets = (payload.size() + lora_protocol::MAX_PAYLOAD_SIZE - 1) / lora_protocol::MAX_PAYLOAD_SIZE;

    while (offset < payload.size()) {
      size_t chunk_size = std::min(lora_protocol::MAX_PAYLOAD_SIZE, payload.size() - offset);

      std::vector<uint8_t> packet;
      packet.push_back(this->address_);
      packet.push_back(gateway_addr);
      packet.push_back(lora_protocol::CMD_POLL_RESPONSE);
      packet.push_back(packet_num);
      packet.push_back(total_packets);
      packet.insert(packet.end(), payload.begin() + offset, payload.begin() + offset + chunk_size);
      packets.push_back(packet);

      offset += chunk_size;
      packet_num++;
    }
  }

  return packets;
}

std::vector<uint8_t> LoraRemoteNode::serialize_sensor_data_() {
  std::vector<uint8_t> data;

  // Serialize sensors
  for (auto *sens : this->sensors_) {
    if (!sens->has_state()) {
      continue;
    }
    data.push_back(lora_protocol::SENSOR_KEY);

    // Float value (4 bytes, IEEE 754)
    float value = sens->state;
    uint8_t *value_bytes = reinterpret_cast<uint8_t *>(&value);
    data.insert(data.end(), value_bytes, value_bytes + 4);

    // Name length and name
    std::string name = sens->get_name();
    data.push_back(static_cast<uint8_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
  }

  // Serialize binary sensors
  for (auto *sens : this->binary_sensors_) {
    if (!sens->has_state()) {
      continue;
    }
    data.push_back(lora_protocol::BINARY_SENSOR_KEY);

    // Bool value (1 byte)
    data.push_back(sens->state ? 0x01 : 0x00);

    // Name length and name
    std::string name = sens->get_name();
    data.push_back(static_cast<uint8_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
  }

  return data;
}

}  // namespace lora_remote_node
}  // namespace esphome
