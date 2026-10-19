#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "BookOrbitCatalogSettings.h"
#include "BookOrbitCatalogTypes.h"

struct BookOrbitSnapshotPage {
  uint32_t version = 0;
  int32_t total = -1;
  std::string generation;
  std::string etag;
  std::string nextCursor;
};

struct BookOrbitSnapshotCallbacks {
  void* context = nullptr;
  // For each book, onBook is followed synchronously by all its onMembership calls before
  // the BookOrbitEntry is changed or reused; consumers may retain its address through that sequence.
  bool (*onBook)(void*, const BookOrbitEntry&) = nullptr;
  bool (*onSource)(void*, BookOrbitCatalogSettings::SourceKind, int32_t, const std::string&, int32_t) = nullptr;
  bool (*onMembership)(void*, int32_t, BookOrbitCatalogSettings::SourceKind, int32_t) = nullptr;
  bool (*onShelf)(void*, BookOrbitEntryGroup, int32_t) = nullptr;
};

/** Incremental parser for one BookOrbit frozen-snapshot page. Feed all body bytes,
 * then call finish(); callbacks occur during feed and any rejected callback fails closed. */
class BookOrbitCatalogSnapshotParser {
 public:
  explicit BookOrbitCatalogSnapshotParser(const BookOrbitSnapshotCallbacks& callbacks);
  ~BookOrbitCatalogSnapshotParser();
  BookOrbitCatalogSnapshotParser(const BookOrbitCatalogSnapshotParser&) = delete;
  BookOrbitCatalogSnapshotParser& operator=(const BookOrbitCatalogSnapshotParser&) = delete;

  void feed(const char* data, size_t length);
  bool finish(BookOrbitSnapshotPage& outPage);
  bool hasError() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
