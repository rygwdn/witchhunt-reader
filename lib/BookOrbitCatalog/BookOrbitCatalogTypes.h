#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Wire model for BookOrbit's KOReader catalog API
// (GET /koreader/plugin/catalog/...). Only the fields the reader actually
// renders or follows are kept: the server's list item carries a dozen more
// (timestamps, library ids, thumbnail urls we can derive from the book id),
// and every one of them would be a string allocation per row on a 380 KB
// device.

enum class BookOrbitEntryType : uint8_t {
  NAVIGATION = 0,  // a row that leads to another listing (library, author, series, ...)
  BOOK = 1,        // a row that leads to a book detail
};

// Which dashboard array produced a row. Ordinary root/section/book listings use
// DEFAULT; the combined dashboard response tags its cover shelves so the UI can
// retain section boundaries without allocating a section-name string per book.
enum class BookOrbitEntryGroup : uint8_t {
  STANDARD = 0,
  CONTINUE_READING = 1,
  DISCOVER = 2,
};

// One row of a catalog listing. Both shapes the API returns collapse into this:
// KoreaderCatalogEntry (navigation) and KoreaderCatalogBookListItem (book).
// The discriminator is `detailUrl`, which only book items carry.
struct BookOrbitEntry {
  BookOrbitEntryType type = BookOrbitEntryType::NAVIGATION;
  BookOrbitEntryGroup group = BookOrbitEntryGroup::STANDARD;
  std::string title;
  std::string subtitle;  // navigation: server subtitle; book: joined author list
  std::string href;      // navigation: href/booksHref to follow; book: detailUrl
  std::string series;    // book: series name, with the index appended when present
  std::string formats;   // book: joined format list ("epub · pdf")
  int32_t bookId = -1;   // book: numeric id, for thumbnail/read-status/rating
  int32_t count = -1;    // navigation: books behind the row; -1 when the server omits it
  int16_t progressPercent = -1;  // book: 0..100; -1 when no progress is recorded
  bool hasCover = false;
};

// One downloadable file of a book. `id` is only carried for diagnostics — the
// download goes to `downloadUrl`, which the server composes itself.
struct BookOrbitFile {
  int32_t id = -1;
  std::string format;  // "epub", "pdf", ... (lowercase, as the server stores it)
  std::string downloadUrl;
  std::string fileHash;  // KOReader partial MD5 when the server can identify exact content bytes
  uint32_t sizeBytes = 0;
};

// Book detail (KoreaderCatalogBookDetail), reduced to what the detail screen
// shows plus the file list the download needs.
struct BookOrbitBookDetail {
  int32_t bookId = -1;
  std::string title;
  std::string authors;  // joined
  std::string series;
  std::string description;
  std::string publisher;
  std::string readStatus;        // server enum: unread|want_to_read|reading|on_hold|read|abandoned
  int16_t progressPercent = -1;  // 0..100, -1 when unknown
  int16_t ratingTenths = -1;     // rating * 10 (server sends 0..5, possibly fractional); -1 unrated
  int16_t publishedYear = -1;
  int32_t pageCount = -1;
  bool hasCover = false;
  std::vector<BookOrbitFile> files;
};

// Pagination links of a listing response (KoreaderCatalogPage /
// KoreaderCatalogSectionResponse). The urls are server-composed absolute paths,
// so the reader follows them verbatim instead of rebuilding query strings.
struct BookOrbitPageInfo {
  std::string nextUrl;
  std::string prevUrl;
  int32_t total = -1;
  int32_t page = 1;
};
