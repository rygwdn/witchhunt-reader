#pragma once

#include <StreamingJsonParser.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "BookOrbitCatalogTypes.h"

/**
 * Streaming reader for a BookOrbit catalog *listing* response:
 *   GET /koreader/plugin/catalog/root            -> { "sections": [ entry, ... ] }
 *   GET /koreader/plugin/catalog/sections/:name  -> { "items": [ entry, ... ], "nextUrl": ... }
 *   GET /koreader/plugin/catalog/books?...       -> { "items": [ book, ... ], "total": n, ... }
 *
 * Rows are handed to onEntry as they come off the socket, so the caller can
 * spool them to the SD cache file (the OPDS browser's pattern) and never hold a
 * page in RAM. A DOM parse would need the raw JSON *and* the object tree
 * resident at once — on a 20-book page that is tens of kilobytes of the
 * device's ~380 KB, right before a TLS handshake wants a contiguous block.
 *
 * Both listing shapes are accepted by one state machine because they differ
 * only in the array key ("sections" vs "items") and in which per-row fields are
 * present; a row carrying `detailUrl` is a book, anything else is navigation.
 *
 * Long values (a description beyond StreamingJsonParser::TOKEN_BUF_SIZE) are
 * dropped by the tokenizer rather than truncated, so listings deliberately
 * carry only short fields. The book *detail* response, whose description is
 * routinely longer than the token buffer, is parsed with ArduinoJson in
 * BookOrbitCatalogClient instead.
 */
class BookOrbitCatalogListParser {
 public:
  using EntryCallback = std::function<void(const BookOrbitEntry&)>;

  explicit BookOrbitCatalogListParser(EntryCallback onEntry);

  BookOrbitCatalogListParser(const BookOrbitCatalogListParser&) = delete;
  BookOrbitCatalogListParser& operator=(const BookOrbitCatalogListParser&) = delete;

  void reset();
  void feed(const char* data, size_t len);

  bool hasError() const { return parser.hasError(); }
  size_t entryCount() const { return entries; }
  const BookOrbitPageInfo& pageInfo() const { return page; }

 private:
  enum class Key : uint8_t {
    NONE,
    // listing container
    ITEMS,
    CONTINUE_READING,
    DISCOVER,
    TOTAL,
    PAGE,
    NEXT_URL,
    PREV_URL,
    // row fields
    ID,
    TITLE,
    SUBTITLE,
    COUNT,
    HREF,
    DETAIL_URL,
    AUTHORS,
    FORMATS,
    SERIES_NAME,
    SERIES_INDEX,
    PROGRESS,
    HAS_COVER,
  };

  static void sOnKey(void* ctx, const char* key, size_t len);
  static void sOnString(void* ctx, const char* value, size_t len);
  static void sOnNumber(void* ctx, const char* value, size_t len);
  static void sOnBool(void* ctx, bool value);
  static void sOnNull(void* ctx);
  static void sOnObjectStart(void* ctx);
  static void sOnObjectEnd(void* ctx);
  static void sOnArrayStart(void* ctx);
  static void sOnArrayEnd(void* ctx);

  // True while the tokenizer is inside the row array's own element object.
  bool inRow() const { return rowDepth == 1; }
  // True while the tokenizer is at the response object's own level, i.e. where
  // the pagination fields live.
  bool atResponseLevel() const { return depth == 1 && !inRowArray && rowDepth == 0; }

  void appendJoined(std::string& target, const char* value, size_t len) const;
  void commitRow();

  StreamingJsonParser parser;
  EntryCallback onEntry;

  uint8_t depth = 0;         // container nesting outside the row array
  uint8_t rowDepth = 0;      // 0 = outside a row, 1 = the row object, >1 = nested in it
  bool inRowArray = false;   // inside "items"/"sections"
  Key lastKey = Key::NONE;
  BookOrbitEntryGroup rowGroup = BookOrbitEntryGroup::STANDARD;
  Key collecting = Key::NONE;  // AUTHORS/FORMATS while their string array is open

  BookOrbitEntry row;
  std::string rowSeriesIndex;
  bool rowIsBook = false;

  BookOrbitPageInfo page;
  size_t entries = 0;
};
