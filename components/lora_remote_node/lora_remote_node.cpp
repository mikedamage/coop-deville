#include "lora_remote_node.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

namespace esphome {
namespace lora_remote_node {

static const char *const TAG = "lora_remote_node";

// --- Auth helpers ---

std::vector<uint8_t> LoraRemoteNode::sign_packet_(std::vector<uint8_t> body) {
  // Append boot epoch (little-endian)
  body.push_back(static_cast<uint8_t>(this->boot_epoch_ & 0xFF));
  body.push_back(static_cast<uint8_t>((this->boot_epoch_ >> 8) & 0xFF));

  // Append sequence number (little-endian), then advance it
  uint16_t seq = this->tx_seq_++;
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

bool LoraRemoteNode::verify_packet_(const std::vector<uint8_t> &packet, uint16_t &epoch_out, uint16_t &seq_out) {
  if (packet.size() < lora_protocol::AUTH_OVERHEAD) {
    return false;
  }

  size_t body_plus_counter_len = packet.size() - lora_protocol::AUTH_TAG_SIZE;
  size_t counter_off = packet.size() - lora_protocol::AUTH_OVERHEAD;  // start of [epoch][seq]

  epoch_out = static_cast<uint16_t>(packet[counter_off]) | (static_cast<uint16_t>(packet[counter_off + 1]) << 8);
  seq_out = static_cast<uint16_t>(packet[counter_off + 2]) | (static_cast<uint16_t>(packet[counter_off + 3]) << 8);

  uint32_t received_tag = static_cast<uint32_t>(packet[body_plus_counter_len]) |
                          (static_cast<uint32_t>(packet[body_plus_counter_len + 1]) << 8) |
                          (static_cast<uint32_t>(packet[body_plus_counter_len + 2]) << 16) |
                          (static_cast<uint32_t>(packet[body_plus_counter_len + 3]) << 24);

  uint32_t expected_tag = lora_protocol::compute_auth_tag(this->auth_key_, packet.data(), body_plus_counter_len);

  return received_tag == expected_tag;
}

bool LoraRemoteNode::check_gw_seq_(uint16_t epoch, uint16_t seq) {
  if (!this->gw_initialized_) {
    // First packet ever from the gateway — accept and baseline
    this->gw_epoch_ = epoch;
    this->gw_seq_ = seq;
    this->gw_initialized_ = true;
    ESP_LOGD(TAG, "New gateway detected");
    return true;
  }

  if (epoch == this->gw_epoch_) {
    // Same session: accept a forward seq within the modular half-space
    if (lora_protocol::is_forward_u16(seq, this->gw_seq_)) {
      this->gw_seq_ = seq;
      return true;
    }
    ESP_LOGW(TAG, "Replay/stale: epoch %u seq %u not forward of %u", epoch, seq, this->gw_seq_);
    return false;
  }

  if (lora_protocol::is_forward_u16(epoch, this->gw_epoch_)) {
    // Gateway rebooted (newer epoch) — re-baseline and force a full update
    ESP_LOGI(TAG, "Gateway new epoch %u (was %u); re-baselining", epoch, this->gw_epoch_);
    this->gw_epoch_ = epoch;
    this->gw_seq_ = seq;
    return true;
  }

  ESP_LOGW(TAG, "Replay/stale: older gateway epoch %u (current %u)", epoch, this->gw_epoch_);
  return false;
}

// --- Component lifecycle ---

void LoraRemoteNode::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Remote Node...");

  // Boot epoch: load, bump once, persist. seq starts at 0 in RAM each boot; the
  // fresh epoch makes that safe (no per-packet NVS writes, no replay on reboot).
  this->boot_epoch_pref_ = global_preferences->make_preference<uint16_t>(fnv1_hash("lora_remote_node_boot_epoch"));
  uint16_t saved_epoch = 0;
  if (this->boot_epoch_pref_.load(&saved_epoch)) {
    ESP_LOGI(TAG, "Restored boot epoch: %u", saved_epoch);
  } else {
    ESP_LOGI(TAG, "No saved boot epoch found, starting fresh");
  }
  this->boot_epoch_ = static_cast<uint16_t>(saved_epoch + 1);
  this->boot_epoch_pref_.save(&this->boot_epoch_);
  this->tx_seq_ = 0;

  // Compute the schema fingerprint once: float sensors then binary sensors, in
  // declared order, by object_id. The gateway computes the same over its keys.
  std::vector<std::string> ids;
  ids.reserve(this->sensors_.size() + this->binary_sensors_.size());
  for (auto *sens : this->sensors_) {
    ids.push_back(sens->get_object_id());
  }
  for (auto *sens : this->binary_sensors_) {
    ids.push_back(sens->get_object_id());
  }
  this->schema_fingerprint_ = lora_protocol::compute_schema_fingerprint(this->auth_key_, ids);

  ESP_LOGI(TAG, "Remote node configured at address 0x%02X with %d sensors and %d binary sensors (fingerprint=0x%04X)",
           this->address_, this->sensors_.size(), this->binary_sensors_.size(), this->schema_fingerprint_);
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
  ESP_LOGCONFIG(TAG, "  Schema Fingerprint: 0x%04X", this->schema_fingerprint_);
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
  uint16_t epoch, seq;
  if (!this->verify_packet_(packet, epoch, seq)) {
    ESP_LOGW(TAG, "Received packet with invalid auth tag, dropping");
    return;
  }

  if (this->is_poll_request_(packet)) {
    if (!this->check_gw_seq_(epoch, seq)) {
      ESP_LOGW(TAG, "Poll request replay detected, dropping");
      return;
    }
    ESP_LOGD(TAG, "Received poll request from 0x%02X (RSSI: %.1f, SNR: %.1f)", packet[lora_protocol::OFFSET_SRC_ADDR],
             rssi, snr);
    this->handle_poll_request_(packet);
  }
}

bool LoraRemoteNode::is_poll_request_(const std::vector<uint8_t> &packet) {
  if (packet.size() < lora_protocol::POLL_REQUEST_MIN_SIZE) {
    return false;
  }
  if (packet[lora_protocol::OFFSET_COMMAND] != lora_protocol::CMD_POLL_REQUEST) {
    return false;
  }
  // Poll requests are unicast only — the destination must be this node (0xFF and
  // any other address are rejected; there are no broadcast frames).
  return (packet[lora_protocol::OFFSET_DST_ADDR] == this->address_);
}

// --- Packet handlers ---

void LoraRemoteNode::apply_time_block_(uint32_t timestamp) {
  if (this->time_ == nullptr) {
    ESP_LOGW(TAG, "Poll carried time but no time source is configured");
    return;
  }
  ESP_LOGI(TAG, "Setting RTC time to %u (seconds since epoch)", timestamp);
  this->time_->set_epoch(timestamp);
  auto esptime = ESPTime::from_epoch_local(timestamp);
  ESP_LOGD(TAG, "Time applied: %04d-%02d-%02d %02d:%02d:%02d", esptime.year, esptime.month, esptime.day_of_month,
           esptime.hour, esptime.minute, esptime.second);
}

void LoraRemoteNode::handle_poll_request_(const std::vector<uint8_t> &packet) {
  uint8_t gateway_addr = packet[lora_protocol::OFFSET_SRC_ADDR];
  uint8_t flags = packet[lora_protocol::OFFSET_POLL_FLAGS];

  // Poll interval is always present (bytes 4-7, LE) — the node self-anchors its
  // listen schedule from this.
  uint32_t poll_interval = static_cast<uint32_t>(packet[lora_protocol::OFFSET_POLL_INTERVAL]) |
                           (static_cast<uint32_t>(packet[lora_protocol::OFFSET_POLL_INTERVAL + 1]) << 8) |
                           (static_cast<uint32_t>(packet[lora_protocol::OFFSET_POLL_INTERVAL + 2]) << 16) |
                           (static_cast<uint32_t>(packet[lora_protocol::OFFSET_POLL_INTERVAL + 3]) << 24);
  this->poll_interval_ms_ = poll_interval;

  if (flags & lora_protocol::POLL_FLAG_RESPONSE_ACKED) {
    ESP_LOGD(TAG, "Gateway acknowledged our previous response");
  }

  // Optional wall-clock time block follows poll_interval when TIME_PRESENT.
  if (flags & lora_protocol::POLL_FLAG_TIME_PRESENT) {
    size_t off = lora_protocol::OFFSET_POLL_OPTIONAL;
    if (packet.size() >= off + lora_protocol::TIME_BLOCK_SIZE + lora_protocol::AUTH_OVERHEAD) {
      uint32_t timestamp = static_cast<uint32_t>(packet[off]) | (static_cast<uint32_t>(packet[off + 1]) << 8) |
                           (static_cast<uint32_t>(packet[off + 2]) << 16) |
                           (static_cast<uint32_t>(packet[off + 3]) << 24);
      this->apply_time_block_(timestamp);
    } else {
      ESP_LOGW(TAG, "Poll claims TIME_PRESENT but is too short for a time block");
    }
  }

  ESP_LOGD(TAG, "Handling poll request from gateway 0x%02X (flags=0x%02X, poll_interval=%u ms)", gateway_addr, flags,
           this->poll_interval_ms_);

  // Record timing for listen window prediction
  this->last_poll_received_ms_ = millis();
  this->consecutive_missed_polls_ = 0;

  // First poll learned the schedule: enter windowed mode if enabled.
  if (this->listen_window_enabled_ && !this->schedule_received_) {
    this->schedule_received_ = true;
    ESP_LOGI(TAG, "Schedule acquired from poll; entering windowed listen mode");
  }

  // Mark that we're transmitting a response (prevents listen window from closing)
  this->responding_ = true;

  // Every response is a full snapshot (no delta); see PROTOCOL.md.
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

// --- Response building ---

std::vector<std::vector<uint8_t>> LoraRemoteNode::build_response_packets_(uint8_t gateway_addr) {
  std::vector<std::vector<uint8_t>> packets;
  std::vector<uint8_t> payload = this->serialize_sensor_data_();

  // Response flags (byte 5). TIME_REQUEST (demand-driven time sync) lands in
  // Phase 4; for now no flags are raised. Identical on every fragment.
  uint8_t resp_flags = 0;

  if (payload.size() <= lora_protocol::MAX_PAYLOAD_SIZE) {
    // Single packet response
    std::vector<uint8_t> body;
    body.push_back(this->address_);
    body.push_back(gateway_addr);
    body.push_back(lora_protocol::CMD_POLL_RESPONSE);
    body.push_back(0x00);  // Packet number (0 for single)
    body.push_back(0x01);  // Total packets (1)
    body.push_back(resp_flags);
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
      body.push_back(resp_flags);
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

  // Schema fingerprint record (first, before any sensor records) so the gateway
  // can verify the index->entity mapping before publishing.
  data.push_back(lora_protocol::SCHEMA_FINGERPRINT_KEY);
  data.push_back(static_cast<uint8_t>(this->schema_fingerprint_ & 0xFF));
  data.push_back(static_cast<uint8_t>((this->schema_fingerprint_ >> 8) & 0xFF));

  // Full snapshot: every sensor with a valid state, identified by its declared
  // index (0-based position). Index is the loop position, NOT a running counter,
  // so a sensor without state simply leaves a gap.
  for (size_t i = 0; i < this->sensors_.size(); i++) {
    auto *sens = this->sensors_[i];
    if (!sens->has_state()) {
      continue;
    }
    float value = sens->state;
    uint8_t value_bytes[4];
    memcpy(value_bytes, &value, 4);

    data.push_back(lora_protocol::SENSOR_KEY);
    data.push_back(static_cast<uint8_t>(i));
    data.insert(data.end(), value_bytes, value_bytes + 4);
  }

  for (size_t i = 0; i < this->binary_sensors_.size(); i++) {
    auto *sens = this->binary_sensors_[i];
    if (!sens->has_state()) {
      continue;
    }
    data.push_back(lora_protocol::BINARY_SENSOR_KEY);
    data.push_back(static_cast<uint8_t>(i));
    data.push_back(sens->state ? 0x01 : 0x00);
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
