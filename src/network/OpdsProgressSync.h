#pragma once
#include <cstdint>
#include <string>

#include "OpdsServerStore.h"

namespace OpdsProgressSync {
struct Remote {
  float progression = 0;
  std::string modified, deviceName, reference;
};
bool associated(const std::string& bookPath);
bool copyAssociation(const std::string& from, const std::string& to);
bool fetch(const std::string& bookPath, Remote& remote, int* status = nullptr);
bool flushForBook(const std::string& bookPath);
std::string deviceId();
bool associate(const std::string& bookPath, const OpdsServer& server, const std::string& progressionUrl);
int64_t nowMilliseconds();
void acceptRemote(const std::string& bookPath);
bool queue(const std::string& bookPath, float progression, const std::string& reference, int64_t readAt);
// Retains failed uploads; stale positions are quarantined for manual reconciliation.
bool flush(const OpdsServer& server, bool* failed = nullptr);
}  // namespace OpdsProgressSync
