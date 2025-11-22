#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "heltec_battery_sensor.h"

#define VBAT_Read 1
#define ADC_Ctrl 37
#define ADC_Resolution 12
#define BAT_MIN_VOLTS 3.0
#define BAT_MAX_VOLTS 4.2

namespace esphome {
namespace heltec_battery_sensor {
static const char *TAG = "heltec_battery_sensor.sensor";

void HeltecBatterySensor::setup() {
  // Initialize ADC control pin and battery voltage input pin
  // https://digitalconcepts.net.au/arduino/index.php?op=Battery
  analogSetAttenuation(ADC_0db);
  pinMode(ADC_Ctrl, OUTPUT);
  pinMode(VBAT_Read, INPUT);
  // adcAttachPin(VBAT_Read);
  analogReadResolution(ADC_Resolution);  // 12-bit ADC resolution

  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR(" ", "Voltage", this->voltage_sensor_);
  LOG_SENSOR(" ", "Charge Percentage", this->charge_percentage_sensor_);
}

float HeltecBatterySensor::read_battery_voltage_() {
  uint16_t rawValue = 0;
  const float adcMaxVoltage = 3.3;
  const int adcMax = pow(2, ADC_Resolution - 1);
  const int numSamples = 10;

  // Voltage divider resistor values:
  const int R1 = 390.0;
  const int R2 = 100.0;

  digitalWrite(ADC_Ctrl, HIGH);

  delay_microseconds_safe(1000);

  // Quickly take 10 samples for increased accuracy
  for (int i = 0; i < numSamples; i++) {
    rawValue += analogRead(VBAT_Read);
  }

  rawValue /= numSamples;  // Average value over numSamples readings

  ESP_LOGD(TAG, "'%s' - got rawValue of %d", this->name_.c_str(), rawValue);

  digitalWrite(ADC_Ctrl, LOW);

  float scale = (adcMaxVoltage / (float) adcMax);
  float voltageDivFactor = (R1 + R2) / (float) R2;
  float volts = (scale * (float) rawValue * voltageDivFactor) / adcMaxVoltage / 2.0;

  ESP_LOGD(TAG, "'%s' - got un-calibrated voltage of %.2f", this->name_.c_str(), volts);

  return volts;
}

float HeltecBatterySensor::get_charge_percentage_(float volts) {
  // linear interpolation from 3.8-4.2 volts to 0-100%
  // y = y1 + (x - x1) * (y2 - y1) / (x2 - x1)
  return ((volts - BAT_MIN_VOLTS) * 100.0) / (BAT_MAX_VOLTS - BAT_MIN_VOLTS);
}

void HeltecBatterySensor::update() {
  float voltage = this->read_battery_voltage_();
  float charge_percentage = this->get_charge_percentage_(voltage);

  if (this->voltage_sensor_ != nullptr) {
    this->voltage_sensor_->publish_state(voltage);
  }

  if (this->charge_percentage_sensor_ != nullptr) {
    this->charge_percentage_sensor_->publish_state(charge_percentage);
  }
}

void HeltecBatterySensor::dump_config() { ESP_LOGCONFIG(TAG, "Heltec Battery Sensor"); }
}  // namespace heltec_battery_sensor
}  // namespace esphome
