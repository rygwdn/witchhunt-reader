#pragma once

#include <cstdint>
#include <memory>

#include "RamFlash.h"

#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
#include <HeapPools.h>
#endif

/// The store for a book's own families: a buffer in RAM and the glyph store's device over it.
///
/// The buffer is in PSRAM on a board that has it, and the store is not made at all where there is none: a book's
/// families then go in the font partition, as they always did. The buffer is big, cold and rebuildable, and it is
/// freed with the store: none of it survives the book.
class RamFontStore {
 public:
  RamFontStore() = default;
  RamFontStore(const RamFontStore&) = delete;
  RamFontStore& operator=(const RamFontStore&) = delete;

  /// Makes the store: `bytes` of PSRAM, erased as a blank chip, and the device over it. False where there is no
  /// PSRAM, or where it cannot serve the buffer: the caller falls back to the font partition.
  bool begin(const uint32_t bytes);

  /// Whether the board has the memory for the store at all: a board without PSRAM never has it, and the book's
  /// families go in the font partition there, as they always did.
  static bool available() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    return true;
#else
    return false;
#endif
  }

  /// Frees the buffer, and the device with it.
  void end();

  /// The device the store runs on; nullptr where there is no store.
  RamFlash* device() { return device_.get(); }
  bool valid() const { return device_ != nullptr; }
  uint32_t bytes() const { return bytes_; }

 private:
  std::unique_ptr<RamFlash> device_;
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  PsramBytes buffer_;  ///< the raw external block; the device reads it in place
#endif
  uint32_t bytes_ = 0;
};
