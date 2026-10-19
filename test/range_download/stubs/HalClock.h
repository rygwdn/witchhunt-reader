#pragma once
#include <string>
struct HalClock {
  static bool ensureUsableForTls(const std::string&) { return true; }
};
