#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/sx126x/sx126x.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "lora_protocol.h"
#include "siphash.h"

namespace esphome {
namespace lora_remote_node {

class LoraRemoteNode : public Component, public sx126x::SX126xListener {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_sx126x(sx126x::SX126x *sx126x) { this->sx126x_ = sx126x; }
  void set_address(uint8_t address) { this->address_ = address; }
  void set_auth_key(const std::vector<uint8_t> &key) {
    std::copy_n(key.begin(), std::min(key.size(), (size_t) lora_protocol::AUTH_KEY_SIZE), this->auth_key_);
  }
  void set_time_source(time::RealTimeClock *time) { this->time_ = time; }
  void set_listen_window(uint32_t ms) {
    this->guard_window_ms_ = ms;
    this->listen_window_enabled_ = true;
  }
  void set_full_update_interval(uint8_t interval) { this->full_update_interval_ = interval; }

  void add_sensor(sensor::Sensor *sensor) { this->sensors_.push_back(sensor); }
  void add_binary_sensor(binary_sensor::BinarySensor *sensor) { this->binary_sensors_.push_back(sensor); }

  // SX126xListener interface
  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override;

 protected:
  sx126x::SX126x *sx126x_{nullptr};
  uint8_t address_{0};
  uint8_t auth_key_[lora_protocol::AUTH_KEY_SIZE]{};
  uint16_t tx_seq_{0};
  uint16_t gw_seq_{0};
  bool gw_seq_initialized_{false};
  time::RealTimeClock *time_{nullptr};
  std::vector<sensor::Sensor *> sensors_;
  std::vector<binary_sensor::BinarySensor *> binary_sensors_;

  // Listen window state
  bool listen_window_enabled_{false};
  bool schedule_received_{false};
  bool radio_sleeping_{false};
  bool responding_{false};
  uint32_t guard_window_ms_{lora_protocol::DEFAULT_GUARD_WINDOW_MS};
  uint32_t poll_interval_ms_{0};
  uint32_t slot_duration_ms_{0};
  uint8_t slot_index_{0};
  uint8_t schedule_node_count_{0};
  uint32_t last_poll_received_ms_{0};
  uint32_t next_listen_start_ms_{0};
  uint32_t next_listen_end_ms_{0};
  uint8_t consecutive_missed_polls_{0};

  // Delta compression state
  std::map<std::string, std::array<uint8_t, 4>> last_sent_sensor_values_;
  std::map<std::string, bool> last_sent_binary_values_;
  uint8_t full_update_counter_{0};
  uint8_t full_update_interval_{10};
  bool force_next_full_update_{true};  // first poll is always full

  // Auth helpers
  std::vector<uint8_t> sign_packet_(std::vector<uint8_t> body);
  bool verify_packet_(const std::vector<uint8_t> &packet, uint16_t &seq_out);
  bool check_gw_seq_(uint16_t seq);

  // Packet classification
  bool is_poll_request_(const std::vector<uint8_t> &packet);
  bool is_time_sync_(const std::vector<uint8_t> &packet);
  bool is_ack_(const std::vector<uint8_t> &packet);

  // Packet handlers
  void handle_poll_request_(const std::vector<uint8_t> &packet);
  void handle_time_sync_(const std::vector<uint8_t> &packet);

  // Response building
  std::vector<std::vector<uint8_t>> build_response_packets_(uint8_t gateway_addr, bool force_full);
  std::vector<uint8_t> serialize_sensor_data_(bool force_full);

  // Listen window management
  void compute_next_listen_window_();
  void wake_radio_();
  void sleep_radio_();
  void fallback_to_continuous_rx_();
};

}  // namespace lora_remote_node
}  // namespace esphome
