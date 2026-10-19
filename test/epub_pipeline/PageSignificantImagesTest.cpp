// Cover for Page::hasSignificantImages(), the coverage gate that decides whether an image page
// is worth the ghost-prevention half-refresh scheduled for the page after it
// (EpubReaderActivity.cpp's pendingHalfRefreshAfterImagePage). A small inline icon/dingbat/
// drop-cap should NOT trigger it — only images that cover a meaningful fraction of the
// viewport should. Page::hasImages() stays an any-size predicate for its other consumers
// (pre-render exclusion, placeholder checks); this is the size-aware layer on top of it.
#include <gtest/gtest.h>

#include <memory>
#include <utility>

#include "Epub/Page.h"
#include "Epub/blocks/ImageBlock.h"

namespace {

constexpr int16_t kViewportW = 480;
constexpr int16_t kViewportH = 800;  // area 384000

std::unique_ptr<PageImage> makeImage(int16_t x, int16_t y, int16_t w, int16_t h) {
  auto block = std::make_unique<ImageBlock>("nonexistent.png", w, h);
  return std::make_unique<PageImage>(std::move(block), x, y);
}

}  // namespace

TEST(PageSignificantImagesTest, NoImagesIsNotSignificant) {
  Page page;
  EXPECT_FALSE(page.hasSignificantImages(kViewportW, kViewportH));
}

TEST(PageSignificantImagesTest, TinyInlineIconIsNotSignificant) {
  Page page;
  // 24x24 dingbat: 576 px, well under 8% of 384000.
  page.elements.push_back(makeImage(10, 10, 24, 24));
  EXPECT_TRUE(page.hasImages());
  EXPECT_FALSE(page.hasSignificantImages(kViewportW, kViewportH));
}

TEST(PageSignificantImagesTest, FullBleedIllustrationIsSignificant) {
  Page page;
  // Full-width illustration covering most of the viewport height.
  page.elements.push_back(makeImage(0, 0, kViewportW, 400));
  EXPECT_TRUE(page.hasSignificantImages(kViewportW, kViewportH));
}

TEST(PageSignificantImagesTest, ManySmallIconsBelowThresholdStayInsignificant) {
  Page page;
  // Several small icons scattered across the page: none individually significant, and their
  // summed area is still under threshold — confirms this sums area rather than using the
  // bounding-box union (which could overestimate for scattered images).
  for (int i = 0; i < 5; ++i) {
    page.elements.push_back(makeImage(static_cast<int16_t>(i * 80), static_cast<int16_t>(i * 100), 20, 20));
  }
  EXPECT_FALSE(page.hasSignificantImages(kViewportW, kViewportH));
}

TEST(PageSignificantImagesTest, ZeroViewportFallsBackToHasImages) {
  Page page;
  page.elements.push_back(makeImage(0, 0, 10, 10));
  EXPECT_TRUE(page.hasSignificantImages(0, 0));
}
