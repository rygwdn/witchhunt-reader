// Cover for CrossPointSettings::enforceMinimumFontSize (SettingsList STR_ENFORCE_MIN_FONT_SIZE):
// when on, no *main-text* block or word may render smaller than the body/reading size.
// Superscript/subscript are deliberately exempt (footnote markers, ordinals, chemical
// formulas are expected to render smaller than the surrounding text) and must still shrink.
// The dump format exposes both channels directly — `mult=` is a TextBlock's block-level
// fontSizeMultiplier and each `W` record's `s=`/`z=` are a word's EpdFontFamily::Style bits
// (SUP=16, SUB=32) and per-word size percent (WordSizePropagation/WordSizeFolding cover the
// channels themselves; this test pins the floor applied on top of them, in
// ChapterHtmlSlimParser's resolveBlockFont and updateEffectiveInlineStyle).
//
// test_font_sizes.epub is built for exactly this: stylesheet percent classes (55%-95%), em/rem
// spans, a nested 1.5em/0.5em composition, out-of-range clamping, and a block-level
// `.footnote { font-size: 80% }` class, so the "off" run is guaranteed to contain sub-100% mult=
// and z= values to floor.
#include <gtest/gtest.h>

#include <filesystem>
#include <regex>
#include <sstream>
#include <string>

#include "PipelineRunner.h"

namespace fs = std::filesystem;

namespace {

const std::string kFixture = std::string(CORPUS_DIR) + "/test_font_sizes.epub";

std::string freshCacheDir(const std::string& tag) {
  const auto dir = fs::temp_directory_path() / "minimum_font_size_test" / tag;
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir.string();
}

std::string dumpWith(bool enforceMinimumFontSize, const std::string& tag) {
  pipeline_harness::Profile profile;
  profile.enforceMinimumFontSize = enforceMinimumFontSize;
  std::ostringstream out;
  const bool ok = pipeline_harness::runAndDump(kFixture, freshCacheDir(tag), profile, out);
  EXPECT_TRUE(ok) << "pipeline failed for " << kFixture << " (enforceMinimumFontSize=" << enforceMinimumFontSize
                  << ")\n"
                  << out.str();
  return out.str();
}

// Smallest mult= value found in the dump (1.0 if the dump has no LINE records at all).
float minBlockMultiplier(const std::string& dump) {
  static const std::regex re(R"(mult=(-?\d+\.\d+))");
  float minVal = 1.0f;
  for (auto it = std::sregex_iterator(dump.begin(), dump.end(), re); it != std::sregex_iterator(); ++it) {
    minVal = std::min(minVal, std::stof((*it)[1].str()));
  }
  return minVal;
}

constexpr int kSupBit = 16;  // EpdFontFamily::Style::SUP
constexpr int kSubBit = 32;  // EpdFontFamily::Style::SUB

// Smallest z= value found among non-sup/sub W records (100 if there are none). Main-text
// words are what the floor governs; sup/sub float free of it by design.
int minMainTextWordSizePct(const std::string& dump) {
  static const std::regex re(R"(s=(\d+) z=(\d+))");
  int minVal = 100;
  for (auto it = std::sregex_iterator(dump.begin(), dump.end(), re); it != std::sregex_iterator(); ++it) {
    const int style = std::stoi((*it)[1].str());
    if ((style & (kSupBit | kSubBit)) != 0) continue;
    minVal = std::min(minVal, std::stoi((*it)[2].str()));
  }
  return minVal;
}

// Smallest z= value found among sup/sub W records (100 if there are none).
int minSupSubWordSizePct(const std::string& dump) {
  static const std::regex re(R"(s=(\d+) z=(\d+))");
  int minVal = 100;
  for (auto it = std::sregex_iterator(dump.begin(), dump.end(), re); it != std::sregex_iterator(); ++it) {
    const int style = std::stoi((*it)[1].str());
    if ((style & (kSupBit | kSubBit)) == 0) continue;
    minVal = std::min(minVal, std::stoi((*it)[2].str()));
  }
  return minVal;
}

}  // namespace

TEST(MinimumFontSizeTest, DisabledFixtureActuallyShrinksBelowBody) {
  // Sanity check on the fixture itself: without the floor, test_font_sizes.epub must produce
  // sub-100% sizes on all three channels, or the "enabled" assertions below would pass
  // vacuously.
  const std::string dump = dumpWith(/*enforceMinimumFontSize=*/false, "off");
  EXPECT_LT(minBlockMultiplier(dump), 1.0f) << "fixture has no sub-body block (.footnote) to floor";
  EXPECT_LT(minMainTextWordSizePct(dump), 100) << "fixture has no sub-body main-text word (.tale1..5/.em070) to floor";
  EXPECT_LT(minSupSubWordSizePct(dump), 100) << "fixture has no sub-body sup/sub word (.sup) to exercise the exemption";
}

TEST(MinimumFontSizeTest, EnabledFloorsEveryMainTextBlockAndWordAtBody) {
  const std::string dump = dumpWith(/*enforceMinimumFontSize=*/true, "on");
  EXPECT_GE(minBlockMultiplier(dump), 1.0f) << "a block rendered smaller than the body font";
  EXPECT_GE(minMainTextWordSizePct(dump), 100) << "a main-text word rendered smaller than the body font";
}

// Sup/sub are exempt: footnote markers, ordinals, and chemical formulas are expected to
// render smaller than the surrounding text, and flooring them to body size would make them
// indistinguishable from normal text.
TEST(MinimumFontSizeTest, EnabledLeavesSupSubShrunk) {
  const std::string off = dumpWith(/*enforceMinimumFontSize=*/false, "off_supsub");
  const std::string on = dumpWith(/*enforceMinimumFontSize=*/true, "on_supsub");
  EXPECT_LT(minSupSubWordSizePct(on), 100) << "the minimum-size floor flattened sup/sub text";
  EXPECT_EQ(minSupSubWordSizePct(on), minSupSubWordSizePct(off)) << "the floor changed a sup/sub word's size";
}

// Upscaling (headings, the 1.2em/1.6em/huge400 classes) must survive untouched — the floor
// only clamps the down side.
TEST(MinimumFontSizeTest, EnabledLeavesUpscalingIntact) {
  const std::string off = dumpWith(/*enforceMinimumFontSize=*/false, "off_up");
  const std::string on = dumpWith(/*enforceMinimumFontSize=*/true, "on_up");

  static const std::regex re(R"(mult=(-?\d+\.\d+))");
  float maxOff = 1.0f;
  for (auto it = std::sregex_iterator(off.begin(), off.end(), re); it != std::sregex_iterator(); ++it) {
    maxOff = std::max(maxOff, std::stof((*it)[1].str()));
  }
  float maxOn = 1.0f;
  for (auto it = std::sregex_iterator(on.begin(), on.end(), re); it != std::sregex_iterator(); ++it) {
    maxOn = std::max(maxOn, std::stof((*it)[1].str()));
  }
  EXPECT_GT(maxOff, 1.0f) << "fixture has no upscaled block (.opener/headings) to compare";
  EXPECT_FLOAT_EQ(maxOn, maxOff) << "the minimum-size floor altered an upscaled block";
}
