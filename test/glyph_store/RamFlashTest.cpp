// A NOR flash in a plain RAM buffer: the chip's rules, and the glyph store run over it.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "GlyphStoreTestUtil.h"
#include "RamFlash.h"

using namespace glyphstore;
using namespace gst_test;

namespace {

class RamFlashDevice : public ::testing::Test {
 protected:
  std::vector<uint8_t> buffer = std::vector<uint8_t>(kFlashBytes, 0xFF);  // a blank chip
  RamFlash flash{buffer.data(), kFlashBytes};
};

}  // namespace

TEST_F(RamFlashDevice, ABlankBufferReadsAsErased) {
  uint8_t bytes[16] = {};
  ASSERT_TRUE(flash.read(0, bytes, sizeof bytes));
  for (const uint8_t byte : bytes) EXPECT_EQ(0xFF, byte);
  ASSERT_TRUE(flash.read(kFlashBytes - 4, bytes, 4));
  for (const uint8_t byte : bytes) EXPECT_EQ(0xFF, byte);
}

TEST_F(RamFlashDevice, ProgrammedBytesReadBack) {
  const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
  ASSERT_TRUE(flash.program(100, data, sizeof data));
  uint8_t read[5] = {};
  ASSERT_TRUE(flash.read(100, read, sizeof read));
  EXPECT_EQ(0, std::memcmp(data, read, sizeof data));
}

TEST_F(RamFlashDevice, AProgramCannotSetABitBackToOne) {
  const uint8_t first = 0xF0;
  const uint8_t second = 0x0F;
  ASSERT_TRUE(flash.program(0, &first, 1));
  ASSERT_TRUE(flash.program(0, &second, 1));
  uint8_t read = 0;
  ASSERT_TRUE(flash.read(0, &read, 1));
  // The high nibble's 1 bits go to 0, as a program may; the low nibble's stay 0: they cannot be set again.
  EXPECT_EQ(0x00, read);
  EXPECT_EQ(1u, flash.violations());
}

TEST_F(RamFlashDevice, AnEraseFillsWithOnes) {
  const uint8_t data[] = {0x00, 0x00, 0x00, 0x00};
  ASSERT_TRUE(flash.program(0, data, sizeof data));
  ASSERT_TRUE(flash.erase(0, kSectorBytes));
  uint8_t read[4] = {};
  ASSERT_TRUE(flash.read(0, read, sizeof read));
  for (const uint8_t byte : read) EXPECT_EQ(0xFF, byte);
  EXPECT_EQ(1u, flash.eraseCalls());
}

TEST_F(RamFlashDevice, AnEraseNeedsWholeSectors) {
  EXPECT_FALSE(flash.erase(1, kSectorBytes));
  EXPECT_FALSE(flash.erase(0, kSectorBytes - 1));
  EXPECT_FALSE(flash.erase(0, 100));
  EXPECT_TRUE(flash.erase(0, kSectorBytes));
  EXPECT_EQ(1u, flash.eraseCalls());
}

TEST_F(RamFlashDevice, AMappingIsTheBuffersOwnBytes) {
  const uint8_t* mapped = flash.map(1000, 64);
  ASSERT_EQ(buffer.data() + 1000, mapped);
  EXPECT_EQ(RamFlash::kMapPages, flash.freeMapPages());
  flash.unmap(mapped);
  EXPECT_EQ(RamFlash::kMapPages, flash.freeMapPages());  // a RAM mapping costs no page
  EXPECT_EQ(nullptr, flash.map(0, 0));
  EXPECT_EQ(nullptr, flash.map(kFlashBytes, 1));
}

TEST_F(RamFlashDevice, WhatIsOutsideTheBufferFails) {
  uint8_t byte = 0;
  EXPECT_FALSE(flash.read(kFlashBytes, &byte, 1));
  EXPECT_FALSE(flash.read(kFlashBytes - 2, &byte, 4));
  EXPECT_FALSE(flash.program(kFlashBytes, &byte, 1));
  EXPECT_FALSE(flash.erase(kFlashBytes, kSectorBytes));
}

TEST_F(RamFlashDevice, TheGlyphStoreRunsOverIt) {
  FlashGlyphStore store{flash};
  const Layout layout = smallLayout();
  ASSERT_TRUE(installAndOpen(store, layout));

  alignas(4) uint8_t buffer[16384] = {};
  GlyphBatch batch{buffer, sizeof buffer};
  const uint32_t key = makeKey('A', 0, 0);
  ASSERT_TRUE(stage(batch, key, 9, 12));
  ASSERT_EQ(Commit::Stored, store.commit(batch));
  EXPECT_TRUE(storedIntact(store, key, 9, 12));

  // The store is closed and opened again, as a book switch does: the glyphs are still there.
  store.close();
  ASSERT_EQ(OpenResult::Ready, store.open(kIdentityA));
  ASSERT_TRUE(claimTestSizes(store));
  EXPECT_TRUE(storedIntact(store, key, 9, 12));
  EXPECT_EQ(0u, flash.violations());
}
