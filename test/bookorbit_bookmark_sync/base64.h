#pragma once

#include <string>

namespace base64 {
struct Encoded {
  std::string value;
  const char* c_str() const { return value.c_str(); }
};

inline Encoded encode(const char*) { return {"unused-in-native-bookmark-tests"}; }
}  // namespace base64
