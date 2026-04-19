#include <cmath>

#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include "heltec_battery_sensor.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#define VBAT_Read 1
#define ADC_Ctrl 37
#define ADC_Resolution 12
#define BAT_MIN_VOLTS 3.0
#define BAT_MAX_VOLTS 4.2

namespace esphome {
namespace heltec_battery_sensor {
static const char *TAG = "heltec_battery_sensor.sensor";

static adc_oneshot_unit_handle_t s_adc_handle = nullptr;

void HeltecBatterySensor::setup() {
  // Initialize ADC control pin and battery voltage input pin
  // https://digitalconcepts.net.au/arduino/index.php?op=Battery
  adc_oneshot_unit_init_cfg_t init_cfg = {};
  init_cfg.unit_id = ADC_UNIT_1;
  adc_oneshot_new_unit(&init_cfg, &s_adc_handle);

  adc_oneshot_chan_cfg_t chan_cfg = {};
  chan_cfg.atten = ADC_ATTEN_DB_0;
  chan_cfg.bitwidth = ADC_BITWIDTH_12;
  // GPIO1 on ESP32-S3 is ADC1 channel 0
  adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_0, &chan_cfg);

  gpio_config_t io_conf = {};
  io_conf.pin_bit_mask = 1ULL << ADC_Ctrl;
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io_conf);

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

  gpio_set_level(static_cast<gpio_num_t>(ADC_Ctrl), 1);

  delay_microseconds_safe(1000);

  // Quickly take 10 samples for increased accuracy
  for (int i = 0; i < numSamples; i++) {
    int sample = 0;
    adc_oneshot_read(s_adc_handle, ADC_CHANNEL_0, &sample);
    rawValue += sample;
  }

  rawValue /= numSamples;  // Average value over numSamples readings

  ESP_LOGD(TAG, "'%s' - got rawValue of %d", this->name_.c_str(), rawValue);

  gpio_set_level(static_cast<gpio_num_t>(ADC_Ctrl), 0);

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
