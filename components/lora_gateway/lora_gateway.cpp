#include "lora_gateway.h"
#include "esphome/core/log.h"

namespace esphome {
namespace lora_gateway {

static const char *const TAG = "lora_gateway";

// RemoteNode implementation

sensor::Sensor *RemoteNode::get_or_create_sensor(const std::string &name) {
  auto it = this->sensors_.find(name);
  if (it != this->sensors_.end()) {
    return it->second;
  }
  // TODO: Phase 4 - Create and register new sensor with device association
  return nullptr;
}

binary_sensor::BinarySensor *RemoteNode::get_or_create_binary_sensor(const std::string &name) {
  auto it = this->binary_sensors_.find(name);
  if (it != this->binary_sensors_.end()) {
    return it->second;
  }
  // TODO: Phase 4 - Create and register new binary sensor with device association
  return nullptr;
}

// LoraGateway implementation

void LoraGateway::setup() {
  ESP_LOGCONFIG(TAG, "Setting up LoRa Gateway...");

  if (this->remote_nodes_.empty()) {
    ESP_LOGW(TAG, "No remote nodes configured");
    return;
  }

  // Initialize state
  this->current_poll_index_ = 0;
  this->waiting_for_response_ = false;
  this->last_poll_start_ms_ = 0;
  this->last_poll_cycle_ms_ = 0;
  this->last_time_sync_ms_ = 0;

  ESP_LOGI(TAG, "Gateway configured with %d remote nodes", this->remote_nodes_.size());
}

void LoraGateway::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRa Gateway:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  ESP_LOGCONFIG(TAG, "  Response Timeout: %u ms", this->response_timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Poll Interval: %u ms", this->poll_interval_ms_);
  if (this->time_sync_interval_ms_ > 0) {
    ESP_LOGCONFIG(TAG, "  Time Sync Interval: %u ms", this->time_sync_interval_ms_);
  }
  ESP_LOGCONFIG(TAG, "  Send ACK: %s", this->send_ack_ ? "true" : "false");
  ESP_LOGCONFIG(TAG, "  Stale Sensor Behavior: %s",
                this->stale_behavior_ == StaleSensorBehavior::KEEP_LAST_VALUE ? "keep" : "invalidate");
  ESP_LOGCONFIG(TAG, "  Remote Nodes:");
  for (auto *node : this->remote_nodes_) {
    ESP_LOGCONFIG(TAG, "    - %s (0x%02X)", node->get_name().c_str(), node->get_address());
  }
}

void LoraGateway::loop() {
  // TODO: Phase 3 - Implement polling state machine
}

bool LoraGateway::should_start_polling_() {
  if (this->time_ == nullptr) {
    return true;
  }
  // Wait for time to be valid before starting polling
  auto now = this->time_->now();
  return now.is_valid();
}

void LoraGateway::poll_next_node_() {
  // TODO: Phase 3 - Implement poll next node
}

void LoraGateway::send_poll_request_(RemoteNode *node) {
  // TODO: Phase 3 - Implement send poll request
}

void LoraGateway::send_ack_packet_(RemoteNode *node) {
  // TODO: Phase 3 - Implement send ACK
}

void LoraGateway::broadcast_time_sync_() {
  // TODO: Phase 3 - Implement time sync broadcast
}

void LoraGateway::on_packet(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // TODO: Phase 3 - Implement packet reception handling
}

void LoraGateway::handle_poll_response_(const std::vector<uint8_t> &packet, float rssi, float snr) {
  // TODO: Phase 3 - Implement response handling
}

void LoraGateway::process_complete_response_(RemoteNode *node, const std::vector<uint8_t> &payload) {
  // TODO: Phase 4 - Implement sensor deserialization and update
}

void LoraGateway::handle_timeout_(RemoteNode *node) {
  // TODO: Phase 3 - Implement timeout handling
}

void LoraGateway::update_metrics_sensors_() {
  // TODO: Phase 5 - Implement metrics sensor updates
}

RemoteNode *LoraGateway::find_node_by_address_(uint8_t address) {
  for (auto *node : this->remote_nodes_) {
    if (node->get_address() == address) {
      return node;
    }
  }
  return nullptr;
}

}  // namespace lora_gateway
}  // namespace esphome
