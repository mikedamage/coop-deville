#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"

namespace esphome {
namespace heltec_battery_sensor {

class HeltecBatterySensor : public sensor::Sensor, public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  void set_voltage_sensor(sensor::Sensor *voltage_sensor) { voltage_sensor_ = voltage_sensor; }
  void set_charge_percentage_sensor(sensor::Sensor *charge_percentage_sensor) {
    charge_percentage_sensor_ = charge_percentage_sensor;
  }

 protected:
  float read_battery_voltage_();
  float get_charge_percentage_(float volts);
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *charge_percentage_sensor_{nullptr};
};

}  // namespace heltec_battery_sensor
}  // namespace esphome
