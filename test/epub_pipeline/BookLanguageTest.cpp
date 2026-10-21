#include <Arduino.h>
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

  // A one-chapter book whose only paragraph is `paragraph`. `tag` picks the directory inside
  // the work area, so a test can build the same book twice without the second build reading
  // the first one's cache.
  std::shared_ptr<Epub> bookWithText(const std::string& tag, const std::string& paragraph) {
    const fs::path dir = tag.empty() ? work : work / tag;
    fs::remove_all(dir);
    fs::create_directories(dir);
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
            "<body><p>" +
                paragraph + "</p></body></html>");
    const auto path = (dir / "book.epub").string();
    zip.write(path);
    auto epub = std::make_shared<Epub>(path, (dir / "cache").string());
    EXPECT_TRUE(epub->load(true));
    epub->setupCacheDir();
    return epub;
  }

  std::shared_ptr<Epub> book() {
    return bookWithText("",
                        "representation extraordinary representation extraordinary representation extraordinary");
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

  // Every word of every line, in reading order: the text the reader puts on the page. Two
  // layouts differ exactly when this differs, and the hyphen the layout inserts at a break is
  // part of the word it broke.
  static std::string pageText(Section& section) {
    std::string text;
    for (int p = 0; p < section.pageCount; ++p) {
      section.currentPage = p;
      const auto page = section.loadPageFromSectionFile();
      if (!page) continue;
      for (const auto& element : page->elements) {
        if (element->getTag() != TAG_PageLine) continue;
        const auto& block = *static_cast<const PageLine&>(*element).getBlock();
        for (uint16_t w = 0; w < block.wordCount(); ++w) {
          text += block.wordText(w);
          text += ' ';
        }
      }
      text += '\n';
    }
    return text;
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

// The hyphenation patterns belong to the book being built. They used to live in a global that
// every build overwrote, so two builds in different languages that interleaved (the foreground
// one and the background one) could lay out a page with the other build's patterns.
//
// The English and the German patterns break "nationalfeiertag" at different places and at no
// place in common, so the two layouts differ whenever the language reaches the layout, and a
// build that interleaves with another one must still produce the layout of its own language.
TEST_P(BookLanguageTest, InterleavedBuildsKeepTheirOwnLanguagePatterns) {
  // Long enough that the parse spans several slices: a book laid out in a single slice cannot
  // be interleaved with another build at all.
  std::string paragraph;
  for (int i = 0; i < 80; ++i) paragraph += "nationalfeiertag nationalfeiertag ";

  Section::BuildParams base;
  base.viewportWidth = 250;
  base.viewportHeight = 760;
  base.hyphenationEnabled = true;
  base.fontSizeNormalization = false;
  auto english = base;
  std::snprintf(english.languageOverride, sizeof(english.languageOverride), "en");
  auto german = base;
  std::snprintf(german.languageOverride, sizeof(german.languageOverride), "de");

  // The two languages, one after the other: the layouts must differ.
  const auto epub = bookWithText("alone", paragraph);
  Section englishAlone(epub, 0, renderer);
  ASSERT_TRUE(englishAlone.createSectionFile(english, {}, true));
  ASSERT_TRUE(englishAlone.loadSectionFile(english));
  Section germanAlone(epub, 0, renderer);
  ASSERT_TRUE(germanAlone.createSectionFile(german, {}, true));
  ASSERT_TRUE(germanAlone.loadSectionFile(german));
  const std::string englishText = pageText(englishAlone);
  const std::string germanText = pageText(germanAlone);
  EXPECT_NE(englishText, germanText) << "the language did not reach the layout";
  EXPECT_TRUE(hasHyphen(englishAlone));
  EXPECT_TRUE(hasHyphen(germanAlone));

  // The same two builds, interleaved slice by slice.
  const auto interleaved = bookWithText("interleaved", paragraph);
  Section englishLive(interleaved, 0, renderer);
  Section germanLive(interleaved, 0, renderer);
  {
    // Without a ticking clock the host's millis() is frozen, the budget never runs out and
    // every build finishes in one call: there would be nothing to interleave.
    const host_clock::Ticking tick(1);
    Section::BuildStep englishStep = Section::BuildStep::Setup;
    Section::BuildStep germanStep = Section::BuildStep::Setup;
    for (int i = 0; i < 20000; ++i) {
      if (englishStep != Section::BuildStep::Done && englishStep != Section::BuildStep::Failed) {
        englishStep = englishLive.stepSectionBuild(english, /*budgetMs=*/1);
      }
      if (germanStep != Section::BuildStep::Done && germanStep != Section::BuildStep::Failed) {
        germanStep = germanLive.stepSectionBuild(german, /*budgetMs=*/1);
      }
      if (englishStep == Section::BuildStep::Done && germanStep == Section::BuildStep::Done) break;
    }
    ASSERT_EQ(englishStep, Section::BuildStep::Done);
    ASSERT_EQ(germanStep, Section::BuildStep::Done);
  }
  ASSERT_TRUE(englishLive.loadSectionFile(english));
  ASSERT_TRUE(germanLive.loadSectionFile(german));
  EXPECT_EQ(pageText(englishLive), englishText) << "the interleaved English build took another build's patterns";
  EXPECT_EQ(pageText(germanLive), germanText) << "the interleaved German build took another build's patterns";
}

INSTANTIATE_TEST_SUITE_P(Metadata, BookLanguageTest,
                        testing::Values("", "<dc:language>English</dc:language>"));
}  // namespace
