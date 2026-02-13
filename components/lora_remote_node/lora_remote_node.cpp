#include "lora_remote_node.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lora_remote_node {

static const char *const TAG = "lora_remote_node";

// --- Auth helpers ---

std::vector<uint8_t> LoraRemoteNode::sign_packet_(std::vector<uint8_t> body) {
  uint16_t seq = this->tx_seq_++;
  body.push_back(static_cast<uint8_t>(seq & 0xFF));
  body.push_back(static_cast<uint8_t>((seq >> 8) & 0xFF));

  uint16_t tag = lora_protocol::compute_auth_tag(this->auth_key_, body.data(), body.size());
  body.push_back(static_cast<uint8_t>(tag & 0xFF));
  body.push_back(static_cast<uint8_t>((tag >> 8) & 0xFF));

  return body;
}

bool LoraRemoteNode::verify_packet_(const std::vector<uint8_t> &packet, uint16_t &seq_out) {
  if (packet.size() < lora_protocol::AUTH_OVERHEAD) {
    return false;
  }

  size_t body_plus_seq_len = packet.size() - lora_protocol::AUTH_TAG_SIZE;
  size_t body_len = packet.size() - lora_protocol::AUTH_OVERHEAD;

  seq_out = static_cast<uint16_t>(packet[body_len]) | (static_cast<uint16_t>(packet[body_len + 1]) << 8);

  uint16_t received_tag =
      static_cast<uint16_t>(packet[body_plus_seq_len]) | (static_cast<uint16_t>(packet[body_plus_seq_len + 1]) << 8);

  uint16_t expected_tag = lora_protocol::compute_auth_tag(this->auth_key_, packet.data(), body_plus_seq_len);

  return received_tag == expected_tag;
}

bool LoraRemoteNode::check_gw_seq_(uint16_t seq) {
  if (!this->gw_seq_initialized_) {
    this->gw_seq_ = seq;
    this->gw_seq_initialized_ = true;
    return true;
  }

  uint16_t diff = seq - this->gw_seq_;
  if (diff >= 1 && diff <= lora_protocol::SEQ_WINDOW_SIZE) {
    this->gw_seq_ = seq;
    return true;
  }

  ESP_LOGW(TAG, "Gateway sequence rejected: got %u, expected after %u", seq, this->gw_seq_);
  return false;
}

// --- Component lifecycle ---

void LoraRemoteNode::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Remote Node...");
  ESP_LOGI(TAG, "Remote node configured at address 0x%02X with %d sensors and %d binary sensors", this->address_,
           this->sensors_.size(), this->binary_sensors_.size());
  if (this->listen_window_enabled_) {
    ESP_LOGI(TAG, "Listen window enabled: guard=%ums", this->guard_window_ms_);
  }
}

void LoraRemoteNode::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRa Remote Node:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Sensors: %d", this->sensors_.size());
  ESP_LOGCONFIG(TAG, "  Binary Sensors: %d", this->binary_sensors_.size());
  ESP_LOGCONFIG(TAG, "  Auth: SipHash-2-4 (16-byte key)");
  if (this->time_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Time Source: configured");
  }
  if (this->listen_window_enabled_) {
    ESP_LOGCONFIG(TAG, "  Listen Window: %u ms guard", this->guard_window_ms_);
  } else {
    ESP_LOGCONFIG(TAG, "  Listen Window: disabled (continuous RX)");
  }
}

void LoraRemoteNode::loop() {
  if (!this->listen_window_enabled_ || !this->schedule_received_) {
    // No windowed listening — radio stays in continuous RX
    return;
  }

  uint32_t now = millis();

  if (this->radio_sleeping_) {
    // Check if it's time to wake up for the next listen window
    if (now >= this->next_listen_start_ms_) {
      this->wake_radio_();
    }
  } else if (!this->responding_) {
    // Radio is awake and we're not in the middle of responding.
    // Check if the listen window has expired.
    if (now >= this->next_listen_end_ms_) {
      // Window expired without receiving a poll
      this->consecutive_missed_polls_++;
      ESP_LOGD(TAG, "Listen window expired without poll (missed %d/%d)", this->consecutive_missed_polls_,
               lora_protocol::MAX_MISSED_POLLS);

      if (this->consecutive_missed_polls_ >= lora_protocol::MAX_MISSED_POLLS) {
        this->fallback_to_continuous_rx_();
      } else {
        this->sleep_radio_();
        this->compute_next_listen_window_();
      }
    }
  }
}

// --- Packet reception ---

void LoraRemoteNode::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // All packets require at least the auth overhead
  if (packet.size() < 3 + lora_protocol::AUTH_OVERHEAD) {
    return;
  }

  // Verify auth before any processing
  uint16_t seq;
  if (!this->verify_packet_(packet, seq)) {
    ESP_LOGW(TAG, "Received packet with invalid auth tag, dropping");
    return;
  }

  if (this->is_poll_request_(packet)) {
    if (!this->check_gw_seq_(seq)) {
      ESP_LOGW(TAG, "Poll request replay detected, dropping");
      return;
    }
    ESP_LOGD(TAG, "Received poll request from 0x%02X (RSSI: %.1f, SNR: %.1f)", packet[lora_protocol::OFFSET_SRC_ADDR],
             rssi, snr);
    this->handle_poll_request_(packet);
  } else if (this->is_time_sync_(packet)) {
    if (!this->check_gw_seq_(seq)) {
      ESP_LOGW(TAG, "Time sync replay detected, dropping");
      return;
    }
    ESP_LOGD(TAG, "Received time sync from 0x%02X (RSSI: %.1f, SNR: %.1f)", packet[lora_protocol::OFFSET_SRC_ADDR],
             rssi, snr);
    this->handle_time_sync_(packet);
  } else if (this->is_ack_(packet)) {
    ESP_LOGD(TAG, "Received ACK from 0x%02X", packet[lora_protocol::OFFSET_SRC_ADDR]);
    // ACKs are informational only for now; future retry logic will use them
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
  // Minimum time sync: header(14) + 0 nodes + auth(4) = 18 bytes
  if (packet.size() < lora_protocol::TIME_SYNC_HEADER_SIZE + lora_protocol::AUTH_OVERHEAD) {
    return false;
  }
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_TIME_SYNC) {
    return false;
  }
  uint8_t target = packet[lora_protocol::OFFSET_DST_ADDR];
  return (target == lora_protocol::BROADCAST_ADDRESS);
}

bool LoraRemoteNode::is_ack_(const std::vector<uint8_t> &packet) {
  if (packet.size() < lora_protocol::ACK_PACKET_SIZE) {
    return false;
  }
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_ACK) {
    return false;
  }
  uint8_t target = packet[lora_protocol::OFFSET_DST_ADDR];
  return (target == this->address_);
}

// --- Packet handlers ---

void LoraRemoteNode::handle_poll_request_(const std::vector<uint8_t> &packet) {
  uint8_t gateway_addr = packet[lora_protocol::OFFSET_SRC_ADDR];
  ESP_LOGD(TAG, "Handling poll request from gateway 0x%02X", gateway_addr);

  // Record timing for listen window prediction
  this->last_poll_received_ms_ = millis();
  this->consecutive_missed_polls_ = 0;

  // Mark that we're transmitting a response (prevents listen window from closing)
  this->responding_ = true;

  auto response_packets = this->build_response_packets_(gateway_addr);
  for (const auto &response : response_packets) {
    this->sx126x_->transmit_packet(response);
    if (response_packets.size() > 1) {
      delay(15);  // 15ms turnaround for multi-packet
    }
  }

  this->responding_ = false;

  // Schedule next listen window if windowed mode is active
  if (this->listen_window_enabled_ && this->schedule_received_) {
    this->compute_next_listen_window_();
    this->sleep_radio_();
  }
}

void LoraRemoteNode::handle_time_sync_(const std::vector<uint8_t> &packet) {
  // Extract timestamp (4 bytes LE at offset 3)
  uint32_t timestamp = 0;
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP]);
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP + 1]) << 8;
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP + 2]) << 16;
  timestamp |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_TIMESTAMP + 3]) << 24;

  // Apply RTC time sync
  if (this->time_ != nullptr) {
    ESP_LOGI(TAG, "Setting RTC time to %u (seconds since epoch)", timestamp);
    auto esptime = ESPTime::from_epoch_local(timestamp);
    this->time_->call_setup();
    ESP_LOGD(TAG, "Time sync applied: %04d-%02d-%02d %02d:%02d:%02d", esptime.year, esptime.month, esptime.day_of_month,
             esptime.hour, esptime.minute, esptime.second);
  }

  // Extract schedule info
  uint32_t poll_interval = 0;
  poll_interval |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_POLL_INTERVAL]);
  poll_interval |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_POLL_INTERVAL + 1]) << 8;
  poll_interval |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_POLL_INTERVAL + 2]) << 16;
  poll_interval |= static_cast<uint32_t>(packet[lora_protocol::OFFSET_TIME_SYNC_POLL_INTERVAL + 3]) << 24;

  uint16_t slot_duration = 0;
  slot_duration |= static_cast<uint16_t>(packet[lora_protocol::OFFSET_TIME_SYNC_SLOT_DURATION]);
  slot_duration |= static_cast<uint16_t>(packet[lora_protocol::OFFSET_TIME_SYNC_SLOT_DURATION + 1]) << 8;

  uint8_t node_count = packet[lora_protocol::OFFSET_TIME_SYNC_NODE_COUNT];

  // Validate packet has enough bytes for the node list + auth
  size_t expected_min = lora_protocol::OFFSET_TIME_SYNC_NODE_LIST + node_count + lora_protocol::AUTH_OVERHEAD;
  if (packet.size() < expected_min) {
    ESP_LOGW(TAG, "Time sync packet too short for declared node count %d", node_count);
    return;
  }

  // Find our slot index in the schedule
  bool found = false;
  for (uint8_t i = 0; i < node_count; i++) {
    if (packet[lora_protocol::OFFSET_TIME_SYNC_NODE_LIST + i] == this->address_) {
      this->slot_index_ = i;
      found = true;
      break;
    }
  }

  if (!found) {
    ESP_LOGW(TAG, "This node (0x%02X) is not in the gateway's schedule", this->address_);
    return;
  }

  this->poll_interval_ms_ = poll_interval;
  this->slot_duration_ms_ = slot_duration;
  this->schedule_node_count_ = node_count;

  ESP_LOGI(TAG, "Schedule received: slot_index=%d, slot_duration=%u, poll_interval=%u, nodes=%d", this->slot_index_,
           this->slot_duration_ms_, this->poll_interval_ms_, this->schedule_node_count_);

  // If this is the first schedule and listen windows are enabled, transition to windowed mode
  if (this->listen_window_enabled_ && !this->schedule_received_) {
    this->schedule_received_ = true;

    // Predict when our first poll will arrive after this time sync.
    // The gateway polls node[i] at (cycle_start + i * slot_duration).
    // The time sync was sent at the start of the cycle, so our first poll should
    // arrive approximately (slot_index + 1) * slot_duration from now.
    // (+1 because the time sync transmission takes ~1 slot worth of time)
    uint32_t first_poll_offset = (this->slot_index_ + 1) * this->slot_duration_ms_;
    uint32_t now = millis();
    this->next_listen_start_ms_ = now + first_poll_offset - this->guard_window_ms_ / 2;
    this->next_listen_end_ms_ = now + first_poll_offset + this->guard_window_ms_ / 2;

    ESP_LOGI(TAG, "Transitioning to windowed listen mode, first window in %u ms", first_poll_offset);
    this->sleep_radio_();
  } else if (this->listen_window_enabled_ && this->schedule_received_) {
    // Schedule updated — recompute windows if we haven't been polled recently
    if (this->last_poll_received_ms_ == 0) {
      uint32_t first_poll_offset = (this->slot_index_ + 1) * this->slot_duration_ms_;
      uint32_t now = millis();
      this->next_listen_start_ms_ = now + first_poll_offset - this->guard_window_ms_ / 2;
      this->next_listen_end_ms_ = now + first_poll_offset + this->guard_window_ms_ / 2;
    }
  }
}

// --- Response building ---

std::vector<std::vector<uint8_t>> LoraRemoteNode::build_response_packets_(uint8_t gateway_addr) {
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> payload = this->serialize_sensor_data_();

  if (payload.size() <= lora_protocol::MAX_PAYLOAD_SIZE) {
    // Single packet response
    std::vector<uint8_t> body;
    body.push_back(this->address_);
    body.push_back(gateway_addr);
    body.push_back(lora_protocol::CMD_POLL_RESPONSE);
    body.push_back(0x00);  // Packet number (0 for single)
    body.push_back(0x01);  // Total packets (1)
    body.insert(body.end(), payload.begin(), payload.end());
    packets.push_back(this->sign_packet_(std::move(body)));
  } else {
    // Multi-packet response
    size_t offset = 0;
    uint8_t packet_num = 1;
    uint8_t total_packets = (payload.size() + lora_protocol::MAX_PAYLOAD_SIZE - 1) / lora_protocol::MAX_PAYLOAD_SIZE;

    while (offset < payload.size()) {
      size_t chunk_size = std::min(lora_protocol::MAX_PAYLOAD_SIZE, payload.size() - offset);

      std::vector<uint8_t> body;
      body.push_back(this->address_);
      body.push_back(gateway_addr);
      body.push_back(lora_protocol::CMD_POLL_RESPONSE);
      body.push_back(packet_num);
      body.push_back(total_packets);
      body.insert(body.end(), payload.begin() + offset, payload.begin() + offset + chunk_size);
      packets.push_back(this->sign_packet_(std::move(body)));

      offset += chunk_size;
      packet_num++;
    }
  }

  return packets;
}

std::vector<uint8_t> LoraRemoteNode::serialize_sensor_data_() {
  std::vector<uint8_t> data;

  for (auto *sens : this->sensors_) {
    if (!sens->has_state()) {
      continue;
    }
    data.push_back(lora_protocol::SENSOR_KEY);

    float value = sens->state;
    uint8_t *value_bytes = reinterpret_cast<uint8_t *>(&value);
    data.insert(data.end(), value_bytes, value_bytes + 4);

    std::string name = sens->get_name();
    data.push_back(static_cast<uint8_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
  }

  for (auto *sens : this->binary_sensors_) {
    if (!sens->has_state()) {
      continue;
    }
    data.push_back(lora_protocol::BINARY_SENSOR_KEY);
    data.push_back(sens->state ? 0x01 : 0x00);

    std::string name = sens->get_name();
    data.push_back(static_cast<uint8_t>(name.size()));
    data.insert(data.end(), name.begin(), name.end());
  }

  return data;
}

// --- Listen window management ---

void LoraRemoteNode::compute_next_listen_window_() {
  // Predict the next poll arrival based on the last received poll + poll_interval
  uint32_t next_poll = this->last_poll_received_ms_ + this->poll_interval_ms_;
  this->next_listen_start_ms_ = next_poll - this->guard_window_ms_ / 2;
  this->next_listen_end_ms_ = next_poll + this->guard_window_ms_ / 2;

  ESP_LOGD(TAG, "Next listen window: start=%u, end=%u (now=%u, delta=%u ms)", this->next_listen_start_ms_,
           this->next_listen_end_ms_, millis(), next_poll - millis());
}

void LoraRemoteNode::wake_radio_() {
  this->sx126x_->set_mode_rx();
  this->radio_sleeping_ = false;
  ESP_LOGD(TAG, "Radio woke for listen window");
}

void LoraRemoteNode::sleep_radio_() {
  // Use standby RC mode for fast wake (~3.5us) while drawing only ~0.6uA
  this->sx126x_->set_mode_standby(sx126x::STDBY_RC);
  this->radio_sleeping_ = true;
  ESP_LOGD(TAG, "Radio entering standby until next listen window");
}

void LoraRemoteNode::fallback_to_continuous_rx_() {
  ESP_LOGW(TAG, "Missed %d consecutive polls, falling back to continuous RX to re-acquire schedule",
           this->consecutive_missed_polls_);
  this->schedule_received_ = false;
  this->consecutive_missed_polls_ = 0;
  this->last_poll_received_ms_ = 0;

  if (this->radio_sleeping_) {
    this->sx126x_->set_mode_rx();
    this->radio_sleeping_ = false;
  }
}

}  // namespace lora_remote_node
}  // namespace esphome
