# Implementation Plan: LoRa Gateway Polling System

## Overview

Replace the push-based `packet_transport` system with a star-topology polling architecture where a gateway device polls up to 10 remote nodes in round-robin fashion over LoRa. Remote sensor data will be transparently forwarded to Home Assistant as entities belonging to distinct virtual devices.

The gateway will also periodically broadcast time synchronization packets to keep remote nodes' RTCs synchronized, enabling accurate timekeeping on nodes without reliable WiFi/NTP access.

## Components

- **`lora_gateway`**: Gateway component that polls remote nodes and forwards their sensor data to Home Assistant
- **`lora_remote_node`**: Remote node component that listens for polls and responds with sensor states

## Protocol Design

### Addressing

- **Address space**: 0x01 - 0xFE (254 usable addresses)
- **Broadcast address**: 0xFF (all nodes respond to this address)
- **Address assignment**: Manual, user responsible for uniqueness

### Packet Formats

All multi-byte integers use little-endian byte order.

#### Poll Request Packet (Gateway → Remote Node)

```
Byte 0:      Gateway address (1 byte)
Byte 1:      Target address (1 byte)
Byte 2:      Command: 0x01 = POLL_REQUEST
```

Total: 3 bytes

#### Poll Response Packet (Remote Node → Gateway)

```
Byte 0:      Source address (1 byte)
Byte 1:      Destination address (1 byte)
Byte 2:      Command: 0x02 = POLL_RESPONSE
Byte 3:      Packet number (1 byte): 0x00 for single packet, 0x01-0xNN for multi-packet
Byte 4:      Total packets (1 byte): 0x01 for single packet, 0x02-0xNN for multi-packet
Bytes 5+:    Payload (sensor data)
```

#### Time Sync Broadcast Packet (Gateway → All Remote Nodes)

```
Byte 0:      Gateway address (1 byte)
Byte 1:      Broadcast address 0xFF (1 byte)
Byte 2:      Command: 0x03 = TIME_SYNC
Bytes 3-6:   Timestamp (4 bytes, uint32_t, seconds since epoch)
```

Total: 7 bytes

#### Acknowledgment Packet (Gateway → Remote Node)

Sent after successfully receiving a complete poll response (optional, configurable).

```
Byte 0:      Gateway address (1 byte)
Byte 1:      Target address (1 byte)
Byte 2:      Command: 0x04 = ACK
```

Total: 3 bytes

#### Sensor Data Payload Format

Reuse packet_transport's serialization scheme (without encryption):

```
For each sensor:
  SENSOR_KEY:     1 byte (0x01)
  Float value:    4 bytes (IEEE 754)
  Name length:    1 byte
  Name:           (length) bytes

For each binary sensor:
  BINARY_SENSOR_KEY:  1 byte (0x02)
  Bool value:         1 byte (0x00 or 0x01)
  Name length:        1 byte
  Name:               (length) bytes
```

### Packet Size Constraints

- Maximum LoRa packet size: 255 bytes (sx126x limit)
- If sensor data exceeds 255 - 5 = 250 bytes (accounting for header), split into multiple packets
- Multi-packet responses use sequential packet numbers starting at 0x01

## Implementation Phases

---

## Phase 1: Project Structure and Protocol Foundation

### Step 1.1: Create Component Directory Structure

Create external component directories:

```
esphome/components/lora_gateway/
├── __init__.py
├── lora_gateway.h
├── lora_gateway.cpp
└── text_sensor.py

esphome/components/lora_remote_node/
├── __init__.py
├── lora_remote_node.h
└── lora_remote_node.cpp
```

### Step 1.2: Define Protocol Constants

Create a shared header for protocol constants (decide whether to duplicate or share):

```cpp
// Protocol commands
const uint8_t CMD_POLL_REQUEST = 0x01;
const uint8_t CMD_POLL_RESPONSE = 0x02;
const uint8_t CMD_TIME_SYNC = 0x03;
const uint8_t CMD_ACK = 0x04;

// Payload keys (from packet_transport)
const uint8_t SENSOR_KEY = 0x01;
const uint8_t BINARY_SENSOR_KEY = 0x02;

// Special addresses
const uint8_t BROADCAST_ADDRESS = 0xFF;

// Packet structure
const size_t MAX_PACKET_SIZE = 255;
const size_t POLL_REQUEST_SIZE = 3;
const size_t POLL_RESPONSE_HEADER_SIZE = 5;
const size_t TIME_SYNC_PACKET_SIZE = 7;
const size_t ACK_PACKET_SIZE = 3;
const size_t MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - POLL_RESPONSE_HEADER_SIZE;
```

### Step 1.3: Define Common Data Structures

For both components, define structures for managing packet assembly/disassembly.

---

## Phase 2: Remote Node Implementation

### Step 2.1: Define `lora_remote_node` Configuration Schema

In `lora_remote_node/__init__.py`, define the configuration schema following ESPHome conventions:

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, binary_sensor, time
from esphome.const import CONF_ID, CONF_SENSORS, CONF_BINARY_SENSORS, CONF_TIME_ID

CODEOWNERS = ["@mikedamage"]
DEPENDENCIES = ["sx126x"]

CONF_SX126X_ID = "sx126x_id"
CONF_ADDRESS = "address"

lora_remote_node_ns = cg.esphome_ns.namespace("lora_remote_node")
LoraRemoteNode = lora_remote_node_ns.class_("LoraRemoteNode", cg.Component)

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(LoraRemoteNode),
    cv.Required(CONF_SX126X_ID): cv.use_id(sx126x.SX126x),
    cv.Required(CONF_ADDRESS): cv.hex_uint8_t,
    cv.Required(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Optional(CONF_SENSORS, default=[]): cv.ensure_list(cv.use_id(sensor.Sensor)),
    cv.Optional(CONF_BINARY_SENSORS, default=[]): cv.ensure_list(cv.use_id(binary_sensor.BinarySensor)),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = await cg.get_variable(config[CONF_SX126X_ID])
    cg.add(var.set_sx126x(radio))
    cg.add(var.set_address(config[CONF_ADDRESS]))

    time_source = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time_source(time_source))

    for sensor_id in config[CONF_SENSORS]:
        sens = await cg.get_variable(sensor_id)
        cg.add(var.add_sensor(sens))

    for binary_sensor_id in config[CONF_BINARY_SENSORS]:
        sens = await cg.get_variable(binary_sensor_id)
        cg.add(var.add_binary_sensor(sens))
```

### Step 2.2: Implement `LoraRemoteNode` Class Header

In `lora_remote_node.h`, following ESPHome C++ conventions (Google C++ Style Guide):

```cpp
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sx126x/sx126x.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/time/real_time_clock.h"

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
```

### Step 2.3: Implement Packet Reception Logic

Implement `on_packet()` to:
- Check packet type (poll request or time sync)
- For poll requests:
  - Validate packet length >= 3, command == CMD_POLL_REQUEST
  - Check if addressed to this node (target_addr == address_ || target_addr == BROADCAST_ADDRESS)
  - Extract gateway address from packet
  - Trigger `handle_poll_request()`
- For time sync:
  - Validate packet length == 7, command == CMD_TIME_SYNC
  - Check if addressed to broadcast (target_addr == BROADCAST_ADDRESS)
  - Trigger `handle_time_sync()`

### Step 2.3a: Implement Time Sync Handler

Implement `handle_time_sync()`:
- Extract 4-byte timestamp from packet (bytes 3-6)
- Convert little-endian bytes to uint32_t seconds since epoch
- Set RTC time via `time_->set_time(timestamp)`
- Log time sync event with debug level

### Step 2.4: Implement Sensor Serialization

Implement `serialize_sensor_data()`:
- Iterate through all sensors, serialize as: [SENSOR_KEY][float_value][name_len][name]
- Iterate through all binary sensors, serialize as: [BINARY_SENSOR_KEY][bool_value][name_len][name]
- Return byte vector of serialized data

### Step 2.5: Implement Response Packet Builder

Implement `build_response_packets()`:
- Serialize sensor data
- Calculate if data fits in single packet (payload <= 250 bytes)
- If single packet: return vector with one packet [src][dst][cmd][0x00][0x01][payload]
- If multi-packet: split payload and create multiple packets with incrementing packet numbers
- Return vector of packets

### Step 2.6: Implement Response Transmission

Implement `handle_poll_request()`:
- Build response packets
- Transmit each packet via `sx126x_->transmit_packet()`
- Add small delay between multi-packet transmissions (10-20ms) to allow gateway RX turnaround

### Step 2.7: Code Generation for Remote Node

In `lora_remote_node/__init__.py`, implement `to_code()`:
- Generate C++ code to instantiate LoraRemoteNode
- Set sx126x parent
- Set address
- Register sensors and binary sensors
- Register as SX126x listener

---

## Phase 3: Gateway Core Implementation

### Step 3.1: Define `lora_gateway` Configuration Schema

In `lora_gateway/__init__.py`, following ESPHome conventions:

```python
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import time
from esphome.const import CONF_ID, CONF_NAME, CONF_TIME_ID

CODEOWNERS = ["@mikedamage"]
DEPENDENCIES = ["sx126x"]

CONF_SX126X_ID = "sx126x_id"
CONF_ADDRESS = "address"
CONF_REMOTE_NODES = "remote_nodes"
CONF_RESPONSE_TIMEOUT = "response_timeout"
CONF_POLL_INTERVAL = "poll_interval"
CONF_TIME_SYNC_INTERVAL = "time_sync_interval"
CONF_SEND_ACK = "send_ack"
CONF_STALE_SENSOR_BEHAVIOR = "stale_sensor_behavior"

lora_gateway_ns = cg.esphome_ns.namespace("lora_gateway")
LoraGateway = lora_gateway_ns.class_("LoraGateway", cg.Component)
RemoteNode = lora_gateway_ns.class_("RemoteNode")

StaleSensorBehavior = lora_gateway_ns.enum("StaleSensorBehavior")
STALE_SENSOR_BEHAVIOR_OPTIONS = {
    "keep": StaleSensorBehavior.KEEP_LAST_VALUE,
    "invalidate": StaleSensorBehavior.INVALIDATE,
}

REMOTE_NODE_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(RemoteNode),
    cv.Required(CONF_ADDRESS): cv.hex_uint8_t,
    cv.Required(CONF_NAME): cv.string,
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(LoraGateway),
    cv.Required(CONF_SX126X_ID): cv.use_id(sx126x.SX126x),
    cv.Required(CONF_ADDRESS): cv.hex_uint8_t,
    cv.Required(CONF_REMOTE_NODES): cv.ensure_list(REMOTE_NODE_SCHEMA),
    cv.Required(CONF_RESPONSE_TIMEOUT): cv.positive_time_period_milliseconds,
    cv.Required(CONF_POLL_INTERVAL): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
    cv.Optional(CONF_TIME_SYNC_INTERVAL): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_SEND_ACK, default=False): cv.boolean,
    cv.Optional(CONF_STALE_SENSOR_BEHAVIOR, default="keep"): cv.enum(STALE_SENSOR_BEHAVIOR_OPTIONS, lower=True),
}).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    radio = await cg.get_variable(config[CONF_SX126X_ID])
    cg.add(var.set_sx126x(radio))
    cg.add(var.set_address(config[CONF_ADDRESS]))
    cg.add(var.set_response_timeout(config[CONF_RESPONSE_TIMEOUT]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_send_ack(config[CONF_SEND_ACK]))
    cg.add(var.set_stale_sensor_behavior(config[CONF_STALE_SENSOR_BEHAVIOR]))

    if CONF_TIME_ID in config:
        time_source = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(time_source))

    if CONF_TIME_SYNC_INTERVAL in config:
        cg.add(var.set_time_sync_interval(config[CONF_TIME_SYNC_INTERVAL]))

    for node_config in config[CONF_REMOTE_NODES]:
        node_var = cg.new_Pvariable(node_config[CONF_ID])
        cg.add(node_var.set_address(node_config[CONF_ADDRESS]))
        cg.add(node_var.set_name(node_config[CONF_NAME]))
        cg.add(var.add_remote_node(node_var))
```

### Step 3.2: Define Remote Node Data Structures

In `lora_gateway.h`, following ESPHome C++ conventions:

```cpp
#pragma once

#include <map>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/sx126x/sx126x.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"

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

  const char *get_name() const { return this->name_; }
  void set_name(const char *name) { this->name_ = name; }

  uint32_t get_device_id() const { return this->device_id_; }
  void set_device_id(uint32_t id) { this->device_id_ = id; }

  RemoteNodeMetrics &get_metrics() { return this->metrics_; }

  sensor::Sensor *get_or_create_sensor(const std::string &name);
  binary_sensor::BinarySensor *get_or_create_binary_sensor(const std::string &name);

 protected:
  uint8_t address_{0};
  const char *name_{nullptr};
  uint32_t device_id_{0};
  RemoteNodeMetrics metrics_;
  std::map<std::string, sensor::Sensor *> sensors_;
  std::map<std::string, binary_sensor::BinarySensor *> binary_sensors_;
};
```

### Step 3.3: Implement `LoraGateway` Class Header

In `lora_gateway.h` (continuation):

```cpp
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
  uint32_t response_timeout_ms_{0};
  uint32_t poll_interval_ms_{0};
  uint32_t time_sync_interval_ms_{0};
  bool send_ack_{false};
  time::RealTimeClock *time_{nullptr};
  StaleSensorBehavior stale_behavior_{StaleSensorBehavior::KEEP_LAST_VALUE};

  std::vector<RemoteNode *> remote_nodes_;
  size_t current_poll_index_{0};
  uint32_t last_poll_start_ms_{0};
  uint32_t last_time_sync_ms_{0};
  bool waiting_for_response_{false};
  RemoteNode *current_node_{nullptr};

  // Multi-packet assembly
  std::map<uint8_t, std::vector<std::vector<uint8_t>>> partial_responses_;

  // Metrics sensors
  text_sensor::TextSensor *timeout_list_sensor_{nullptr};
  text_sensor::TextSensor *last_heard_list_sensor_{nullptr};
  text_sensor::TextSensor *signal_quality_list_sensor_{nullptr};

  void poll_next_node_();
  void send_poll_request_(RemoteNode *node);
  void send_ack_(RemoteNode *node);
  void broadcast_time_sync_();
  void handle_poll_response_(const std::vector<uint8_t> &packet, float rssi, float snr);
  void process_complete_response_(RemoteNode *node, const std::vector<uint8_t> &payload);
  void handle_timeout_(RemoteNode *node);
  void update_metrics_sensors_();

  bool should_start_polling_();
};

}  // namespace lora_gateway
}  // namespace esphome
```

### Step 3.4: Implement Setup and Initialization

Implement `setup()`:
- Register as SX126x listener
- Validate configuration (addresses, remote nodes, etc.)
- Set initial state (current_poll_index = 0, waiting_for_response = false)

Implement `should_start_polling()`:
- If no time source configured, return true
- If time source configured, check if time is valid (not 1970, etc.)
- Return true only when time is synced

### Step 3.5: Implement Polling State Machine

Implement `loop()`:
- If not `should_start_polling()`, return early
- Check if time sync interval elapsed (if time_sync_interval_ms_ > 0):
  - If `millis() - last_time_sync_ms_ > time_sync_interval_ms_`: call `broadcast_time_sync()`
- If `waiting_for_response_`:
  - Check if timeout elapsed: `millis() - last_poll_start_ms_ > response_timeout_ms_`
  - If timeout: call `handle_timeout()`, set `waiting_for_response_ = false`, schedule next poll
- If not `waiting_for_response_`:
  - Check if poll interval elapsed since last cycle
  - If elapsed: call `poll_next_node()`

Implement `poll_next_node()`:
- Get next node from `remote_nodes_[current_poll_index_]`
- Send poll request
- Set `waiting_for_response_ = true`
- Set `current_node_` pointer
- Record `last_poll_start_ms_ = millis()`
- Increment `current_poll_index_` (wrap around at end of list)

### Step 3.5a: Implement Time Sync Broadcast

Implement `broadcast_time_sync()`:
- Check if time source is configured, if not return early
- Get current time from time component: `time_->now().timestamp` (seconds since epoch, uint32_t)
- Build packet: [gateway_address_][BROADCAST_ADDRESS][CMD_TIME_SYNC][4 bytes timestamp in little-endian]
- Call `sx126x_->transmit_packet(packet)`
- Update `last_time_sync_ms_ = millis()`
- Log time sync broadcast with debug level

### Step 3.6: Implement Poll Request Transmission

Implement `send_poll_request()`:
- Build packet: [gateway_address_][node->get_address()][CMD_POLL_REQUEST]
- Call `sx126x_->transmit_packet(packet)`

### Step 3.7: Implement Response Reception

Implement `on_packet()`:
- Validate packet is poll response (length >= 5, command == CMD_POLL_RESPONSE)
- Validate destination address matches gateway address
- Extract source address and find corresponding RemoteNode
- If not `waiting_for_response_` or source doesn't match `current_node_`, ignore (late/duplicate)
- Extract packet number and total packets
- If single packet (packet_num=0, total=1): call `process_complete_response()` immediately
- If multi-packet: store in `partial_responses_[source_addr]`, check if complete, then process
- Update node metrics (RSSI, SNR, latency, last_heard)
- Set `waiting_for_response_ = false`

### Step 3.7a: Implement ACK Transmission

Implement `send_ack()`:
- Check if `send_ack_` is enabled, if not return early
- Build packet: [gateway_address_][node->get_address()][CMD_ACK]
- Call `sx126x_->transmit_packet(packet)`
- Log ACK sent with debug level

Call `send_ack()` from `handle_poll_response()` after successfully processing a complete response (single-packet or final packet of multi-packet).

### Step 3.8: Implement Multi-Packet Assembly

When receiving multi-packet response:
- Store packet payload in `partial_responses_[source_addr][packet_num]`
- Check if all packets received (packet count == total_packets)
- If complete: concatenate all payloads in order, call `process_complete_response()`, clear `partial_responses_[source_addr]`
- Add timeout mechanism to clear stale partial responses (e.g., after 5 seconds)

### Step 3.9: Implement Timeout Handling

Implement `handle_timeout()`:
- Set `current_node_->metrics_.last_response_received = false`
- If `stale_behavior_ == INVALIDATE`: set all node sensors to NaN/invalid
- Update metrics sensors

---

## Phase 4: Multi-Device Home Assistant Integration

### Step 4.1: Register Virtual Devices

In gateway `setup()`:
- For each RemoteNode, create an ESPHome `Device` object
- Set device_id (use sequential IDs starting at 1)
- Set device name from node configuration
- Call `App.register_device(device)`
- Store device_id in RemoteNode

### Step 4.2: Dynamic Sensor Creation

Implement `RemoteNode::get_or_create_sensor()`:
- Check if sensor with given name exists in `sensors_` map
- If exists: return existing sensor
- If not: create new sensor, set name, call `set_device(device)` to associate with virtual device
- Register with `App.register_sensor()`
- Store in map and return

Implement similar logic for `get_or_create_binary_sensor()`

### Step 4.3: Deserialize and Update Sensors

Implement `process_complete_response()`:
- Parse payload using packet_transport serialization format
- For each sensor in payload:
  - Call `node->get_or_create_sensor(name)`
  - Call `sensor->publish_state(value)`
- For each binary sensor:
  - Call `node->get_or_create_binary_sensor(name)`
  - Call `binary_sensor->publish_state(value)`
- Update node metrics

---

## Phase 5: Metrics and Diagnostics

### Step 5.1: Define Text Sensor Configuration

In `lora_gateway/text_sensor.py`:

```python
from esphome.components import text_sensor
import esphome.config_validation as cv

CONF_TIMEOUT_LIST = 'timeout_list'
CONF_LAST_HEARD_LIST = 'last_heard_list'
CONF_SIGNAL_QUALITY_LIST = 'signal_quality_list'

CONFIG_SCHEMA = cv.Schema({
    cv.Optional(CONF_TIMEOUT_LIST): text_sensor.text_sensor_schema(),
    cv.Optional(CONF_LAST_HEARD_LIST): text_sensor.text_sensor_schema(),
    cv.Optional(CONF_SIGNAL_QUALITY_LIST): text_sensor.text_sensor_schema(),
})
```

### Step 5.2: Implement Metrics Formatting

Implement `update_metrics_sensors()`:

For timeout list sensor:
```
Format: "0x02: timeout, 0x05: timeout"
Logic: iterate nodes, include those with last_response_received == false
```

For last heard list sensor:
```
Format: "0x02: 1234567890, 0x03: 1234567895, 0x05: never"
Logic: iterate nodes, show last_heard_ms or "never" if 0
```

For signal quality list sensor:
```
Format: "0x02: RSSI=-85dBm SNR=10dB, 0x03: RSSI=-90dBm SNR=8dB"
Logic: iterate nodes, show last RSSI/SNR values
```

Call `update_metrics_sensors()` after each poll response or timeout.

### Step 5.3: Code Generation for Metrics Sensors

In `lora_gateway/__init__.py`, extend `to_code()`:
- Create and register text sensors for metrics
- Call setters on gateway component

---

## Phase 6: Code Generation and Configuration

### Step 6.1: Complete `lora_remote_node` Code Generation

In `lora_remote_node/__init__.py`, implement complete `to_code()`:
- Import necessary modules
- Get sx126x component reference
- Create LoraRemoteNode instance
- Set address, sx126x parent
- Add sensors and binary sensors
- Register as listener
- Register component

### Step 6.2: Complete `lora_gateway` Code Generation

In `lora_gateway/__init__.py`, implement complete `to_code()`:
- Import necessary modules
- Get sx126x component reference
- Get time component reference (if specified)
- Create LoraGateway instance
- Set configuration (address, timeouts, poll interval, stale behavior)
- Create RemoteNode instances for each configured node
- Add remote nodes to gateway
- Register devices for each remote node
- Register component

### Step 6.3: Validation and Error Handling

Add validation logic:
- Ensure address is in valid range (0x01-0xFE)
- Ensure no duplicate addresses in gateway remote_nodes list
- Ensure response_timeout < poll_interval (warn if not)
- Validate time_id references valid RTC component

---

## Phase 7: Testing and Debugging

### Step 7.1: Create Test Configuration for Remote Node

Create a minimal test config:
```yaml
lora_remote_node:
  sx126x_id: lora_radio
  address: 0x02
  time_id: internal_rtc
  sensors:
    - battery_voltage
    - solar_voltage
  binary_sensors:
    - light_on
```

Compile and verify code generation works.

### Step 7.2: Create Test Configuration for Gateway

Create a minimal test config:
```yaml
lora_gateway:
  sx126x_id: lora_radio
  address: 0x01
  response_timeout: 2s
  poll_interval: 10s
  time_id: sntp_time
  time_sync_interval: 60s
  send_ack: false  # optional, default false; enable for future retry support
  remote_nodes:
    - id: test_node
      address: 0x02
      name: Test Node
```

Compile and verify code generation works.

### Step 7.3: Add Debug Logging

Add ESP_LOGx statements at key points:
- Poll request sent (gateway)
- Poll request received (remote)
- Response sent (remote)
- Response received (gateway)
- ACK sent (gateway, if enabled)
- ACK received (remote, for future retry logic)
- Timeout occurred (gateway)
- Multi-packet assembly progress
- Sensor updates
- Time sync broadcast sent (gateway)
- Time sync received and applied (remote)

### Step 7.4: Test Single Node Communication

- Flash remote node firmware
- Flash gateway firmware
- Monitor logs to verify:
  - Poll requests are sent
  - Remote node receives and responds
  - Gateway receives response and updates sensors
  - Metrics sensors update correctly

### Step 7.5: Test Multi-Node Communication

- Add second remote node with different address
- Verify round-robin polling works correctly
- Verify no cross-talk or interference
- Verify metrics track both nodes independently

### Step 7.6: Test Edge Cases

- Timeout handling: power off a node, verify gateway handles timeout
- Multi-packet: add many sensors to exceed 250 bytes, verify fragmentation works
- Broadcast: send poll to 0xFF, verify all nodes respond (test carefully!)
- Stale sensor behavior: test both 'keep' and 'invalidate' modes
- Time sync: test gateway waits for time before polling

### Step 7.7: Test Time Synchronization

- Configure gateway with time_sync_interval (e.g., 60s)
- Configure remote nodes with time_id
- Monitor logs to verify:
  - Gateway broadcasts time sync packets at configured interval
  - Remote nodes receive time sync packets
  - Remote nodes update their RTC time
  - Remote node time stays in sync with gateway
- Test with nodes that start with incorrect/unsynced time
- Verify remote nodes can maintain reasonable timekeeping between syncs

---

## Phase 8: Documentation and Integration

### Step 8.1: Update Device Configurations

Update `chicken-tractor.yaml`:
- Replace `packet_transport` with `lora_remote_node`
- Configure sensors and binary sensors
- Set address

Update `lora-gateway.yaml`:
- Replace `packet_transport` with `lora_gateway`
- Configure remote_nodes list
- Configure metrics sensors
- Set polling parameters

### Step 8.2: Update Circuit Documentation

Document any configuration changes in `CIRCUIT_CONNECTIONS.md` if needed.

### Step 8.3: Create Component README

Document:
- Component purpose and architecture
- Configuration options and examples
- Protocol specification
- Troubleshooting tips
- Future enhancements (encryption, adaptive timeouts, etc.)

### Step 8.4: Compile and Test Final Configurations

- Compile both chicken-tractor and lora-gateway
- Flash devices
- Verify end-to-end operation
- Verify Home Assistant integration shows separate devices

---

## Future Enhancements (Not in Initial Implementation)

1. **Retry Logic with Random Backoff** (builds on ACK support)
   - Remote node waits for ACK after sending response
   - If no ACK received within timeout, retry with random backoff
   - Configurable max retry count
   - Exponential backoff with jitter to avoid repeated collisions

2. **Encryption & Authentication**
   - Add XXTEA encryption option (from packet_transport)
   - Add shared key authentication
   - Add rolling code anti-replay protection

3. **Adaptive Timeouts**
   - Track response latency per node
   - Adjust timeout dynamically based on historical performance
   - Reduce polling cycle time for responsive nodes

4. **Priority/Urgent Messages**
   - Allow remote nodes to send unsolicited urgent packets
   - Gateway checks for urgent messages between polls

5. **Wake Windows**
   - Remote nodes deep sleep and wake periodically
   - Gateway synchronizes polling with wake windows
   - Optimize power consumption

6. **Collision Avoidance**
   - Use sx126x CAD (Channel Activity Detection)
   - Defer polling if channel is busy

7. **Mesh Networking**
   - Allow remote nodes to relay messages
   - Extend range beyond single hop

8. **Firmware OTA via LoRa**
   - Transfer firmware updates over LoRa
   - Useful for nodes without reliable WiFi

---

## Success Criteria

- ✅ Gateway successfully polls 10 remote nodes in round-robin fashion
- ✅ Zero packet collisions under normal operation
- ✅ Remote sensor data appears in Home Assistant as separate devices
- ✅ Timeout handling works correctly
- ✅ Multi-packet responses work for large payloads
- ✅ Metrics sensors provide visibility into network health
- ✅ Time synchronization broadcasts keep remote nodes' RTCs in sync
- ✅ Code compiles without errors
- ✅ Configurations are clean and maintainable

---

## Development Workflow

Following the workflow from CLAUDE.md and ESPHome conventions:

1. **After each phase**: Compile both configurations to verify validity
2. **If missing secrets error**: Run `scripts/populate-secrets` and retry
3. **If linker error**: Clean build environment, try again, then modify config if needed
4. **Test incrementally**: Don't wait until all phases are done to test

### ESPHome Coding Conventions

Follow the ESPHome AI Collaboration Guide (`/Users/mike/dev/esphome/.ai/instructions.md`):

- **Python**: Use `ruff` and `flake8` for linting, follow PEP 8
- **C++**: Use `clang-format`, follow Google C++ Style Guide
- **Namespaces**: All C++ code in `esphome::component_name` namespace
- **Protected methods**: Use trailing underscore (e.g., `handle_timeout_()`)
- **Member variables**: Use trailing underscore and `this->` prefix
- **Setters**: Inline in header for simple assignments
- **Component metadata**: Include `CODEOWNERS`, `DEPENDENCIES`, etc.
- **Code generation**: Use `async def to_code(config)` pattern with `cg.add()`

## Estimated Timeline

- Phase 1 (Structure): 1-2 hours
- Phase 2 (Remote Node + Time Sync): 2-4 hours
- Phase 3 (Gateway Core + Time Sync): 3-5 hours
- Phase 4 (HA Integration): 2-3 hours
- Phase 5 (Metrics): 1-2 hours
- Phase 6 (Code Gen): 2-3 hours
- Phase 7 (Testing + Time Sync): 3-5 hours
- Phase 8 (Documentation): 1-2 hours

**Total**: ~15-26 hours of development time
