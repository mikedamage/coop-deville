#include "lora_gateway.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace lora_gateway {

static const char *const TAG = "lora_gateway";

// --- Auth helpers ---

std::vector<uint8_t> LoraGateway::sign_packet_(RemoteNode *node, std::vector<uint8_t> body) {
  // Append boot epoch (little-endian)
  body.push_back(static_cast<uint8_t>(this->boot_epoch_ & 0xFF));
  body.push_back(static_cast<uint8_t>((this->boot_epoch_ >> 8) & 0xFF));

  // Append the destination node's per-node sequence number (little-endian)
  uint16_t seq = node->next_tx_seq();
  body.push_back(static_cast<uint8_t>(seq & 0xFF));
  body.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));

  // Compute 32-bit auth tag over body + epoch + seq, append little-endian
  uint32_t tag = lora_protocol::compute_auth_tag(this->auth_key_, body.data(), body.size());
  body.push_back(static_cast<uint8_t>(tag & 0xFF));
  body.push_back(static_cast<uint8_t>((tag >> 8) & 0xFF));
  body.push_back(static_cast<uint8_t>((tag >> 16) & 0xFF));
  body.push_back(static_cast<uint8_t>((tag >> 24) & 0xFF));

  return body;
}

bool LoraGateway::verify_packet_(const std::vector<uint8_t> &packet, uint16_t &epoch_out, uint16_t &seq_out) {
  if (packet.size() < lora_protocol::AUTH_OVERHEAD) {
    return false;
  }

  size_t body_plus_counter_len = packet.size() - lora_protocol::AUTH_TAG_SIZE;
  size_t counter_off = packet.size() - lora_protocol::AUTH_OVERHEAD;  // start of [epoch][seq]

  // Extract epoch and seq from the 4 bytes before the tag
  epoch_out = static_cast<uint16_t>(packet[counter_off]) | (static_cast<uint16_t>(packet[counter_off + 1]) << 8);
  seq_out = static_cast<uint16_t>(packet[counter_off + 2]) | (static_cast<uint16_t>(packet[counter_off + 3]) << 8);

  // Extract received 32-bit tag (little-endian)
  uint32_t received_tag = static_cast<uint32_t>(packet[body_plus_counter_len]) |
                          (static_cast<uint32_t>(packet[body_plus_counter_len + 1]) << 8) |
                          (static_cast<uint32_t>(packet[body_plus_counter_len + 2]) << 16) |
                          (static_cast<uint32_t>(packet[body_plus_counter_len + 3]) << 24);

  // Compute expected tag over body + epoch + seq (everything except the tag itself)
  uint32_t expected_tag = lora_protocol::compute_auth_tag(this->auth_key_, packet.data(), body_plus_counter_len);

  return received_tag == expected_tag;
}

bool LoraGateway::check_seq_(RemoteNode *node, uint16_t epoch, uint16_t seq) {
  if (!node->get_rx_initialized()) {
    // First packet ever from this node — accept and baseline
    node->set_rx_epoch(epoch);
    node->set_rx_seq(seq);
    node->set_rx_initialized(true);
    return true;
  }

  uint16_t last_epoch = node->get_rx_epoch();
  if (epoch == last_epoch) {
    // Same session: accept a forward seq within the modular half-space
    if (lora_protocol::is_forward_u16(seq, node->get_rx_seq())) {
      node->set_rx_seq(seq);
      return true;
    }
    ESP_LOGW(TAG, "Replay/stale: epoch %u seq %u not forward of %u", epoch, seq, node->get_rx_seq());
    return false;
  }

  if (lora_protocol::is_forward_u16(epoch, last_epoch)) {
    // Newer session (node rebooted): re-baseline to this packet
    ESP_LOGI(TAG, "Node 0x%02X new epoch %u (was %u); re-baselining", node->get_address(), epoch, last_epoch);
    node->set_rx_epoch(epoch);
    node->set_rx_seq(seq);
    return true;
  }

  ESP_LOGW(TAG, "Replay/stale: older epoch %u (current %u)", epoch, last_epoch);
  return false;
}

// --- LoraGateway implementation ---

void LoraGateway::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Gateway...");

  // Boot epoch: load, bump once, persist. seq starts at 0 in RAM each boot; the
  // fresh epoch makes that safe (no per-packet NVS writes, no replay on reboot).
  this->boot_epoch_pref_ = global_preferences->make_preference<uint16_t>(fnv1_hash("lora_gateway_boot_epoch"));
  uint16_t saved_epoch = 0;
  if (this->boot_epoch_pref_.load(&saved_epoch)) {
    ESP_LOGI(TAG, "Restored boot epoch: %u", saved_epoch);
  } else {
    ESP_LOGI(TAG, "No saved boot epoch found, starting fresh");
  }
  this->boot_epoch_ = static_cast<uint16_t>(saved_epoch + 1);
  this->boot_epoch_pref_.save(&this->boot_epoch_);

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

  // Register virtual devices for each remote node and precompute the schema
  // fingerprint the gateway expects from each node (over its declared keys).
  uint32_t device_id = 1;
  for (auto *node : this->remote_nodes_) {
    node->set_device_id(device_id++);
    node->set_expected_fingerprint(lora_protocol::compute_schema_fingerprint(this->auth_key_, node->schema_manifest()));
    ESP_LOGD(TAG, "Registered virtual device for node %s (0x%02X) device_id %u, expected fingerprint 0x%04X",
             node->get_name().c_str(), node->get_address(), node->get_device_id(), node->get_expected_fingerprint());
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

  // Flags: RESPONSE_ACKED confirms receipt of this node's previous response,
  // riding the poll it is already awake for (replaces the old ACK frame).
  uint8_t flags = 0;
  if (node->get_ack_pending()) {
    flags |= lora_protocol::POLL_FLAG_RESPONSE_ACKED;
    node->set_ack_pending(false);
  }
  // TIME_PRESENT is set only when answering a node's time request (Phase 4).
  body.push_back(flags);

  // Poll interval (4 bytes LE) — always present so the node can self-anchor its
  // listen schedule from any single poll it receives.
  body.push_back(static_cast<uint8_t>(this->poll_interval_ms_ & 0xFF));
  body.push_back(static_cast<uint8_t>((this->poll_interval_ms_ >> 8) & 0xFF));
  body.push_back(static_cast<uint8_t>((this->poll_interval_ms_ >> 16) & 0xFF));
  body.push_back(static_cast<uint8_t>((this->poll_interval_ms_ >> 24) & 0xFF));

  // No time block (TIME_PRESENT unset) and no downlink commands yet.
  body.push_back(0x00);  // cmd_count

  auto packet = this->sign_packet_(node, std::move(body));
  this->sx126x_->transmit_packet(packet);
  ESP_LOGD(TAG, "Sent poll request to node 0x%02X (%s) [flags=0x%02X]", node->get_address(), node->get_name().c_str(),
           flags);
}

void LoraGateway::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // Minimum size: poll-response header (5) + auth overhead (8) = 13 bytes
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
  uint16_t epoch, seq;
  if (!this->verify_packet_(packet, epoch, seq)) {
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

  // Re-extract (epoch, seq) from the verified packet for the anti-replay check.
  size_t counter_off = packet.size() - lora_protocol::AUTH_OVERHEAD;
  uint16_t epoch = static_cast<uint16_t>(packet[counter_off]) | (static_cast<uint16_t>(packet[counter_off + 1]) << 8);
  uint16_t seq = static_cast<uint16_t>(packet[counter_off + 2]) | (static_cast<uint16_t>(packet[counter_off + 3]) << 8);

  if (!this->check_seq_(node, epoch, seq)) {
    ESP_LOGW(TAG, "Replay detected from node 0x%02X, dropping", src_addr);
    return;
  }

  // Extract packet number, total packets and response flags (before auth footer)
  uint8_t packet_num = packet[lora_protocol::OFFSET_PACKET_NUM];
  uint8_t total_packets = packet[lora_protocol::OFFSET_TOTAL_PACKETS];
  uint8_t resp_flags = packet[lora_protocol::OFFSET_RESPONSE_FLAGS];

  if (resp_flags & lora_protocol::RESP_FLAG_TIME_REQUEST) {
    // Node is requesting a wall-clock update; demand-driven time sync lands in Phase 4.
    ESP_LOGD(TAG, "Node 0x%02X requested a time update", src_addr);
  }

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
    node->set_ack_pending(true);  // confirm receipt via RESPONSE_ACKED on the next poll
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
      node->set_ack_pending(true);  // confirm receipt via RESPONSE_ACKED on the next poll
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

    if (key == lora_protocol::SCHEMA_FINGERPRINT_KEY) {
      if (offset + 2 > payload.size()) {
        ESP_LOGW(TAG, "Incomplete schema fingerprint record");
        break;
      }
      uint16_t fp = static_cast<uint16_t>(payload[offset]) | (static_cast<uint16_t>(payload[offset + 1]) << 8);
      offset += 2;
      if (fp != node->get_expected_fingerprint()) {
        ESP_LOGE(TAG,
                 "Schema fingerprint mismatch from node %s (0x%02X): got 0x%04X, expected 0x%04X. The remote's "
                 "entity declaration order/object_ids do not match this gateway's keys; dropping sensor data.",
                 node->get_name().c_str(), node->get_address(), fp, node->get_expected_fingerprint());
        return;
      }

    } else if (key == lora_protocol::SENSOR_KEY) {
      if (offset + 1 + 4 > payload.size()) {
        ESP_LOGW(TAG, "Incomplete float sensor record at offset %d", offset - 1);
        break;
      }
      uint8_t index = payload[offset++];
      float value;
      memcpy(&value, &payload[offset], sizeof(float));
      offset += 4;

      auto *sens = node->sensor_at(index);
      if (sens != nullptr) {
        sens->publish_state(value);
        sensor_count++;
        ESP_LOGD(TAG, "  Sensor[%u] = %.2f", index, value);
      } else {
        ESP_LOGW(TAG, "Float sensor index %u out of range from node %s (0x%02X)", index, node->get_name().c_str(),
                 node->get_address());
      }

    } else if (key == lora_protocol::BINARY_SENSOR_KEY) {
      if (offset + 1 + 1 > payload.size()) {
        ESP_LOGW(TAG, "Incomplete binary sensor record at offset %d", offset - 1);
        break;
      }
      uint8_t index = payload[offset++];
      bool value = (payload[offset++] != 0);

      auto *sens = node->binary_sensor_at(index);
      if (sens != nullptr) {
        sens->publish_state(value);
        binary_sensor_count++;
        ESP_LOGD(TAG, "  Binary Sensor[%u] = %s", index, value ? "ON" : "OFF");
      } else {
        ESP_LOGW(TAG, "Binary sensor index %u out of range from node %s (0x%02X)", index, node->get_name().c_str(),
                 node->get_address());
      }

    } else {
      ESP_LOGW(TAG, "Unknown record tag 0x%02X at offset %d, skipping rest of payload", key, offset - 1);
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
    for (auto *sens : sensors) {
      if (sens != nullptr) {
        sens->publish_state(NAN);
      }
    }
    auto &binary_sensors = node->get_binary_sensors();
    for (auto *sens : binary_sensors) {
      if (sens != nullptr) {
        sens->publish_state(false);
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
