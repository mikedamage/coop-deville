#pragma once

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
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

  uint16_t get_rx_epoch() const { return this->rx_epoch_; }
  void set_rx_epoch(uint16_t epoch) { this->rx_epoch_ = epoch; }
  uint16_t get_rx_seq() const { return this->rx_seq_; }
  void set_rx_seq(uint16_t seq) { this->rx_seq_ = seq; }
  bool get_rx_initialized() const { return this->rx_initialized_; }
  void set_rx_initialized(bool v) { this->rx_initialized_ = v; }

  // Per-destination outbound seq so this node sees a dense +1 sequence
  uint16_t next_tx_seq() { return this->tx_seq_++; }

  // RESPONSE_ACKED: set when this node's last response was fully received,
  // consumed (and cleared) when its receipt is confirmed in the next poll.
  bool get_ack_pending() const { return this->ack_pending_; }
  void set_ack_pending(bool v) { this->ack_pending_ = v; }

  // Sensors are stored positionally: declaration order defines the wire index.
  // The parallel key vectors hold each entity's object_id (the gateway's `key`,
  // == the remote's object_id) and feed the schema-fingerprint manifest.
  void add_sensor(const std::string &key, sensor::Sensor *sens) {
    this->sensor_keys_.push_back(key);
    this->sensors_.push_back(sens);
  }
  void add_binary_sensor(const std::string &key, binary_sensor::BinarySensor *sens) {
    this->binary_keys_.push_back(key);
    this->binary_sensors_.push_back(sens);
  }

  sensor::Sensor *sensor_at(size_t index) const {
    return index < this->sensors_.size() ? this->sensors_[index] : nullptr;
  }
  binary_sensor::BinarySensor *binary_sensor_at(size_t index) const {
    return index < this->binary_sensors_.size() ? this->binary_sensors_[index] : nullptr;
  }

  const std::vector<sensor::Sensor *> &get_sensors() const { return this->sensors_; }
  const std::vector<binary_sensor::BinarySensor *> &get_binary_sensors() const { return this->binary_sensors_; }

  // Ordered object_id manifest (float sensors then binary sensors) for the fingerprint.
  std::vector<std::string> schema_manifest() const {
    std::vector<std::string> ids;
    ids.reserve(this->sensor_keys_.size() + this->binary_keys_.size());
    ids.insert(ids.end(), this->sensor_keys_.begin(), this->sensor_keys_.end());
    ids.insert(ids.end(), this->binary_keys_.begin(), this->binary_keys_.end());
    return ids;
  }

  uint16_t get_expected_fingerprint() const { return this->expected_fingerprint_; }
  void set_expected_fingerprint(uint16_t fp) { this->expected_fingerprint_ = fp; }

 protected:
  uint8_t address_{0};
  std::string name_;
  uint32_t device_id_{0};
  RemoteNodeMetrics metrics_;
  std::vector<sensor::Sensor *> sensors_;
  std::vector<binary_sensor::BinarySensor *> binary_sensors_;
  std::vector<std::string> sensor_keys_;
  std::vector<std::string> binary_keys_;
  uint16_t expected_fingerprint_{0};

  // Per-node inbound (epoch, seq) tracking for anti-replay
  uint16_t rx_epoch_{0};
  uint16_t rx_seq_{0};
  bool rx_initialized_{false};

  // Per-node outbound seq (RAM only; paired with the gateway's boot epoch)
  uint16_t tx_seq_{0};
  bool ack_pending_{false};
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
  // Boot epoch: persisted once per boot and bumped at startup so a rebooted
  // gateway never reuses a (epoch, seq) pair. Per-node seq lives on RemoteNode
  // and resets to 0 each boot; the fresh epoch makes that safe.
  uint16_t boot_epoch_{0};
  ESPPreferenceObject boot_epoch_pref_;
  uint32_t response_timeout_ms_{0};
  uint32_t poll_interval_ms_{0};
  uint32_t time_sync_interval_ms_{0};
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

  // Packet construction with auth (seq drawn from the destination node)
  std::vector<uint8_t> sign_packet_(RemoteNode *node, std::vector<uint8_t> body);
  bool verify_packet_(const std::vector<uint8_t> &packet, uint16_t &epoch_out, uint16_t &seq_out);
  bool check_seq_(RemoteNode *node, uint16_t epoch, uint16_t seq);

  void start_new_cycle_();
  void poll_next_node_();
  void send_poll_request_(RemoteNode *node);
  void handle_poll_response_(const std::vector<uint8_t> &packet, float rssi, float snr);
  void process_complete_response_(RemoteNode *node, const std::vector<uint8_t> &payload);
  void handle_timeout_(RemoteNode *node);
  void update_metrics_sensors_();
  RemoteNode *find_node_by_address_(uint8_t address);

  bool should_start_polling_();
};

}  // namespace lora_gateway
}  // namespace esphome
