#pragma once

#include <ctime>
#include <cstdint>

namespace HalClock {
bool ensureUsableForTls(const char* preferredServer = nullptr);
bool isSynced();
bool isPlausibleForTls();
time_t now();
bool isColdBoot();
uint64_t retainedMonotonicUs();
}  // namespace HalClock
