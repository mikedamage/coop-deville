#include "lora_gateway.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lora_gateway {

static const char *const TAG = "lora_gateway";

// RemoteNode implementation

sensor::Sensor *RemoteNode::get_or_create_sensor(const std::string &name) {
  auto it = this->sensors_.find(name);
  if (it != this->sensors_.end()) {
    return it->second;
  }
  // TODO: Phase 4 - Create and register new sensor with device association
  return nullptr;
}

binary_sensor::BinarySensor *RemoteNode::get_or_create_binary_sensor(const std::string &name) {
  auto it = this->binary_sensors_.find(name);
  if (it != this->binary_sensors_.end()) {
    return it->second;
  }
  // TODO: Phase 4 - Create and register new binary sensor with device association
  return nullptr;
}

// LoraGateway implementation

void LoraGateway::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Gateway...");

  if (this->remote_nodes_.empty()) {
    ESP_LOGW(TAG, "No remote nodes configured");
    return;
  }

  // Initialize state
  this->current_poll_index_ = 0;
  this->waiting_for_response_ = false;
  this->last_poll_start_ms_ = 0;
  this->last_poll_cycle_ms_ = 0;
  this->last_time_sync_ms_ = 0;

  ESP_LOGI(TAG, "Gateway configured with %d remote nodes", this->remote_nodes_.size());
}

void LoraGateway::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRa Gateway:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Response Timeout: %u ms", this->response_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Poll Interval: %u ms", this->poll_interval_ms_);
  if (this->time_sync_interval_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Time Sync Interval: %u ms", this->time_sync_interval_ms_);
  }
  ESP_LOGCONFIG(TAG, "  Send ACK: %s", this->send_ack_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Stale Sensor Behavior: %s",
                this->stale_behavior_ == StaleSensorBehavior::KEEP_LAST_VALUE ? "keep" : "invalidate");
  ESP_LOGCONFIG(TAG, "  Remote Nodes:");
  for (auto *node : this->remote_nodes_) {
    ESP_LOGCONFIG(TAG, "    - %s (0x%02X)", node->get_name().c_str(), node->get_address());
  }
}

void LoraGateway::loop() {
  // Wait for time to be valid before starting polling (if time source configured)
  if (!this->should_start_polling_()) {
    return;
  }

  uint32_t now = millis();

  // Check if time sync interval has elapsed
  if (this->time_sync_interval_ms_ > 0 && this->time_ != nullptr) {
    if (now - this->last_time_sync_ms_ >= this->time_sync_interval_ms_) {
      this->broadcast_time_sync_();
    }
  }

  // Handle response timeout
  if (this->waiting_for_response_) {
    if (now - this->last_poll_start_ms_ >= this->response_timeout_ms_) {
      // Timeout occurred
      this->handle_timeout_(this->current_node_);
      this->waiting_for_response_ = false;
    }
    // Still waiting for response, don't start new poll
    return;
  }

  // Check if poll interval has elapsed since last poll cycle started
  if (now - this->last_poll_cycle_ms_ >= this->poll_interval_ms_) {
    this->poll_next_node_();
  }
}

bool LoraGateway::should_start_polling_() {
  if (this->time_ == nullptr) {
    return true;
  }
  // Wait for time to be valid before starting polling
  auto now = this->time_->now();
  return now.is_valid();
}

void LoraGateway::poll_next_node_() {
  if (this->remote_nodes_.empty()) {
    return;
  }

  // Get the next node to poll
  this->current_node_ = this->remote_nodes_[this->current_poll_index_];

  // Send poll request
  this->send_poll_request_(this->current_node_);

  // Update state
  this->waiting_for_response_ = true;
  this->last_poll_start_ms_ = millis();

  // Advance to next node for the next poll cycle
  this->current_poll_index_ = (this->current_poll_index_ + 1) % this->remote_nodes_.size();

  // If we've wrapped around, update the poll cycle timestamp
  if (this->current_poll_index_ == 0) {
    this->last_poll_cycle_ms_ = millis();
  }
}

void LoraGateway::send_poll_request_(RemoteNode *node) {
  std::vector<uint8_t> packet;
  packet.push_back(this->address_);                   // Gateway address
  packet.push_back(node->get_address());              // Target node address
  packet.push_back(lora_protocol::CMD_POLL_REQUEST);  // Command

  this->sx126x_->transmit_packet(packet);
  ESP_LOGD(TAG, "Sent poll request to node 0x%02X (%s)", node->get_address(), node->get_name().c_str());
}

void LoraGateway::send_ack_packet_(RemoteNode *node) {
  if (!this->send_ack_) {
    return;
  }

  std::vector<uint8_t> packet;
  packet.push_back(this->address_);          // Gateway address
  packet.push_back(node->get_address());     // Target node address
  packet.push_back(lora_protocol::CMD_ACK);  // Command

  this->sx126x_->transmit_packet(packet);
  ESP_LOGD(TAG, "Sent ACK to node 0x%02X", node->get_address());
}

void LoraGateway::broadcast_time_sync_() {
  if (this->time_ == nullptr) {
    return;
  }

  auto now = this->time_->now();
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "Cannot broadcast time sync - time not valid");
    return;
  }

  uint32_t timestamp = now.timestamp;

  std::vector<uint8_t> packet;
  packet.push_back(this->address_);                    // Gateway address
  packet.push_back(lora_protocol::BROADCAST_ADDRESS);  // Broadcast to all nodes
  packet.push_back(lora_protocol::CMD_TIME_SYNC);      // Command
  // Add timestamp in little-endian format
  packet.push_back(static_cast<uint8_t>(timestamp & 0xFF));
  packet.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
  packet.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
  packet.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));

  this->sx126x_->transmit_packet(packet);
  this->last_time_sync_ms_ = millis();
  ESP_LOGD(TAG, "Broadcasted time sync: %u", timestamp);
}

void LoraGateway::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // Validate minimum packet size
  if (packet.size() < lora_protocol::POLL_RESPONSE_HEADER_SIZE) {
    ESP_LOGD(TAG, "Received packet too small (%d bytes), ignoring", packet.size());
    return;
  }

  // Check if this is a poll response
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_POLL_RESPONSE) {
    ESP_LOGD(TAG, "Received non-poll-response packet (cmd=0x%02X), ignoring", packet[lora_protocol::OFFSET_COMMAND]);
    return;
  }

  // Check if packet is addressed to this gateway
  uint8_t dst_addr = packet[lora_protocol::OFFSET_DST_ADDR];
  if (dst_addr != this->address_) {
    ESP_LOGD(TAG, "Received packet not addressed to this gateway (dst=0x%02X), ignoring", dst_addr);
    return;
  }

  // Handle the poll response
  this->handle_poll_response_(packet, rssi, snr);
}

void LoraGateway::handle_poll_response_(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // Extract source address
  uint8_t src_addr = packet[lora_protocol::OFFSET_SRC_ADDR];

  // Find the node
  RemoteNode *node = this->find_node_by_address_(src_addr);
  if (node == nullptr) {
    ESP_LOGW(TAG, "Received response from unknown node 0x%02X", src_addr);
    return;
  }

  // Check if we're waiting for a response
  if (!this->waiting_for_response_) {
    ESP_LOGD(TAG, "Received late/duplicate response from node 0x%02X, ignoring", src_addr);
    return;
  }

  // Check if this is from the node we're currently polling
  if (node != this->current_node_) {
    ESP_LOGD(TAG, "Received response from node 0x%02X but expecting 0x%02X, ignoring", src_addr,
             this->current_node_->get_address());
    return;
  }

  // Extract packet number and total packets
  uint8_t packet_num = packet[lora_protocol::OFFSET_PACKET_NUM];
  uint8_t total_packets = packet[lora_protocol::OFFSET_TOTAL_PACKETS];

  ESP_LOGD(TAG, "Received poll response from node 0x%02X (%s), packet %d/%d (RSSI: %.1f, SNR: %.1f)", src_addr,
           node->get_name().c_str(), packet_num == 0 ? 1 : packet_num, total_packets, rssi, snr);

  // Extract payload
  std::vector<uint8_t> payload(packet.begin() + lora_protocol::OFFSET_PAYLOAD, packet.end());

  // Update metrics
  auto &metrics = node->get_metrics();
  metrics.last_response_received = true;
  metrics.last_heard_ms = millis();
  metrics.response_latency_ms = millis() - this->last_poll_start_ms_;
  metrics.last_rssi = rssi;
  metrics.last_snr = snr;

  // Handle single-packet vs multi-packet response
  if (packet_num == 0 && total_packets == 1) {
    // Single packet response - process immediately
    this->waiting_for_response_ = false;
    this->process_complete_response_(node, payload);
    this->send_ack_packet_(node);
    this->update_metrics_sensors_();
  } else {
    // Multi-packet response
    auto &partial = this->partial_responses_[src_addr];

    // Ensure we have enough space for all packets
    if (partial.size() < total_packets) {
      partial.resize(total_packets);
    }

    // Store this packet's payload (packet_num is 1-indexed for multi-packet)
    if (packet_num > 0 && packet_num <= total_packets) {
      partial[packet_num - 1] = payload;
    } else {
      ESP_LOGW(TAG, "Invalid packet number %d (total: %d) from node 0x%02X", packet_num, total_packets, src_addr);
      return;
    }

    // Check if we have all packets
    bool complete = true;
    for (const auto &p : partial) {
      if (p.empty()) {
        complete = false;
        break;
      }
    }

    if (complete) {
      // Concatenate all payloads
      std::vector<uint8_t> complete_payload;
      for (const auto &p : partial) {
        complete_payload.insert(complete_payload.end(), p.begin(), p.end());
      }

      // Clear partial responses
      this->partial_responses_.erase(src_addr);

      // Process complete response
      this->waiting_for_response_ = false;
      this->process_complete_response_(node, complete_payload);
      this->send_ack_packet_(node);
      this->update_metrics_sensors_();

      ESP_LOGD(TAG, "Multi-packet response complete from node 0x%02X (%d bytes total)", src_addr,
               complete_payload.size());
    } else {
      ESP_LOGD(TAG, "Waiting for more packets from node 0x%02X", src_addr);
    }
  }
}

void LoraGateway::process_complete_response_(RemoteNode *node, const std::vector<uint8_t> &payload) {
  // TODO: Phase 4 - Implement sensor deserialization and update
}

void LoraGateway::handle_timeout_(RemoteNode *node) {
  ESP_LOGW(TAG, "Timeout waiting for response from node 0x%02X (%s)", node->get_address(), node->get_name().c_str());

  auto &metrics = node->get_metrics();
  metrics.last_response_received = false;

  // Handle stale sensor behavior
  if (this->stale_behavior_ == StaleSensorBehavior::INVALIDATE) {
    // TODO: Phase 4 - Invalidate all sensors for this node
    ESP_LOGD(TAG, "Would invalidate sensors for node 0x%02X (not implemented yet)", node->get_address());
  }

  // Clear any partial responses from this node
  this->partial_responses_.erase(node->get_address());

  this->update_metrics_sensors_();
}

void LoraGateway::update_metrics_sensors_() {
  // TODO: Phase 5 - Implement metrics sensor updates
}

RemoteNode *LoraGateway::find_node_by_address_(uint8_t address) {
  for (auto *node : this->remote_nodes_) {
    if (node->get_address() == address) {
      return node;
    }
  }
  return nullptr;
}

}  // namespace lora_gateway
}  // namespace esphome
