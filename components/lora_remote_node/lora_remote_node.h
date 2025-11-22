#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/sx126x/sx126x.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "lora_protocol.h"

namespace esphome {
namespace lora_remote_node {

class LoraRemoteNode : public Component, public sx126x::SX126xListener {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_sx126x(sx126x::SX126x *sx126x) { this->sx126x_ = sx126x; }
  void set_address(uint8_t address) { this->address_ = address; }
  void set_time_source(time::RealTimeClock *time) { this->time_ = time; }

  void add_sensor(sensor::Sensor *sensor) { this->sensors_.push_back(sensor); }
  void add_binary_sensor(binary_sensor::BinarySensor *sensor) { this->binary_sensors_.push_back(sensor); }

  // SX126xListener interface
  void on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) override;

 protected:
  sx126x::SX126x *sx126x_{nullptr};
  uint8_t address_{0};
  time::RealTimeClock *time_{nullptr};
  std::vector<sensor::Sensor *> sensors_;
  std::vector<binary_sensor::BinarySensor *> binary_sensors_;

  bool is_poll_request_(const std::vector<uint8_t> &packet);
  bool is_time_sync_(const std::vector<uint8_t> &packet);
  void handle_poll_request_(const std::vector<uint8_t> &packet);
  void handle_time_sync_(const std::vector<uint8_t> &packet);
  std::vector<std::vector<uint8_t>> build_response_packets_(uint8_t gateway_addr);
  std::vector<uint8_t> serialize_sensor_data_();
};

}  // namespace lora_remote_node
}  // namespace esphome
