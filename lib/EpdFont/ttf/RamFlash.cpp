#include "RamFlash.h"

#include <cstring>

bool RamFlash::read(const uint32_t offset, void* dst, const uint32_t len) const {
  if (!buffer_ || !inRange(offset, len)) return false;
  std::memcpy(dst, buffer_ + offset, len);
  return true;
}

bool RamFlash::program(const uint32_t offset, const void* src, const uint32_t len) {
  if (!buffer_ || !inRange(offset, len)) return false;
  const auto* bytes = static_cast<const uint8_t*>(src);
  for (uint32_t i = 0; i < len; ++i) {
    uint8_t& cell = buffer_[offset + i];
    if ((cell & bytes[i]) != bytes[i]) ++violations_;  // a bit asked to go 0 -> 1: the chip cannot
    cell &= bytes[i];
  }
  return true;
}

bool RamFlash::erase(const uint32_t offset, const uint32_t len) {
  if (!buffer_ || !inRange(offset, len) || offset % kSectorBytes != 0 || len % kSectorBytes != 0) return false;
  ++eraseCalls_;
  std::memset(buffer_ + offset, 0xFF, len);
  return true;
}

const uint8_t* RamFlash::map(const uint32_t offset, const uint32_t len) {
  if (!buffer_ || len == 0 || !inRange(offset, len)) return nullptr;
  return buffer_ + offset;
}
