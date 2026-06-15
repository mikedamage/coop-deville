#pragma once

#include "esphome/components/time/real_time_clock.h"

namespace esphome {
namespace lora_remote_node {

// Time source whose clock is set from a wall-clock time block carried in a
// (unicast) poll request rather than polled. Remote nodes have no reliable
// WiFi/internet, so SNTP and
// Home Assistant time platforms are unavailable; this platform lets the node
// push the gateway-supplied epoch into the system clock using the same
// self-synchronize pattern the GPS and Home Assistant time platforms use
// (synchronize_epoch_() is protected on RealTimeClock and reached here because
// this class derives from it).
class LoraNodeTime : public time::RealTimeClock {
 public:
  // Time arrives out-of-band via set_epoch(); there is nothing to poll.
  void update() override {}

  void set_epoch(uint32_t epoch) { this->synchronize_epoch_(epoch); }
};

}  // namespace lora_remote_node
}  // namespace esphome
