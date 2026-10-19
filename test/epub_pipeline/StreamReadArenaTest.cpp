#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <Print.h>

#include "BuildArena.h"
#include "Epub.h"

#ifndef FIXTURE_EPUB
#error "FIXTURE_EPUB must be defined (path to test/fixtures/moby-dick.epub)"
#endif

namespace fs = std::filesystem;

namespace {

constexpr char kTestEntry[] = "OEBPS/3308270851576161417_2701-h-0.htm.html";

class VectorSink : public Print {
 public:
  size_t write(const uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, const size_t size) override {
    bytes.insert(bytes.end(), data, data + size);
    return size;
  }

  std::vector<uint8_t> bytes;
};

class StreamReadArenaTest : public testing::Test {
 protected:
  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_stream_arena_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
  }

  void TearDown() override { fs::remove_all(work); }

  bool stream(BuildArena* arena, VectorSink& sink) const {
    Epub epub(FIXTURE_EPUB, cacheDir);
    return epub.readItemContentsToStream(kTestEntry, sink, 1024, nullptr, arena);
  }

  fs::path work;
  std::string cacheDir;
};

TEST_F(StreamReadArenaTest, ExplicitArenaMatchesHeapAndReclaimsItsReaderScope) {
  VectorSink heapOutput;
  ASSERT_TRUE(stream(nullptr, heapOutput));
  ASSERT_GT(heapOutput.bytes.size(), 2048u);

  BuildArena arena(40 * 1024);
  ASSERT_TRUE(arena.valid());
  VectorSink arenaOutput;
  ASSERT_TRUE(stream(&arena, arenaOutput));

  EXPECT_EQ(arenaOutput.bytes, heapOutput.bytes);
  EXPECT_GT(arena.highWater(), 1024u) << "the read buffer and inflate ring should use the arena";
  EXPECT_EQ(arena.used(), 0u) << "the EntryReader scope must be reclaimed after streaming";
  EXPECT_EQ(arena.failedAllocSize(), 0u);
}

TEST_F(StreamReadArenaTest, TooSmallArenaFallsBackBeforeWritingAnyBytes) {
  VectorSink heapOutput;
  ASSERT_TRUE(stream(nullptr, heapOutput));
  ASSERT_GT(heapOutput.bytes.size(), 2048u) << "the entry must require more than the tiny arena";

  BuildArena arena(2 * 1024);
  ASSERT_TRUE(arena.valid());
  VectorSink arenaOutput;
  ASSERT_TRUE(stream(&arena, arenaOutput));

  EXPECT_EQ(arenaOutput.bytes, heapOutput.bytes);
  EXPECT_GT(arena.failedAllocSize(), 0u) << "the external reader should refuse its ring before heap retry";
  EXPECT_EQ(arena.used(), 0u) << "a refused reader must release its partial arena allocations";
}

}  // namespace
