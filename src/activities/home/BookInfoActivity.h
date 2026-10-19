#pragma once

#include <cstdint>
#include <array>
#include <ctime>
#include <string>
#include <vector>

#include "../Activity.h"
#include "ReadingStats.h"
#include "components/SeqPublish.h"

class BookInfoActivity final : public Activity {
  const std::string filePath;

  // Metadata populated in onEnter
  std::string title;
  std::string author;
  std::string series;
  std::string seriesIndex;
  std::string description;
  std::string coverBmpPath;
  std::string loadError;
  bool loadSucceeded = false;
  size_t fileSizeBytes = 0;

  // Reading stats snapshot, populated in loadData() when this book has history.
  bool hasReadingStats = false;
  uint32_t statTotalSeconds = 0;
  uint8_t statProgress = 0;
  time_t statLastReadEpoch = 0;

  // Description paging (populated lazily on first render)
  std::vector<std::string> descLines;
  int descWrappedWidth = 0;
  int descPage = 0;
  int descLinesPerPage = 0;
  int descTotalPages = 0;

  // Review data stays on SD; each render window is at most 512 bytes and the
  // wrapped-line vector is bounded to that one window, never the full note.
  static constexpr size_t REVIEW_WINDOW_BYTES = 512;
  static constexpr size_t REVIEW_MAX_WINDOWS = 80;
  std::string reviewHash;
  uint32_t reviewLength = 0;
  bool hasReview = false;
  bool showingReview = false;
  std::array<char, REVIEW_WINDOW_BYTES + 1> reviewBuffer{};
  std::vector<std::string> reviewLines;
  std::array<uint32_t, REVIEW_MAX_WINDOWS> reviewWindowStarts{};
  uint32_t reviewWindowEnd = 0;
  size_t reviewWindowIndex = 0;
  size_t reviewLineStart = 0;
  int reviewLinesPerPage = 0;
  int reviewWrappedWidth = 0;
  bool reviewWindowLoaded = false;
  bool reviewShowLastPage = false;

  // Partial-render caches: set on the first full render, reused when only the
  // description page changes.
  bool fullRenderDone = false;
  // Paged-content band geometry and navigation flags are published together:
  // render() writes them and loop() reads them to resolve taps on another task.
  struct DescBand {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool hasPrevious = false;
    bool hasNext = false;
  };
  DescBand descBand;
  SeqPublish descBandSeq;
  int hintsBandY = 0;
  int hintsBandHeight = 0;
  int partialRenderCount = 0;
  static constexpr int MAX_PARTIAL_RENDERS = 10;

  static std::string formatFileSize(size_t bytes);
  void renderLoading();
  void loadData();
  void renderDescriptionAndHints();
  bool loadReviewWindow();

 public:
  explicit BookInfoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath)
      : Activity("BookInfo", renderer, mappedInput), filePath(std::move(filePath)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
};
