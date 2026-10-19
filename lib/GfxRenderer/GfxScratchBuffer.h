#pragma once

#include <HeapPools.h>

#include <cstddef>
#include <cstdint>

class GfxRenderer;

// Caller-serialized scratch storage. Consumers must destroy all users of the region before
// release/reset. PSRAM backing survives release; a framebuffer loan never enters the heap.
class GfxScratchBuffer {
 public:
  enum class Policy { PreferDedicated, DedicatedOnly, BorrowOnly };
  static constexpr size_t DEFAULT_CAPACITY = 52 * 1024;

  GfxScratchBuffer() = default;
  ~GfxScratchBuffer();
  GfxScratchBuffer(const GfxScratchBuffer&) = delete;
  GfxScratchBuffer& operator=(const GfxScratchBuffer&) = delete;

  uint8_t* acquire(GfxRenderer& renderer, size_t bytes,
                   Policy policy = Policy::PreferDedicated);
  void release();
  void reset();
  uint8_t* data() const { return data_; }
  size_t capacity() const { return capacity_; }
  bool isAcquired() const { return data_ != nullptr; }
  bool hasDedicatedBacking() const {
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
    return storage_ != nullptr;
#else
    return false;
#endif
  }
  bool isBorrowed() const { return data_ != nullptr && borrowedRenderer_ != nullptr; }
  bool isDedicated() const { return data_ != nullptr && borrowedRenderer_ == nullptr; }

 private:
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  // One activity-lifetime allocation per consumer; tens of KiB cannot live on the task stack.
  PsramBytes storage_;
  size_t storageBytes_ = 0;
#endif
  uint8_t* data_ = nullptr;
  size_t capacity_ = 0;
  GfxRenderer* borrowedRenderer_ = nullptr;
};
