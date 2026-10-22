#pragma once

#include <cstdint>

#include "NorFlash.h"

/// NOR flash in a plain RAM buffer: the glyph store's device when the store's data is worth keeping out of the
/// chip (a book's own families and their glyphs, in PSRAM).
///
/// The chip's rules stand, so the store's write-once rules run over it unchanged: bits program from 1 to 0 only,
/// and only an erase, in whole 4 KB sectors, sets them back to 1. A program that asks for a 0 -> 1 change leaves
/// the bit at 0, as on the chip, and is counted in violations().
///
/// The buffer is the caller's and outlives the device: on the device it is PSRAM, and it is freed with the store.
class RamFlash final : public NorFlash {
 public:
  /// A mapping costs no MMU page here, so the store's planner is bounded by the buffer's size alone.
  static constexpr uint32_t kMapPages = 256;

  /// `buffer` is the caller's, `bytes` its size. The buffer must be erased (0xFF) for the store to install into
  /// it as it would into a blank chip.
  RamFlash(uint8_t* buffer, const uint32_t bytes, const uint32_t freePages = kMapPages)
      : buffer_(buffer), bytes_(bytes), freePages_(freePages) {}

  uint32_t size() const override { return bytes_; }

  bool read(const uint32_t offset, void* dst, const uint32_t len) const override;

  bool program(const uint32_t offset, const void* src, const uint32_t len) override;

  bool erase(const uint32_t offset, const uint32_t len) override;

  /// The buffer's own bytes: nothing is mapped, so this succeeds wherever the range does.
  const uint8_t* map(const uint32_t offset, const uint32_t len) override;

  /// Nothing to release: the mapping was the buffer's own bytes.
  void unmap(const uint8_t* /*mapped*/) override {}

  uint32_t freeMapPages() const override { return freePages_; }

  /// Programs that asked for a bit to go 0 -> 1: the chip cannot, so the bit stays 0. The store never does it.
  uint32_t violations() const { return violations_; }
  /// Erases run: a RAM store wears nothing, but the store's own log counts them.
  uint32_t eraseCalls() const { return eraseCalls_; }

 private:
  bool inRange(const uint32_t offset, const uint32_t len) const { return offset <= bytes_ && len <= bytes_ - offset; }

  uint8_t* buffer_;
  uint32_t bytes_;
  uint32_t freePages_;
  uint32_t violations_ = 0;
  uint32_t eraseCalls_ = 0;
};
