#include <GfxRenderer.h>
#include <GfxScratchBuffer.h>

#include <utility>

GfxScratchBuffer::~GfxScratchBuffer() { reset(); }

uint8_t* GfxScratchBuffer::acquire(GfxRenderer& renderer, const size_t bytes, const Policy policy) {
  if (data_ != nullptr || bytes == 0) return nullptr;

#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  if (policy != Policy::BorrowOnly) {
    if (storage_ && storageBytes_ >= bytes) {
      data_ = storage_.get();
      capacity_ = storageBytes_;
      return data_;
    }
    PsramBytes replacement = psramAllocBytes(bytes);
    if (replacement) {
      storage_ = std::move(replacement);
      storageBytes_ = bytes;
      data_ = storage_.get();
      capacity_ = storageBytes_;
      return data_;
    }
  }
#endif

  if (policy == Policy::DedicatedOnly) return nullptr;
  size_t borrowedBytes = 0;
  uint8_t* borrowed = renderer.borrowSecondaryBuffer(&borrowedBytes);
  if (!borrowed) return nullptr;
  data_ = borrowed;
  capacity_ = borrowedBytes;
  borrowedRenderer_ = &renderer;
  return data_;
}

void GfxScratchBuffer::release() {
  // Network-memory trimming can return a borrowed block outside this lease. Do not return it twice.
  if (borrowedRenderer_ != nullptr && !borrowedRenderer_->hasSecondaryBuffer()) {
    borrowedRenderer_->returnSecondaryBuffer();
  }
  data_ = nullptr;
  capacity_ = 0;
  borrowedRenderer_ = nullptr;
}

void GfxScratchBuffer::reset() {
  release();
#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM
  storage_.reset();
  storageBytes_ = 0;
#endif
}
