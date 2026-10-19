#pragma once

#include <ctime>

namespace HalClock {

inline bool& syncedRef() {
  static bool synced = false;
  return synced;
}

inline std::time_t& epochRef() {
  static std::time_t epoch = 0;
  return epoch;
}

inline bool isSynced() { return syncedRef(); }
inline std::time_t now() { return epochRef(); }
inline void set(const std::time_t epoch, const bool synced = true) {
  epochRef() = epoch;
  syncedRef() = synced;
}

}  // namespace HalClock
