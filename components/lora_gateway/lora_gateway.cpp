#include "lora_gateway.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lora_gateway {

static const char *const TAG = "lora_gateway";

// --- Auth helpers ---

std::vector<uint8_t> LoraGateway::sign_packet_(std::vector<uint8_t> body) {
  // Append sequence number (little-endian)
  uint16_t seq = this->tx_seq_++;
  body.push_back(static_cast<uint8_t>(seq & 0xFF));
  body.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));

  // Compute auth tag over body + seq
  uint16_t tag = lora_protocol::compute_auth_tag(this->auth_key_, body.data(), body.size());
  body.push_back(static_cast<uint8_t>(tag & 0xFF));
  body.push_back(static_cast<uint8_t>((tag >> 8) & 0xFF));

  return body;
}

bool LoraGateway::verify_packet_(const std::vector<uint8_t> &packet, uint16_t &seq_out) {
  if (packet.size() < lora_protocol::AUTH_OVERHEAD) {
    return false;
  }

  size_t body_plus_seq_len = packet.size() - lora_protocol::AUTH_TAG_SIZE;
  size_t body_len = packet.size() - lora_protocol::AUTH_OVERHEAD;

  // Extract sequence number
  seq_out = static_cast<uint16_t>(packet[body_len]) | (static_cast<uint16_t>(packet[body_len + 1]) << 8);

  // Extract received tag
  uint16_t received_tag =
      static_cast<uint16_t>(packet[body_plus_seq_len]) | (static_cast<uint16_t>(packet[body_plus_seq_len + 1]) << 8);

  // Compute expected tag over body + seq (everything except the tag itself)
  uint16_t expected_tag = lora_protocol::compute_auth_tag(this->auth_key_, packet.data(), body_plus_seq_len);

  return received_tag == expected_tag;
}

bool LoraGateway::check_seq_(RemoteNode *node, uint16_t seq) {
  if (!node->get_rx_seq_initialized()) {
    // First packet from this node — accept and initialize
    node->set_rx_seq(seq);
    node->set_rx_seq_initialized(true);
    return true;
  }

  uint16_t last = node->get_rx_seq();
  // Accept if seq is within [last+1, last+WINDOW_SIZE] (modular arithmetic)
  uint16_t diff = seq - last;  // wraps correctly for uint16_t
  if (diff >= 1 && diff <= lora_protocol::SEQ_WINDOW_SIZE) {
    node->set_rx_seq(seq);
    return true;
  }

  ESP_LOGW(TAG, "Sequence number rejected: got %u, expected after %u", seq, last);
  return false;
}

// --- RemoteNode implementation ---

sensor::Sensor *RemoteNode::get_or_create_sensor(const std::string &name) {
  auto it = this->sensors_.find(name);
  if (it != this->sensors_.end()) {
    return it->second;
  }

  if (this->sensors_.size() >= this->max_sensors_) {
    ESP_LOGE("lora_gateway",
             "Cannot create sensor '%s' for node %s (0x%02X): max_sensors_per_node (%zu) exceeded. Increase "
             "max_sensors_per_node in the gateway config.",
             name.c_str(), this->name_.c_str(), this->address_, this->max_sensors_);
    return nullptr;
  }

  auto *sens = new DynamicSensor(this->name_ + " " + name);
  App.register_sensor(sens);

  this->sensors_[name] = sens;
  ESP_LOGD("lora_gateway", "Created new sensor '%s' for node %s (0x%02X)", name.c_str(), this->name_.c_str(),
           this->address_);
  return sens;
}

binary_sensor::BinarySensor *RemoteNode::get_or_create_binary_sensor(const std::string &name) {
  auto it = this->binary_sensors_.find(name);
  if (it != this->binary_sensors_.end()) {
    return it->second;
  }

  if (this->binary_sensors_.size() >= this->max_binary_sensors_) {
    ESP_LOGE("lora_gateway",
             "Cannot create binary sensor '%s' for node %s (0x%02X): max_binary_sensors_per_node (%zu) exceeded. "
             "Increase max_binary_sensors_per_node in the gateway config.",
             name.c_str(), this->name_.c_str(), this->address_, this->max_binary_sensors_);
    return nullptr;
  }

  auto *sens = new DynamicBinarySensor(this->name_ + " " + name);
  App.register_binary_sensor(sens);

  this->binary_sensors_[name] = sens;
  ESP_LOGD("lora_gateway", "Created new binary sensor '%s' for node %s (0x%02X)", name.c_str(), this->name_.c_str(),
           this->address_);
  return sens;
}

// --- LoraGateway implementation ---

void LoraGateway::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Gateway...");

  if (this->remote_nodes_.empty()) {
    ESP_LOGW(TAG, "No remote nodes configured");
    return;
  }

  // Compute slot duration from response timeout
  this->slot_duration_ms_ = this->response_timeout_ms_ + lora_protocol::SLOT_MARGIN_MS;

  // Validate that poll_interval is sufficient for all slots
  uint32_t min_cycle = this->slot_duration_ms_ * this->remote_nodes_.size();
  if (this->poll_interval_ms_ < min_cycle) {
    ESP_LOGW(TAG, "poll_interval (%u ms) is shorter than minimum cycle duration (%u ms). Adjusting to %u ms.",
             this->poll_interval_ms_, min_cycle, min_cycle);
    this->poll_interval_ms_ = min_cycle;
  }

  // Register virtual devices for each remote node
  uint32_t device_id = 1;
  for (auto *node : this->remote_nodes_) {
    node->set_device_id(device_id++);
    ESP_LOGD(TAG, "Registered virtual device for node %s (0x%02X) with device_id %u", node->get_name().c_str(),
             node->get_address(), node->get_device_id());
  }

  // Initialize state
  this->current_poll_index_ = 0;
  this->waiting_for_response_ = false;
  this->last_poll_start_ms_ = 0;
  this->cycle_start_ms_ = 0;
  this->last_time_sync_ms_ = 0;

  ESP_LOGI(TAG, "Gateway configured: %d nodes, slot=%ums, cycle=%ums, poll_interval=%ums", this->remote_nodes_.size(),
           this->slot_duration_ms_, min_cycle, this->poll_interval_ms_);
}

void LoraGateway::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRa Gateway:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Response Timeout: %u ms", this->response_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Slot Duration: %u ms", this->slot_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Poll Interval: %u ms", this->poll_interval_ms_);
  if (this->time_sync_interval_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Time Sync Interval: %u ms", this->time_sync_interval_ms_);
  }
  ESP_LOGCONFIG(TAG, "  Send ACK: %s", this->send_ack_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Stale Sensor Behavior: %s",
                this->stale_behavior_ == StaleSensorBehavior::KEEP_LAST_VALUE ? "keep" : "invalidate");
  ESP_LOGCONFIG(TAG, "  Auth: SipHash-2-4 (16-byte key)");
  ESP_LOGCONFIG(TAG, "  Remote Nodes:");
  for (auto *node : this->remote_nodes_) {
    ESP_LOGCONFIG(TAG, "    - %s (0x%02X)", node->get_name().c_str(), node->get_address());
  }
}

void LoraGateway::loop() {
  if (!this->should_start_polling_()) {
    return;
  }

  uint32_t now = millis();

  // Handle response timeout
  if (this->waiting_for_response_) {
    if (now - this->last_poll_start_ms_ >= this->response_timeout_ms_) {
      this->handle_timeout_(this->current_node_);
      this->waiting_for_response_ = false;
    }
    return;
  }

  // Check if it's time to start a new cycle
  if (this->cycle_start_ms_ == 0 || now - this->cycle_start_ms_ >= this->poll_interval_ms_) {
    this->start_new_cycle_();
    return;
  }

  // Within a cycle: check if it's time for the next slot
  if (this->current_poll_index_ < this->remote_nodes_.size()) {
    uint32_t slot_start = this->cycle_start_ms_ + this->current_poll_index_ * this->slot_duration_ms_;
    if (now >= slot_start) {
      this->poll_next_node_();
    }
  }
}

bool LoraGateway::should_start_polling_() {
  if (this->time_ == nullptr) {
    return true;
  }
  auto now = this->time_->now();
  return now.is_valid();
}

void LoraGateway::start_new_cycle_() {
  this->cycle_start_ms_ = millis();
  this->current_poll_index_ = 0;

  // Broadcast time sync at the start of each cycle (if interval elapsed)
  if (this->time_sync_interval_ms_ > 0 && this->time_ != nullptr) {
    uint32_t now = millis();
    if (now - this->last_time_sync_ms_ >= this->time_sync_interval_ms_) {
      this->broadcast_time_sync_();
    }
  }
}

void LoraGateway::poll_next_node_() {
  if (this->remote_nodes_.empty()) {
    return;
  }

  this->current_node_ = this->remote_nodes_[this->current_poll_index_];
  this->send_poll_request_(this->current_node_);

  this->waiting_for_response_ = true;
  this->last_poll_start_ms_ = millis();
  this->current_poll_index_++;
}

void LoraGateway::send_poll_request_(RemoteNode *node) {
  std::vector<uint8_t> body;
  body.push_back(this->address_);
  body.push_back(node->get_address());
  body.push_back(lora_protocol::CMD_POLL_REQUEST);

  auto packet = this->sign_packet_(std::move(body));
  this->sx126x_->transmit_packet(packet);
  ESP_LOGD(TAG, "Sent poll request to node 0x%02X (%s) [seq=%u]", node->get_address(), node->get_name().c_str(),
           this->tx_seq_ - 1);
}

void LoraGateway::send_ack_packet_(RemoteNode *node) {
  if (!this->send_ack_) {
    return;
  }

  std::vector<uint8_t> body;
  body.push_back(this->address_);
  body.push_back(node->get_address());
  body.push_back(lora_protocol::CMD_ACK);

  auto packet = this->sign_packet_(std::move(body));
  this->sx126x_->transmit_packet(packet);
  ESP_LOGD(TAG, "Sent ACK to node 0x%02X", node->get_address());
}

void LoraGateway::broadcast_time_sync_() {
  if (this->time_ == nullptr) {
    return;
  }

  auto now_time = this->time_->now();
  if (!now_time.is_valid()) {
    ESP_LOGW(TAG, "Cannot broadcast time sync - time not valid");
    return;
  }

  uint32_t timestamp = now_time.timestamp;

  std::vector<uint8_t> body;
  body.push_back(this->address_);
  body.push_back(lora_protocol::BROADCAST_ADDRESS);
  body.push_back(lora_protocol::CMD_TIME_SYNC);

  // Timestamp (4 bytes LE)
  body.push_back(static_cast<uint8_t>(timestamp & 0xFF));
  body.push_back(static_cast<uint8_t>((timestamp >> 8) & 0xFF));
  body.push_back(static_cast<uint8_t>((timestamp >> 16) & 0xFF));
  body.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));

  // Poll interval (4 bytes LE)
  body.push_back(static_cast<uint8_t>(this->poll_interval_ms_ & 0xFF));
  body.push_back(static_cast<uint8_t>((this->poll_interval_ms_ >> 8) & 0xFF));
  body.push_back(static_cast<uint8_t>((this->poll_interval_ms_ >> 16) & 0xFF));
  body.push_back(static_cast<uint8_t>((this->poll_interval_ms_ >> 24) & 0xFF));

  // Slot duration (2 bytes LE)
  body.push_back(static_cast<uint8_t>(this->slot_duration_ms_ & 0xFF));
  body.push_back(static_cast<uint8_t>((this->slot_duration_ms_ >> 8) & 0xFF));

  // Node count
  body.push_back(static_cast<uint8_t>(this->remote_nodes_.size()));

  // Node addresses in poll order
  for (auto *node : this->remote_nodes_) {
    body.push_back(node->get_address());
  }

  auto packet = this->sign_packet_(std::move(body));
  this->sx126x_->transmit_packet(packet);
  this->last_time_sync_ms_ = millis();
  ESP_LOGD(TAG, "Broadcasted time sync: epoch=%u, poll_interval=%u, slot_duration=%u, nodes=%d", timestamp,
           this->poll_interval_ms_, this->slot_duration_ms_, this->remote_nodes_.size());
}

void LoraGateway::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // Minimum size: header (5) + auth overhead (4) = 9 bytes
  if (packet.size() < lora_protocol::POLL_RESPONSE_HEADER_SIZE + lora_protocol::AUTH_OVERHEAD) {
    ESP_LOGD(TAG, "Received packet too small (%d bytes), ignoring", packet.size());
    return;
  }

  // Check command byte (before auth strip — command is in the body)
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_POLL_RESPONSE) {
    ESP_LOGD(TAG, "Received non-poll-response packet (cmd=0x%02X), ignoring", packet[lora_protocol::OFFSET_COMMAND]);
    return;
  }

  // Check destination address
  uint8_t dst_addr = packet[lora_protocol::OFFSET_DST_ADDR];
  if (dst_addr != this->address_) {
    ESP_LOGD(TAG, "Received packet not addressed to this gateway (dst=0x%02X), ignoring", dst_addr);
    return;
  }

  // Verify authentication
  uint16_t seq;
  if (!this->verify_packet_(packet, seq)) {
    ESP_LOGW(TAG, "Received packet with invalid auth tag, dropping");
    return;
  }

  this->handle_poll_response_(packet, rssi, snr);
}

void LoraGateway::handle_poll_response_(const std::vector<uint8_t> &packet, float rssi, float snr) {
  uint8_t src_addr = packet[lora_protocol::OFFSET_SRC_ADDR];

  RemoteNode *node = this->find_node_by_address_(src_addr);
  if (node == nullptr) {
    ESP_LOGW(TAG, "Received response from unknown node 0x%02X", src_addr);
    return;
  }

  if (!this->waiting_for_response_) {
    ESP_LOGD(TAG, "Received late/duplicate response from node 0x%02X, ignoring", src_addr);
    return;
  }

  if (node != this->current_node_) {
    ESP_LOGD(TAG, "Received response from node 0x%02X but expecting 0x%02X, ignoring", src_addr,
             this->current_node_->get_address());
    return;
  }

  // Check sequence number (extract from verified packet)
  uint16_t seq;
  // Re-extract seq (we already verified the packet)
  size_t body_len = packet.size() - lora_protocol::AUTH_OVERHEAD;
  seq = static_cast<uint16_t>(packet[body_len]) | (static_cast<uint16_t>(packet[body_len + 1]) << 8);

  if (!this->check_seq_(node, seq)) {
    ESP_LOGW(TAG, "Replay detected from node 0x%02X, dropping", src_addr);
    return;
  }

  // Extract packet number and total packets from the body (before auth footer)
  uint8_t packet_num = packet[lora_protocol::OFFSET_PACKET_NUM];
  uint8_t total_packets = packet[lora_protocol::OFFSET_TOTAL_PACKETS];

  ESP_LOGD(TAG, "Received poll response from node 0x%02X (%s), packet %d/%d (RSSI: %.1f, SNR: %.1f)", src_addr,
           node->get_name().c_str(), packet_num == 0 ? 1 : packet_num, total_packets, rssi, snr);

  // Extract payload: between header and auth footer
  size_t payload_end = packet.size() - lora_protocol::AUTH_OVERHEAD;
  std::vector<uint8_t> payload(packet.begin() + lora_protocol::OFFSET_PAYLOAD, packet.begin() + payload_end);

  // Update metrics
  auto &metrics = node->get_metrics();
  metrics.last_response_received = true;
  metrics.last_heard_ms = millis();
  metrics.response_latency_ms = millis() - this->last_poll_start_ms_;
  metrics.last_rssi = rssi;
  metrics.last_snr = snr;

  // Handle single-packet vs multi-packet response
  if (packet_num == 0 && total_packets == 1) {
    this->waiting_for_response_ = false;
    this->process_complete_response_(node, payload);
    this->send_ack_packet_(node);
    this->update_metrics_sensors_();
  } else {
    auto &partial = this->partial_responses_[src_addr];

    if (partial.size() < total_packets) {
      partial.resize(total_packets);
    }

    if (packet_num > 0 && packet_num <= total_packets) {
      partial[packet_num - 1] = payload;
    } else {
      ESP_LOGW(TAG, "Invalid packet number %d (total: %d) from node 0x%02X", packet_num, total_packets, src_addr);
      return;
    }

    bool complete = true;
    for (const auto &p : partial) {
      if (p.empty()) {
        complete = false;
        break;
      }
    }

    if (complete) {
      std::vector<uint8_t> complete_payload;
      for (const auto &p : partial) {
        complete_payload.insert(complete_payload.end(), p.begin(), p.end());
      }

      this->partial_responses_.erase(src_addr);
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
  ESP_LOGD(TAG, "Processing complete response from node %s (0x%02X), payload size: %d bytes", node->get_name().c_str(),
           node->get_address(), payload.size());

  size_t offset = 0;
  int sensor_count = 0;
  int binary_sensor_count = 0;

  while (offset < payload.size()) {
    uint8_t key = payload[offset++];

    if (key == lora_protocol::SENSOR_KEY) {
      if (offset + 4 >= payload.size()) {
        ESP_LOGW(TAG, "Incomplete sensor data at offset %d", offset - 1);
        break;
      }

      float value;
      memcpy(&value, &payload[offset], sizeof(float));
      offset += 4;

      if (offset >= payload.size()) {
        ESP_LOGW(TAG, "Missing name length at offset %d", offset);
        break;
      }
      uint8_t name_len = payload[offset++];

      if (offset + name_len > payload.size()) {
        ESP_LOGW(TAG, "Incomplete name at offset %d (expected %d bytes, have %d)", offset, name_len,
                 payload.size() - offset);
        break;
      }
      std::string name(payload.begin() + offset, payload.begin() + offset + name_len);
      offset += name_len;

      auto *sens = node->get_or_create_sensor(name);
      if (sens != nullptr) {
        sens->publish_state(value);
        sensor_count++;
        ESP_LOGD(TAG, "  Sensor '%s' = %.2f", name.c_str(), value);
      }

    } else if (key == lora_protocol::BINARY_SENSOR_KEY) {
      if (offset >= payload.size()) {
        ESP_LOGW(TAG, "Missing binary sensor value at offset %d", offset - 1);
        break;
      }

      bool value = (payload[offset++] != 0);

      if (offset >= payload.size()) {
        ESP_LOGW(TAG, "Missing name length at offset %d", offset);
        break;
      }
      uint8_t name_len = payload[offset++];

      if (offset + name_len > payload.size()) {
        ESP_LOGW(TAG, "Incomplete name at offset %d (expected %d bytes, have %d)", offset, name_len,
                 payload.size() - offset);
        break;
      }
      std::string name(payload.begin() + offset, payload.begin() + offset + name_len);
      offset += name_len;

      auto *sens = node->get_or_create_binary_sensor(name);
      if (sens != nullptr) {
        sens->publish_state(value);
        binary_sensor_count++;
        ESP_LOGD(TAG, "  Binary Sensor '%s' = %s", name.c_str(), value ? "ON" : "OFF");
      }

    } else {
      ESP_LOGW(TAG, "Unknown key 0x%02X at offset %d, skipping rest of payload", key, offset - 1);
      break;
    }
  }

  ESP_LOGI(TAG, "Updated %d sensors and %d binary sensors from node %s (0x%02X)", sensor_count, binary_sensor_count,
           node->get_name().c_str(), node->get_address());
}

void LoraGateway::handle_timeout_(RemoteNode *node) {
  ESP_LOGW(TAG, "Timeout waiting for response from node 0x%02X (%s)", node->get_address(), node->get_name().c_str());

  auto &metrics = node->get_metrics();
  metrics.last_response_received = false;

  if (this->stale_behavior_ == StaleSensorBehavior::INVALIDATE) {
    auto &sensors = node->get_sensors();
    for (auto &kv : sensors) {
      if (kv.second != nullptr) {
        kv.second->publish_state(NAN);
      }
    }
    auto &binary_sensors = node->get_binary_sensors();
    for (auto &kv : binary_sensors) {
      if (kv.second != nullptr) {
        kv.second->publish_state(false);
      }
    }
    ESP_LOGD(TAG, "Invalidated %d sensors and %d binary sensors for node 0x%02X", sensors.size(), binary_sensors.size(),
             node->get_address());
  }

  this->partial_responses_.erase(node->get_address());
  this->update_metrics_sensors_();
}

void LoraGateway::update_metrics_sensors_() {
  if (this->timeout_list_sensor_ != nullptr) {
    std::string timeout_list;
    bool first = true;
    for (auto *node : this->remote_nodes_) {
      auto &metrics = node->get_metrics();
      if (!metrics.last_response_received) {
        if (!first) {
          timeout_list += ", ";
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "0x%02X: timeout", node->get_address());
        timeout_list += buf;
        first = false;
      }
    }
    if (timeout_list.empty()) {
      timeout_list = "none";
    }
    this->timeout_list_sensor_->publish_state(timeout_list);
  }

  if (this->last_heard_list_sensor_ != nullptr) {
    std::string last_heard_list;
    bool first = true;
    for (auto *node : this->remote_nodes_) {
      if (!first) {
        last_heard_list += ", ";
      }
      char buf[64];
      auto &metrics = node->get_metrics();
      if (metrics.last_heard_ms == 0) {
        snprintf(buf, sizeof(buf), "0x%02X: never", node->get_address());
      } else {
        snprintf(buf, sizeof(buf), "0x%02X: %u", node->get_address(), metrics.last_heard_ms);
      }
      last_heard_list += buf;
      first = false;
    }
    this->last_heard_list_sensor_->publish_state(last_heard_list);
  }

  if (this->signal_quality_list_sensor_ != nullptr) {
    std::string signal_quality_list;
    bool first = true;
    for (auto *node : this->remote_nodes_) {
      auto &metrics = node->get_metrics();
      if (metrics.last_heard_ms > 0) {
        if (!first) {
          signal_quality_list += ", ";
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "0x%02X: RSSI=%.1fdBm SNR=%.1fdB", node->get_address(), metrics.last_rssi,
                 metrics.last_snr);
        signal_quality_list += buf;
        first = false;
      }
    }
    if (signal_quality_list.empty()) {
      signal_quality_list = "no data";
    }
    this->signal_quality_list_sensor_->publish_state(signal_quality_list);
  }
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
