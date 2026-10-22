#include "RamFontStore.h"

#include <Memory.h>

#include <cstring>

bool RamFontStore::begin(const uint32_t bytes) {
  end();
  if (bytes == 0) return false;
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  buffer_ = psramAllocBytes(bytes);
  if (!buffer_) return false;
  std::memset(buffer_.get(), 0xFF, bytes);  // a blank chip
  device_ = makeUniqueNoThrow<RamFlash>(buffer_.get(), bytes);
  if (!device_) {
    buffer_.reset();
    return false;
  }
  bytes_ = bytes;
  return true;
#else
  return false;  // no PSRAM: the book's families go in the font partition
#endif
}

void RamFontStore::end() {
  device_.reset();  // the device reads the buffer: it goes first
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  buffer_.reset();
#endif
  bytes_ = 0;
}
