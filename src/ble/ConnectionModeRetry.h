#pragma once

#include <cstdint>

namespace plugin_ble {

// Bounded retries: a rejected parameter request must not become an idle polling loop.
class ConnectionModeRetry {
 public:
  void reset(uint32_t now) {
    pending_ = true;
    attempts_ = 0;
    nextAttemptAt_ = now;
  }
  bool due(uint32_t now) const { return pending_ && static_cast<int32_t>(now - nextAttemptAt_) >= 0; }
  bool exhausted() const { return attempts_ >= 3; }
  void attempted(uint32_t now) {
    nextAttemptAt_ = now + (1000U << attempts_);
    ++attempts_;
  }
  void finish() { pending_ = false; }

 private:
  bool pending_ = false;
  uint8_t attempts_ = 0;
  uint32_t nextAttemptAt_ = 0;
};

}  // namespace plugin_ble
