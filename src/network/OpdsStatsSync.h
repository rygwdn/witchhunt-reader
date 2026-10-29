#pragma once
#include <cstdint>
#include <string>

#include "OpdsServerStore.h"

namespace OpdsStatsSync {
struct Session {
  const char* id;
  int64_t started, ended;
  uint32_t activeSeconds, pagesTurned;
  int startProgress = -1, endProgress = -1;
};
void queue(const std::string& bookPath, const Session& session);
void flush(const OpdsServer& server);
}  // namespace OpdsStatsSync
