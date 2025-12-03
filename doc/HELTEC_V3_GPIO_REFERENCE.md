# Heltec WiFi LoRa 32 V3 GPIO Reference

This document provides a comprehensive reference for all GPIO pins on the Heltec WiFi LoRa 32 V3 board (ESP32-S3), including their functions, special capabilities, and current usage in this project.

## Pin Legend

| Symbol | Meaning |
|--------|---------|
| **STRAP** | ESP32-S3 strapping pin (affects boot behavior) |
| **ADC** | Analog-to-Digital Converter capable |
| **TOUCH** | Capacitive touch capable |
| **SPI** | SPI bus pin |
| **I2C** | I2C bus pin |
| **JTAG** | JTAG debugging interface |
| :white_check_mark: | Currently used in chicken-tractor.yaml |

---

## Header J3 (Left Side)

| Physical Pin | GPIO | Functions | ADC Channel | Special | Project Usage |
|--------------|------|-----------|-------------|---------|---------------|
| 18 | GPIO7 | TOUCH7 | ADC1_CH6 | | |
| 17 | GPIO6 | TOUCH6 | ADC1_CH5 | | |
| 16 | GPIO5 | TOUCH5 | ADC1_CH4 | | |
| 16 | GPIO5 | TOUCH5 | ADC1_CH4 | | :white_check_mark: **Solar Voltage ADC** |
| 15 | GPIO4 | TOUCH4 | ADC1_CH3 | | |
| 14 | GPIO3 | TOUCH3 | ADC1_CH2 | **STRAP** | |
| 13 | GPIO2 | TOUCH2 | ADC1_CH1 | | :white_check_mark: **I2C SCL** (sensor_bus) |
| 12 | GPIO1 | TOUCH1, VBAT_Read | ADC1_CH0 | | :white_check_mark: **I2C SDA** (sensor_bus) |
| 11 | GPIO38 | SUBSPIWP, FSPIWP | | | :white_check_mark: **Charge PWM** (LEDC output) |
| 10 | GPIO39 | MTCK | | JTAG | |
| 9 | GPIO40 | MTDO | | JTAG | |
| 8 | GPIO41 | MTDI | | JTAG | |
| 7 | GPIO42 | MTMS | | JTAG | |
| 6 | GPIO45 | | | **STRAP** | |
| 5 | GPIO46 | | | **STRAP** (input only) | |
| 4 | GPIO37 | ADC_Ctrl, SUBSPIQ, FSPIQ, SPIDQS | | | |
| 3 | 3V3 | Power | | | |
| 2 | 3V3 | Power | | | |
| 1 | GND | Ground | | | |

---

## Header J2 (Right Side)

| Physical Pin | GPIO | Functions | ADC Channel | Special | Project Usage |
|--------------|------|-----------|-------------|---------|---------------|
| 18 | GPIO19 | U1RST | ADC2_CH8 | USB D-, CLK_OUT2 | |
| 17 | GPIO20 | U1CTS | ADC2_CH9 | USB D+, CLK_OUT1 | |
| 16 | GPIO21 | OLED_RST | | | :white_check_mark: **OLED Reset** |
| 15 | GPIO26 | SPICS1 | | | |
| 14 | GPIO48 | | | | :white_check_mark: **Load Switch Output** |
| 13 | GPIO47 | | | | |
| 12 | GPIO33 | SPIIO4 | | FSPIHD, SUBSPIHD | |
| 11 | GPIO34 | SPIIO5 | | FSPICS0, SUBSPICS0 | |
| 10 | GPIO35 | SPIIO6 | | FSPID, SUBSPID, LED_Write | |
| 9 | GPIO36 | SPIIO7 | | FSPICLK, SUBSPICLK, Vext_Ctrl | :white_check_mark: **OLED Vext Control** (low = on) |
| 8 | GPIO0 | USER_SW | | **STRAP**, Pull-up | :white_check_mark: **Display Toggle Button** |
| 7 | RST | Reset | | RST_SW, Pull-up | |
| 6 | GPIO43 | U0TXD | | CP2102_RX | |
| 5 | GPIO44 | U0RXD | | CP2102_TX | |
| 4 | Ve | Vext | | | |
| 3 | Ve | Vext | | | |
| 2 | 5V | Power | | | |
| 1 | GND | Ground | | | |

---

## Integrated OLED Display (SSD1306 128x64)

| Function | GPIO | Notes |
|----------|------|-------|
| I2C SDA | GPIO17 | :white_check_mark: display_bus |
| I2C SCL | GPIO18 | :white_check_mark: display_bus |
| Reset | GPIO21 | :white_check_mark: Active low |
| Vext Control | GPIO36 | :white_check_mark: LOW = display power on |

I2C Address: `0x3C`

---

## Integrated SX1262 LoRa Radio

| Function | GPIO | Notes |
|----------|------|-------|
| NSS (CS) | GPIO8 | :white_check_mark: SPI chip select |
| SCK | GPIO9 | :white_check_mark: SPI clock |
| MOSI | GPIO10 | :white_check_mark: SPI data out |
| MISO | GPIO11 | :white_check_mark: SPI data in |
| RST | GPIO12 | :white_check_mark: Radio reset |
| BUSY | GPIO13 | :white_check_mark: Radio busy status |
| DIO1 | GPIO14 | :white_check_mark: Interrupt line |

---

## ESP32-S3 Strapping Pins

These pins affect boot behavior and should be used with caution:

| GPIO | Boot Function | Default State | Notes |
|------|---------------|---------------|-------|
| GPIO0 | Boot mode select | Pull-up | LOW = download mode, HIGH = normal boot |
| GPIO3 | JTAG select | Pull-up | Directly connected to JTAG signal, avoid external pulls |
| GPIO45 | VDD_SPI voltage | Pull-down | Selects flash voltage (3.3V/1.8V) |
| GPIO46 | Boot mode select | Pull-down | Input only, affects ROM messages |

> **Note**: GPIO3 was previously used for solar voltage ADC but moved to GPIO5 to avoid strapping pin issues when the solar panel is disconnected (which would pull the pin low).

---

## ADC Channels

### ADC1 (Available during WiFi operation)

| Channel | GPIO | Current Usage |
|---------|------|---------------|
| ADC1_CH0 | GPIO1 | I2C SDA (not ADC) |
| ADC1_CH1 | GPIO2 | I2C SCL (not ADC) |
| ADC1_CH2 | GPIO3 | Available (strapping pin - avoid) |
| ADC1_CH3 | GPIO4 | Available |
| ADC1_CH4 | GPIO5 | :white_check_mark: **Solar Voltage** |
| ADC1_CH5 | GPIO6 | Available |
| ADC1_CH6 | GPIO7 | Available |

### ADC2 (Not available during WiFi operation)

| Channel | GPIO | Notes |
|---------|------|-------|
| ADC2_CH8 | GPIO19 | USB D- |
| ADC2_CH9 | GPIO20 | USB D+ |

---

## chicken-tractor.yaml GPIO Summary

### Direct GPIO Usage

| GPIO | Function | Configuration | File |
|------|----------|---------------|------|
| GPIO0 | User button | `binary_sensor` (display toggle) | chicken-tractor.yaml:382 |
| GPIO1 | I2C SDA | `i2c.sda` (sensor_bus) | chicken-tractor.yaml:137 |
| GPIO2 | I2C SCL | `i2c.scl` (sensor_bus) | chicken-tractor.yaml:138 |
| GPIO5 | Solar voltage | `sensor.adc` | chicken-tractor.yaml:251 |
| GPIO38 | Charge PWM | `output.ledc` | chicken-tractor.yaml:284 |
| GPIO8 | LoRa CS | `sx126x.cs_pin` | heltec-lora.yaml:10 |
| GPIO9 | LoRa SCK | `spi.clk_pin` | heltec-lora.yaml:2 |
| GPIO10 | LoRa MOSI | `spi.mosi_pin` | heltec-lora.yaml:3 |
| GPIO11 | LoRa MISO | `spi.miso_pin` | heltec-lora.yaml:4 |
| GPIO12 | LoRa RST | `sx126x.rst_pin` | heltec-lora.yaml:11 |
| GPIO13 | LoRa BUSY | `sx126x.busy_pin` | heltec-lora.yaml:12 |
| GPIO14 | LoRa DIO1 | `sx126x.dio1_pin` | heltec-lora.yaml:13 |
| GPIO17 | OLED SDA | `i2c.sda` (display_bus) | heltec-display.yaml:14 |
| GPIO18 | OLED SCL | `i2c.scl` (display_bus) | heltec-display.yaml:15 |
| GPIO21 | OLED Reset | `display.reset_pin` | heltec-display.yaml:21 |
| GPIO36 | OLED Vext | `pinMode()` in on_boot | heltec-display.yaml:7 |
| GPIO48 | Load switch | `output.ledc` | chicken-tractor.yaml:289 |

### I2C Buses

| Bus ID | SDA | SCL | Devices |
|--------|-----|-----|---------|
| display_bus | GPIO17 | GPIO18 | SSD1306 OLED (0x3C) |
| sensor_bus | GPIO1 | GPIO2 | INA219 x2 (0x40, 0x41) |

---

## Available GPIOs

The following GPIOs are exposed on the headers and not currently used:

| GPIO | Header | Pin | ADC | Notes |
|------|--------|-----|-----|-------|
| GPIO3 | J3 | 14 | ADC1_CH2 | **Strapping pin** - avoid for analog input |
| GPIO4 | J3 | 15 | ADC1_CH3 | Touch capable |
| GPIO6 | J3 | 17 | ADC1_CH5 | Touch capable |
| GPIO7 | J3 | 18 | ADC1_CH6 | Touch capable |
| GPIO26 | J2 | 15 | | SPI CS1 alternate |
| GPIO33 | J2 | 12 | | SPI alternate |
| GPIO34 | J2 | 11 | | SPI alternate |
| GPIO35 | J2 | 10 | | LED_Write capable |
| GPIO37 | J3 | 4 | | ADC_Ctrl |
| GPIO39-42 | J3 | 7-10 | | JTAG pins |
| GPIO45 | J3 | 6 | | **Strapping pin** - use with caution |
| GPIO46 | J3 | 5 | | **Input only**, strapping pin |
| GPIO47 | J2 | 13 | | |

---

## Commented/Disabled Features

The following GPIO assignments are commented out in chicken-tractor.yaml:

| GPIO | Intended Function | Status |
|------|-------------------|--------|
| GPIO47 | Dallas 1-Wire temperature sensor | Commented out (lines 68-71) |

---

## References

- [Heltec WiFi LoRa 32 V3 Pinout Diagram](heltec_wifi_lora_32_v3_pinout.jpg)
- [ESP32-S3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf)
- [ESPHome ESP32-S3 Documentation](https://esphome.io/components/esp32.html)
