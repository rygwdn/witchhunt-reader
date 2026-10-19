#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>

#include <Memory.h>

#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
#include <HeapPools.h>
#endif

/// A byte block for a buffer that is cold and rebuildable: in PSRAM on a board that has it, in the
/// ordinary heap everywhere else.
///
/// "Cold" is the point: the buffer is touched only when something is missing (a glyph the store has
/// not rendered yet) or when it is rebuilt, so its latency shows on no hot path. "Rebuildable" is
/// why a full PSRAM may fall back to the ordinary heap: the buffer can be made again, and a font
/// that does not load is worse than one that loads from the pool it was meant to spare. On a board
/// with PSRAM the ordinary heap is the internal pool (P0.3), so the fallback spends exactly what
/// the external placement was to save; on the C3 it is the only pool there is, and nothing moves.
///
/// The block is aligned to `alignment` (a power of two): its callers cast parts of it to wider
/// types (the rasterizer's cell pool and its point arrays).
class ColdBuffer {
 public:
  ColdBuffer() = default;
  ColdBuffer(const ColdBuffer&) = delete;
  ColdBuffer& operator=(const ColdBuffer&) = delete;
  ~ColdBuffer() { reset(); }

  /// Allocates `bytes` of zeroed memory, aligned to `alignment`. False when neither pool could.
  bool allocate(const size_t bytes, const size_t alignment = 8) {
    reset();
    if (bytes == 0 || alignment == 0) return false;
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    // The external block first: the internal pool is the scarce one. psramAllocBytes has no internal
    // fallback of its own, so a PSRAM that cannot serve this falls through to the ordinary heap.
    psram_ = psramAllocBytes(bytes + alignment);
    if (psram_) {
      const auto raw = reinterpret_cast<uintptr_t>(psram_.get());
      const auto aligned = (raw + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
      ptr_ = reinterpret_cast<uint8_t*>(aligned);
      size_ = bytes;
      external_ = true;
      std::memset(ptr_, 0, bytes);
      return true;
    }
#endif
    heap_ = makeUniqueNoThrow<uint8_t[]>(bytes);
    if (!heap_) return false;
    ptr_ = heap_.get();
    size_ = bytes;
    external_ = false;
    return true;
  }

  /// Frees the block. Nothing points at it any more.
  void reset() {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    psram_.reset();
#endif
    heap_.reset();
    ptr_ = nullptr;
    size_ = 0;
    external_ = false;
  }

  uint8_t* data() const { return ptr_; }
  bool valid() const { return ptr_ != nullptr; }
  size_t size() const { return size_; }
  /// True when the block is in PSRAM; false for the ordinary heap, which is the internal pool on a
  /// board that has PSRAM.
  bool external() const { return external_; }

 private:
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  PsramBytes psram_;  ///< the raw external block; the aligned pointer is inside it
#endif
  std::unique_ptr<uint8_t[]> heap_;
  uint8_t* ptr_ = nullptr;
  size_t size_ = 0;
  bool external_ = false;
};
