#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/sx126x/sx126x.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "lora_protocol.h"
#include "siphash.h"

namespace esphome {
namespace lora_gateway {

struct RemoteNodeMetrics {
  bool last_response_received{false};
  uint32_t last_heard_ms{0};
  uint32_t response_latency_ms{0};
  float last_rssi{NAN};
  float last_snr{NAN};
};

class RemoteNode {
 public:
  uint8_t get_address() const { return this->address_; }
  void set_address(uint8_t addr) { this->address_ = addr; }

  const std::string &get_name() const { return this->name_; }
  void set_name(const std::string &name) { this->name_ = name; }

  uint32_t get_device_id() const { return this->device_id_; }
  void set_device_id(uint32_t id) { this->device_id_ = id; }

  RemoteNodeMetrics &get_metrics() { return this->metrics_; }

  uint16_t get_rx_seq() const { return this->rx_seq_; }
  void set_rx_seq(uint16_t seq) { this->rx_seq_ = seq; }
  bool get_rx_seq_initialized() const { return this->rx_seq_initialized_; }
  void set_rx_seq_initialized(bool v) { this->rx_seq_initialized_ = v; }

  sensor::Sensor *get_or_create_sensor(const std::string &name);
  binary_sensor::BinarySensor *get_or_create_binary_sensor(const std::string &name);

  std::map<std::string, sensor::Sensor *> &get_sensors() { return this->sensors_; }
  std::map<std::string, binary_sensor::BinarySensor *> &get_binary_sensors() { return this->binary_sensors_; }

 protected:
  uint8_t address_{0};
  std::string name_;
  uint32_t device_id_{0};
  RemoteNodeMetrics metrics_;
  std::map<std::string, sensor::Sensor *> sensors_;
  std::map<std::string, binary_sensor::BinarySensor *> binary_sensors_;

  // Per-node inbound sequence tracking for anti-replay
  uint16_t rx_seq_{0};
  bool rx_seq_initialized_{false};
};

enum class StaleSensorBehavior : uint8_t {
  KEEP_LAST_VALUE,
  INVALIDATE,
};

class LoraGateway : public Component, public sx126x::SX126xListener {
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
  void set_response_timeout(uint32_t timeout_ms) { this->response_timeout_ms_ = timeout_ms; }
  void set_poll_interval(uint32_t interval_ms) { this->poll_interval_ms_ = interval_ms; }
  void set_time_source(time::RealTimeClock *time) { this->time_ = time; }
  void set_time_sync_interval(uint32_t interval_ms) { this->time_sync_interval_ms_ = interval_ms; }
  void set_send_ack(bool send_ack) { this->send_ack_ = send_ack; }
  void set_stale_sensor_behavior(StaleSensorBehavior behavior) { this->stale_behavior_ = behavior; }

  void add_remote_node(RemoteNode *node) { this->remote_nodes_.push_back(node); }

  // Metrics text sensors
  void set_timeout_list_sensor(text_sensor::TextSensor *sensor) { this->timeout_list_sensor_ = sensor; }
  void set_last_heard_list_sensor(text_sensor::TextSensor *sensor) { this->last_heard_list_sensor_ = sensor; }
  void set_signal_quality_list_sensor(text_sensor::TextSensor *sensor) { this->signal_quality_list_sensor_ = sensor; }

  // SX126xListener interface
  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override;

 protected:
  sx126x::SX126x *sx126x_{nullptr};
  uint8_t address_{0};
  uint8_t auth_key_[lora_protocol::AUTH_KEY_SIZE]{};
  uint16_t tx_seq_{0};
  uint32_t response_timeout_ms_{0};
  uint32_t poll_interval_ms_{0};
  uint32_t time_sync_interval_ms_{0};
  bool send_ack_{false};
  time::RealTimeClock *time_{nullptr};
  StaleSensorBehavior stale_behavior_{StaleSensorBehavior::KEEP_LAST_VALUE};

  std::vector<RemoteNode *> remote_nodes_;
  size_t current_poll_index_{0};
  uint32_t last_poll_start_ms_{0};
  uint32_t cycle_start_ms_{0};
  uint32_t last_time_sync_ms_{0};
  uint32_t slot_duration_ms_{0};
  bool waiting_for_response_{false};
  RemoteNode *current_node_{nullptr};

  // Multi-packet assembly
  std::map<uint8_t, std::vector<std::vector<uint8_t>>> partial_responses_;

  // Metrics sensors
  text_sensor::TextSensor *timeout_list_sensor_{nullptr};
  text_sensor::TextSensor *last_heard_list_sensor_{nullptr};
  text_sensor::TextSensor *signal_quality_list_sensor_{nullptr};

  // Packet construction with auth
  std::vector<uint8_t> sign_packet_(std::vector<uint8_t> body);
  bool verify_packet_(const std::vector<uint8_t> &packet, uint16_t &seq_out);
  bool check_seq_(RemoteNode *node, uint16_t seq);

  void start_new_cycle_();
  void poll_next_node_();
  void send_poll_request_(RemoteNode *node);
  void send_ack_packet_(RemoteNode *node);
  void broadcast_time_sync_();
  void handle_poll_response_(const std::vector<uint8_t> &packet, float rssi, float snr);
  void process_complete_response_(RemoteNode *node, const std::vector<uint8_t> &payload);
  void handle_timeout_(RemoteNode *node);
  void update_metrics_sensors_();
  RemoteNode *find_node_by_address_(uint8_t address);

  bool should_start_polling_();
};

}  // namespace lora_gateway
}  // namespace esphome
