#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "Epub.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace {
namespace fs = std::filesystem;

class BookLanguageTest : public testing::TestWithParam<const char*> {
 protected:
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("book_language_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
  }
  void TearDown() override { fs::remove_all(work); }

  std::shared_ptr<Epub> book() {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<container xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\" version=\"1.0\">"
            "<rootfiles><rootfile full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/>"
            "</rootfiles></container>");
    zip.add("content.opf",
            std::string("<package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
                        "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Language</dc:title>") +
                GetParam() + "</metadata><manifest><item id=\"c\" href=\"chapter.xhtml\" "
                             "media-type=\"application/xhtml+xml\"/></manifest>"
                             "<spine><itemref idref=\"c\"/></spine></package>");
    zip.add("chapter.xhtml",
            "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><title>Language</title></head>"
            "<body><p>representation extraordinary representation extraordinary representation extraordinary</p>"
            "</body></html>");
    const auto path = (work / "book.epub").string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (work / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  static bool hasHyphen(Section& section) {
    for (int p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      EXPECT_TRUE(page);
      if (!page) continue;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto& block = *static_cast<const PageLine&>(*element).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          if (std::string(block.wordText(w)).ends_with('-')) return true;
        }
      }
    }
    return false;
  }
};

TEST_P(BookLanguageTest, EnglishOverrideEnablesHyphenationWithoutReusingMetadataLayout) {
  const auto epub = book();
  const auto metadataLanguage = epub->getLanguage();
  Section::BuildParams metadata;
  metadata.viewportWidth = 250;
  metadata.viewportHeight = 760;
  metadata.hyphenationEnabled = true;
  metadata.fontSizeNormalization = false;
  Section original(epub, 0, renderer);
  ASSERT_TRUE(original.createSectionFile(metadata, {}, true));
  ASSERT_TRUE(original.loadSectionFile(metadata));
  EXPECT_FALSE(hasHyphen(original));

  auto english = metadata;
  std::snprintf(english.languageOverride, sizeof(english.languageOverride), "en");
  Section corrected(epub, 0, renderer);
  EXPECT_FALSE(corrected.loadSectionFile(english));
  ASSERT_TRUE(corrected.createSectionFile(english, {}, true));
  ASSERT_TRUE(corrected.loadSectionFile(english));
  EXPECT_TRUE(hasHyphen(corrected));
  EXPECT_EQ(epub->getLanguage(), metadataLanguage);

  // Clearing the override restores the original layout, including from its warm cache.
  Section restored(epub, 0, renderer);
  ASSERT_TRUE(restored.loadSectionFile(metadata));
  EXPECT_FALSE(hasHyphen(restored));
  Section reopened(epub, 0, renderer);
  ASSERT_TRUE(reopened.loadSectionFile(english));
  EXPECT_TRUE(hasHyphen(reopened));
}

INSTANTIATE_TEST_SUITE_P(Metadata, BookLanguageTest,
                        testing::Values("", "<dc:language>English</dc:language>"));
}  // namespace
