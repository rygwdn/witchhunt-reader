#include "LogViewerActivity.h"

// Whole translation unit is gated: on envs without ENABLE_LOG_VIEWER (gh_release,
// gh_release_rc, slim) this file must compile to an empty object, not just be
// unreachable from the UI, so the debug tool costs zero flash in shipped builds.
#ifdef ENABLE_LOG_VIEWER

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>

#include "DiskLogger.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t INDEX_CHUNK_SIZE = 192;
constexpr size_t PAGE_CHUNK_SIZE = 192;
}  // namespace

void LogViewerActivity::onEnter() {
  Activity::onEnter();

  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const auto& metrics = UITheme::getInstance().getMetrics();
  marginTop = top + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  marginLeft = left + metrics.contentSidePadding;
  const int marginRight = right + metrics.contentSidePadding;
  marginBottom = bottom + metrics.buttonHintsHeight + metrics.verticalSpacing;
  viewportWidth = renderer.getScreenWidth() - marginLeft - marginRight;

  const int viewportHeight = renderer.getScreenHeight() - marginTop - marginBottom;
  linesPerPage = std::max(1, viewportHeight / renderer.getLineHeight(UI_10_FONT_ID));
  pageLines.resize(static_cast<size_t>(linesPerPage));

  scanAvailableGenerations();
  if (!availableGenerations.empty()) {
    openGeneration(0);
  }

  requestUpdate();
}

void LogViewerActivity::scanAvailableGenerations() {
  availableGenerations.clear();
  availableGenerations.reserve(DiskLogger::getGenerationCount());
  for (int gen = 0; gen < DiskLogger::getGenerationCount(); gen++) {
    if (Storage.exists(DiskLogger::getLogFilePath(gen).c_str())) {
      availableGenerations.push_back(gen);
    }
  }
}

void LogViewerActivity::openGeneration(int availIdx) {
  availableIndex = availIdx;
  currentPath = DiskLogger::getLogFilePath(availableGenerations[availIdx]);
  buildPageIndex();
  currentPage = std::max(0, totalPages - 1);
}

void LogViewerActivity::buildPageIndex() {
  pageOffsets.clear();
  totalPages = 0;

  HalFile f;
  if (!Storage.openFileForRead("LOGV", currentPath, f)) {
    return;
  }

  const size_t fileSize = f.fileSize();
  if (fileSize == 0) {
    return;
  }

  // Generous underestimate of bytes-per-page (assumes very short average lines) so we
  // reserve enough up front and never hit a vector growth reallocation while scanning
  // — a 512KB log at worst-case-short lines would otherwise realloc a dozen times.
  const size_t estimatedPages = fileSize / (static_cast<size_t>(linesPerPage) * 16) + 2;
  pageOffsets.reserve(estimatedPages);
  pageOffsets.push_back(0);

  uint8_t buffer[INDEX_CHUNK_SIZE];
  size_t offset = 0;
  int lineCount = 0;
  while (offset < fileSize) {
    const size_t toRead = std::min(INDEX_CHUNK_SIZE, fileSize - offset);
    const int n = f.read(buffer, toRead);
    if (n <= 0) break;
    for (int i = 0; i < n; i++) {
      if (buffer[i] == '\n') {
        lineCount++;
        if (lineCount % linesPerPage == 0 && offset + i + 1 < fileSize) {
          pageOffsets.push_back(offset + i + 1);
        }
      }
    }
    offset += static_cast<size_t>(n);
  }

  totalPages = static_cast<int>(pageOffsets.size());
  // file closes automatically via DESTRUCTOR_CLOSES_FILE
}

void LogViewerActivity::loadPage(int page) {
  visibleLineCount = 0;
  for (auto& line : pageLines) line.clear();
  if (page < 0 || page >= totalPages) return;

  HalFile f;
  if (!Storage.openFileForRead("LOGV", currentPath, f)) return;

  const size_t fileSize = f.fileSize();
  size_t pos = pageOffsets[page];
  uint8_t buffer[PAGE_CHUNK_SIZE];

  while (pos < fileSize && visibleLineCount < pageLines.size()) {
    f.seek(pos);
    const size_t toRead = std::min(sizeof(buffer), fileSize - pos);
    const int n = f.read(buffer, toRead);
    if (n <= 0) break;

    int consumed = 0;
    for (; consumed < n; consumed++) {
      if (buffer[consumed] == '\n') {
        ++visibleLineCount;
        if (visibleLineCount >= pageLines.size()) {
          ++consumed;
          break;
        }
      } else if (buffer[consumed] != '\r') {
        pageLines[visibleLineCount].push_back(static_cast<char>(buffer[consumed]));
      }
    }
    pos += static_cast<size_t>(consumed);
  }

  if (pos == fileSize && visibleLineCount < pageLines.size() &&
      !pageLines[visibleLineCount].empty()) {
    ++visibleLineCount;
  }
  // file closes automatically via DESTRUCTOR_CLOSES_FILE
}
void LogViewerActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (availableGenerations.empty()) {
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (currentPage > 0) {
      currentPage--;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (currentPage < totalPages - 1) {
      currentPage++;
      requestUpdate();
    }
    return;
  }

  // Left cycles to older backups, Right cycles back toward the active log.
  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    if (availableIndex + 1 < static_cast<int>(availableGenerations.size())) {
      openGeneration(availableIndex + 1);
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    if (availableIndex > 0) {
      openGeneration(availableIndex - 1);
      requestUpdate();
    }
    return;
  }
}

void LogViewerActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_VIEW_LOGS));

  if (totalPages == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2, tr(STR_EMPTY_FILE));
  } else {
    loadPage(currentPage);

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    int y = marginTop;
    for (size_t i = 0; i < visibleLineCount; ++i) {
      const auto& line = pageLines[i];
      // Clipped to the viewport, not drawn raw: log lines (WebDAV paths, MEM stats)
      // routinely overrun the panel in portrait, and every off-panel pixel costs a
      // synchronous GFX error log line — which itself lands in the very file being
      // viewed, and stalls the render behind a flood of SD writes.
      renderer.drawText(UI_10_FONT_ID, marginLeft, y,
                        renderer.truncatedText(UI_10_FONT_ID, line.c_str(), viewportWidth).c_str());
      y += lineHeight;
    }

    const size_t slash = currentPath.rfind('/');
    const char* fileName = currentPath.c_str() + (slash == std::string::npos ? 0 : slash + 1);
    const float progress = totalPages > 0 ? (currentPage + 1) * 100.0f / totalPages : 0;
    GUI.drawStatusBar(renderer, progress, currentPage + 1, totalPages, fileName);
  }

  const bool hasOlder = availableIndex + 1 < static_cast<int>(availableGenerations.size());
  const bool hasNewer = availableIndex > 0;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", hasOlder ? "<" : "", hasNewer ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

#endif  // ENABLE_LOG_VIEWER
