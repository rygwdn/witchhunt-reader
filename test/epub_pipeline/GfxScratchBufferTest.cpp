#include <GfxRenderer.h>
#include <GfxScratchBuffer.h>
#include <gtest/gtest.h>

TEST(GfxScratchBuffer, BorrowOnlyReturnsFramebufferOnReleaseAndCanBeReused) {
  GfxRenderer renderer;
  GfxScratchBuffer scratch;

  uint8_t* first = scratch.acquire(renderer, 4096, GfxScratchBuffer::Policy::BorrowOnly);
  ASSERT_NE(first, nullptr);
  EXPECT_TRUE(scratch.isBorrowed());
  EXPECT_FALSE(scratch.isDedicated());
  EXPECT_EQ(scratch.capacity(), 52 * 1024u);
  EXPECT_EQ(scratch.acquire(renderer, 4096, GfxScratchBuffer::Policy::BorrowOnly), nullptr);

  scratch.release();
  EXPECT_FALSE(scratch.isAcquired());
  EXPECT_TRUE(renderer.hasSecondaryBuffer());
  EXPECT_EQ(renderer.secondaryReturnCount(), 1u);

  EXPECT_NE(scratch.acquire(renderer, 4096, GfxScratchBuffer::Policy::BorrowOnly), nullptr);
  scratch.reset();
  EXPECT_TRUE(renderer.hasSecondaryBuffer());
  EXPECT_EQ(renderer.secondaryReturnCount(), 2u);
}

TEST(GfxScratchBuffer, DoesNotReturnFramebufferTwiceAfterExternalRestore) {
  GfxRenderer renderer;
  GfxScratchBuffer scratch;
  ASSERT_NE(scratch.acquire(renderer, 4096, GfxScratchBuffer::Policy::BorrowOnly), nullptr);

  // Model a caller such as network-memory trimming restoring the loan first.
  renderer.returnSecondaryBuffer();
  ASSERT_TRUE(renderer.hasSecondaryBuffer());
  ASSERT_EQ(renderer.secondaryReturnCount(), 1u);

  scratch.release();
  EXPECT_EQ(renderer.secondaryReturnCount(), 1u);
}
