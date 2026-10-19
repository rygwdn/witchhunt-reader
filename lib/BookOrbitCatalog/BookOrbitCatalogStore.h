#pragma once

#include <cstdint>
#include <string>

#include "BookOrbitCatalogClient.h"

/** SD-backed snapshot of the configured BookOrbit scope and its local navigation graph. */
class BookOrbitCatalogStore {
 public:
  enum class RebuildStatus : uint8_t { InProgress, Complete, Failed, Cancelled };
  struct RebuildProgress {
    uint32_t completedPages = 0;
    uint32_t discoveredPages = 0;
  };

  static bool available();
  static bool beginRebuild();
  static RebuildStatus rebuildStep(RebuildProgress* progress = nullptr);
  static void cancelRebuild();
  static BookOrbitCatalogClient::Error rebuildError();
  static BookOrbitCatalogClient::Error rebuild(void (*progress)(void*, uint32_t), void* context);
  static bool listing(const std::string& url, const BookOrbitCatalogListParser::EntryCallback& onEntry,
                      BookOrbitPageInfo& outPage);
  static bool findBook(int32_t id, BookOrbitEntry& outEntry);
  static bool dashboard(const BookOrbitCatalogListParser::EntryCallback& callback);
  static bool search(const std::string& query, uint32_t page, const BookOrbitCatalogListParser::EntryCallback& onEntry,
                     BookOrbitPageInfo& outPage);
};
