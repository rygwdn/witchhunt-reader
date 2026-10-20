// Page::serialize against the smashed vector metadata that faulted on device (crash 2026-10-19).
//
// During a Background-B section build of spine 36, the build Page's footnotes vector metadata was
// overwritten on the heap: its begin pointer read as 8 while finish stood past it, so the
// serializer's first file.write(fn.number, 32) reached the ROM memcpy with src=0x8 and the device
// took a LoadProhibited (PC 0x40056F17, __memcpy_aux). serialize() now refuses a page whose
// element or footnote vector begins implausibly low (no real allocation lives below 4 KiB on the
// host or the ESP32 pools): onPageComplete turns the false return into a clean, logged build
// failure and the page is rebuilt.
//
// What has to hold:
//   1. A footnote vector forged to the crash state makes serialize() return false, without
//      faulting and without writing the page.
//   2. The same holds for a smashed element vector.
//   3. An honest page still serializes (the guard never fires on real metadata).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "Epub/Page.h"
#include "HalStorage.h"

namespace {

// Forges a std::vector's three pointers to the crash state: begin=8, finish=capacity=0x88
// (FootnoteEntry is 128 B, so finish-begin reads as one element). Returns the previous pointers
// so the caller can restore them before the vector destructs.
template <typename Vec>
void smashVector(Vec& vector, void* (&saved)[3]) {
  static_assert(sizeof(vector) == 3 * sizeof(void*), "the test assumes libstdc++ vector layout");
  void* forged[3] = {reinterpret_cast<void*>(0x8), reinterpret_cast<void*>(0x88),
                     reinterpret_cast<void*>(0x88)};
  auto* meta = reinterpret_cast<void**>(&vector);
  std::memcpy(saved, meta, sizeof saved);
  std::memcpy(meta, forged, sizeof forged);
}

template <typename Vec>
void restoreVector(Vec& vector, void* const (&saved)[3]) {
  auto* meta = reinterpret_cast<void**>(&vector);
  std::memcpy(meta, saved, sizeof saved);
}

struct PageSerializeGuardTest : testing::Test {
  std::filesystem::path path;

  void SetUp() override {
    path = std::filesystem::temp_directory_path() / "page_serialize_guard.bin";
    std::filesystem::remove(path);
  }
  void TearDown() override { std::filesystem::remove(path); }

  FsFile openForWrite() {
    FsFile file;
    EXPECT_TRUE(file.openForWrite(path.string()));
    return file;
  }
};

TEST_F(PageSerializeGuardTest, RefusesSmashedFootnoteMetadata) {
  auto* page = new Page();  // owned by this test; metadata is restored before destruction
  FootnoteEntry fn{};
  std::strcpy(fn.number, "12");
  std::strcpy(fn.href, "#note-12");
  page->footnotes.push_back(fn);

  void* saved[3];
  smashVector(page->footnotes, saved);
  {
    FsFile file = openForWrite();
    EXPECT_FALSE(page->serialize(file));
    file.close();
  }
  restoreVector(page->footnotes, saved);
  delete page;
}

TEST_F(PageSerializeGuardTest, RefusesSmashedElementMetadata) {
  auto* page = new Page();
  page->elements.push_back(nullptr);  // never dereferenced: the guard fires on the begin pointer

  void* saved[3];
  smashVector(page->elements, saved);
  {
    FsFile file = openForWrite();
    EXPECT_FALSE(page->serialize(file));
    file.close();
  }
  restoreVector(page->elements, saved);
  delete page;
}

TEST_F(PageSerializeGuardTest, HonestPageStillSerializes) {
  Page page;
  FootnoteEntry fn{};
  std::strcpy(fn.number, "12");
  std::strcpy(fn.href, "#note-12");
  page.footnotes.push_back(fn);

  FsFile file = openForWrite();
  EXPECT_TRUE(page.serialize(file));
  file.close();
}

}  // namespace