#pragma once

#include <WString.h>

#include <cstdint>
#include <cstdio>
#include <string>

// The native bookmark smoke checks that the production code consistently constructs and carries
// keys through exchange/ack. Digest bytes are not the subject of this harness; firmware builds use
// Arduino's MD5Builder.
class MD5Builder {
 public:
  void begin() { material_.clear(); }
  void add(const char* value) {
    if (value != nullptr) material_.append(value);
  }
  void calculate() {
    uint32_t state[4] = {0x811C9DC5u, 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u};
    for (const unsigned char byte : material_) {
      for (size_t i = 0; i < 4; ++i) {
        state[i] ^= static_cast<uint32_t>(byte) + static_cast<uint32_t>(i * 0x51u);
        state[i] *= 0x01000193u + static_cast<uint32_t>(i * 0x1021u);
        state[i] = (state[i] << (5u + static_cast<uint32_t>(i))) | (state[i] >> (27u - static_cast<uint32_t>(i)));
      }
    }
    char hex[33];
    std::snprintf(hex, sizeof(hex), "%08x%08x%08x%08x", state[0], state[1], state[2], state[3]);
    digest_ = hex;
  }
  String toString() const { return String(digest_.c_str()); }

 private:
  std::string material_;
  std::string digest_;
};
