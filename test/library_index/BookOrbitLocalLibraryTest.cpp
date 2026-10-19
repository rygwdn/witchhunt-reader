#include <BookOrbitLocalLibrary.h>
#include <BuildArena.h>
#include <LibraryBuilder.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

class BookOrbitLocalLibraryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root = fs::temp_directory_path() / ("bookorbit_local_" +
        std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    fs::remove_all(root);
    fs::create_directories(root);
  }
  void TearDown() override { fs::remove_all(root); }
  std::string book(const char* relative, size_t size = 128) {
    auto path = root / relative;
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << std::string(size, 'x');
    return path.generic_string();
  }
  std::string index() const { return (root / ".crosspoint/library/library.bin").generic_string(); }
  void build() {
    LibraryBuilder::Config config;
    config.root = root.generic_string();
    config.workDir = (root / ".crosspoint/library").generic_string();
    config.indexPath = index();
    config.isBook = [](const char* name) { return std::string(name).ends_with(".epub"); };
    LibraryBuilder builder(std::move(config));
    BuildArena arena(48000);
    for (int i = 0; i < 100000 && !builder.finished(); ++i) builder.step(&arena);
    ASSERT_EQ(builder.phase(), LibraryBuilder::Phase::Done);
  }
  fs::path root;
};

TEST_F(BookOrbitLocalLibraryTest, OpensExistingDownloadBeforeIndexExists) {
  const auto path = book("downloads/book.epub");
  EXPECT_EQ(BookOrbitLocalLibrary::downloadedPath(path, 128, index().c_str()), path);
}

TEST_F(BookOrbitLocalLibraryTest, FindsMovedDownloadThroughCanonicalIndex) {
  const auto moved = book("COMPLETED/book.epub");
  build();
  const auto expected = (root / "downloads/book.epub").generic_string();
  EXPECT_EQ(BookOrbitLocalLibrary::downloadedPath(expected, 128, index().c_str()), moved);
}

TEST_F(BookOrbitLocalLibraryTest, RefusesDifferentSizeAndMissingIndexedFiles) {
  const auto path = book("COMPLETED/book.epub");
  build();
  const auto expected = (root / "downloads/book.epub").generic_string();
  EXPECT_TRUE(BookOrbitLocalLibrary::downloadedPath(expected, 127, index().c_str()).empty());
  fs::remove(path);
  EXPECT_TRUE(BookOrbitLocalLibrary::downloadedPath(expected, 128, index().c_str()).empty());
}

TEST_F(BookOrbitLocalLibraryTest, OldCatalogWithoutSizeCanFindMovedFilename) {
  const auto moved = book("COMPLETED/book.epub");
  build();
  EXPECT_EQ(BookOrbitLocalLibrary::downloadedPath((root / "downloads/book.epub").generic_string(),
                                                0, index().c_str()), moved);
}
TEST_F(BookOrbitLocalLibraryTest, BatchLookupPreservesDirectAndUniqueMovedMatches) {
  book("COMPLETED/book.epub");
  const auto direct = book("downloads/direct.epub");
  build();
  const std::vector<std::string> expected{
      (root / "downloads/book.epub").generic_string(),
      (root / "other/book.epub").generic_string(),
      direct,
      (root / "missing.epub").generic_string(),
  };
  std::vector<uint8_t> flags;
  BookOrbitLocalLibrary::downloadedFlags(expected, flags, index().c_str());
  EXPECT_EQ(flags, (std::vector<uint8_t>{1, 1, 1, 0}));
}

TEST_F(BookOrbitLocalLibraryTest, DoesNotChooseBetweenAmbiguousLocalCopies) {
  book("one/book.epub", 128);
  book("two/book.epub", 129);
  build();
  const auto expected = (root / "downloads/book.epub").generic_string();
  EXPECT_TRUE(BookOrbitLocalLibrary::downloadedPath(expected, 0, index().c_str()).empty());
  std::vector<uint8_t> flags;
  BookOrbitLocalLibrary::downloadedFlags(std::vector<std::string>{expected}, flags, index().c_str());
  EXPECT_EQ(flags, (std::vector<uint8_t>{0}));
  EXPECT_EQ(BookOrbitLocalLibrary::downloadedPath(expected, 128, index().c_str()),
            (root / "one/book.epub").generic_string());
}

TEST_F(BookOrbitLocalLibraryTest, EmptyAndCorruptIndexDoNotInventDownload) {
  EXPECT_TRUE(BookOrbitLocalLibrary::downloadedPath("", 128, index().c_str()).empty());
  const auto expected = (root / "downloads/book.epub").generic_string();
  book(".crosspoint/library/library.bin", 12);
  EXPECT_TRUE(BookOrbitLocalLibrary::downloadedPath(expected, 128, index().c_str()).empty());
}
}  // namespace
