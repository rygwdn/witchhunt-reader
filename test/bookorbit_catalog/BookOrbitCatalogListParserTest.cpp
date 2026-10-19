#include <BookOrbitCatalogListParser.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

// Parses `json` in `chunkSize`-byte pieces so every test also exercises the
// streaming path the device uses (the body arrives in SecureHttpClient's 2 KB
// reads, which land mid-token).
std::vector<BookOrbitEntry> parseRows(const std::string& json, BookOrbitPageInfo* pageOut = nullptr,
                                      size_t chunkSize = 0) {
  std::vector<BookOrbitEntry> rows;
  BookOrbitCatalogListParser parser([&rows](const BookOrbitEntry& entry) { rows.push_back(entry); });

  if (chunkSize == 0) {
    parser.feed(json.data(), json.size());
  } else {
    for (size_t offset = 0; offset < json.size(); offset += chunkSize) {
      parser.feed(json.data() + offset, std::min(chunkSize, json.size() - offset));
    }
  }

  EXPECT_FALSE(parser.hasError());
  EXPECT_EQ(parser.entryCount(), rows.size());
  if (pageOut) *pageOut = parser.pageInfo();
  return rows;
}

// GET /koreader/plugin/catalog/root, abridged to three of its eight rows.
constexpr char ROOT_JSON[] = R"({
  "sections": [
    {"id": "continue-reading", "title": "Continue reading", "section": "continue-reading",
     "booksHref": "/api/v1/koreader/plugin/catalog/books?sort=recently_read&readStatus=reading"},
    {"id": "libraries", "title": "Libraries", "section": "libraries",
     "href": "/api/v1/koreader/plugin/catalog/sections/libraries"},
    {"id": "all-books", "title": "All Books", "section": "all-books",
     "booksHref": "/api/v1/koreader/plugin/catalog/books?sort=title"}
  ]
})";

// GET /koreader/plugin/catalog/books, two items plus the pagination envelope.
constexpr char BOOKS_JSON[] = R"({
  "items": [
    {"id": 412, "title": "Leviathan Wakes", "authors": ["James S. A. Corey", "Daniel Abraham"],
     "seriesId": 9, "seriesName": "The Expanse", "seriesIndex": "1", "progressPercentage": 42.6,
     "lastReadAt": "2026-09-01T10:00:00.000Z", "readStatus": "reading", "formats": ["epub", "pdf"],
     "hasCover": true, "thumbnailUrl": "/api/v1/koreader/plugin/catalog/books/412/thumbnail",
     "detailUrl": "/api/v1/koreader/plugin/catalog/books/412",
     "addedAt": "2026-08-01T10:00:00.000Z", "updatedAt": "2026-09-01T10:00:00.000Z"},
    {"id": 77, "title": "Solaris", "authors": [], "seriesId": null, "seriesName": null,
     "seriesIndex": null, "progressPercentage": null, "lastReadAt": null, "readStatus": null,
     "formats": ["epub"], "hasCover": false, "thumbnailUrl": null,
     "detailUrl": "/api/v1/koreader/plugin/catalog/books/77",
     "addedAt": "2026-08-02T10:00:00.000Z", "updatedAt": "2026-08-02T10:00:00.000Z"}
  ],
  "total": 37,
  "page": 2,
  "size": 12,
  "hasNext": true,
  "hasPrevious": true,
  "nextUrl": "/api/v1/koreader/plugin/catalog/books?page=3&size=12&sort=title",
  "previousUrl": "/api/v1/koreader/plugin/catalog/books?page=1&size=12&sort=title"
})";

// GET /koreader/plugin/catalog/sections/series — navigation rows with counts.
constexpr char SERIES_JSON[] = R"({
  "section": "series",
  "items": [
    {"id": "series:9", "title": "The Expanse", "section": "series", "count": 9, "seriesId": 9,
     "booksHref": "/api/v1/koreader/plugin/catalog/books?seriesId=9&sort=series"},
    {"id": "series:14", "title": "Culture", "section": "series", "count": 10, "seriesId": 14,
     "booksHref": "/api/v1/koreader/plugin/catalog/books?seriesId=14&sort=series"}
  ],
  "page": 1,
  "hasNext": false,
  "hasPrevious": false,
  "nextUrl": null,
  "previousUrl": null,
  "query": null
})";

}  // namespace

TEST(BookOrbitCatalogListParser, ReadsRootNavigationRows) {
  const auto rows = parseRows(ROOT_JSON);

  ASSERT_EQ(rows.size(), 3u);
  for (const auto& row : rows) {
    EXPECT_EQ(row.type, BookOrbitEntryType::NAVIGATION);
    EXPECT_EQ(row.bookId, -1);
  }
  EXPECT_EQ(rows[0].title, "Continue reading");
  EXPECT_EQ(rows[0].href, "/api/v1/koreader/plugin/catalog/books?sort=recently_read&readStatus=reading");
  EXPECT_EQ(rows[1].href, "/api/v1/koreader/plugin/catalog/sections/libraries");
  EXPECT_EQ(rows[2].title, "All Books");
}

TEST(BookOrbitCatalogListParser, ReadsBookRowsAndPagination) {
  BookOrbitPageInfo page;
  const auto rows = parseRows(BOOKS_JSON, &page);

  ASSERT_EQ(rows.size(), 2u);

  const auto& first = rows[0];
  EXPECT_EQ(first.type, BookOrbitEntryType::BOOK);
  EXPECT_EQ(first.bookId, 412);
  EXPECT_EQ(first.title, "Leviathan Wakes");
  EXPECT_EQ(first.subtitle, "James S. A. Corey \xC2\xB7 Daniel Abraham");
  EXPECT_EQ(first.series, "The Expanse #1");
  EXPECT_EQ(first.formats, "epub \xC2\xB7 pdf");
  EXPECT_EQ(first.progressPercent, 43);  // 42.6 rounds to whole percent
  EXPECT_TRUE(first.hasCover);
  EXPECT_EQ(first.href, "/api/v1/koreader/plugin/catalog/books/412");

  // A book with nothing recorded: nulls must not become zero progress or a
  // phantom series, which the reader would draw as real information.
  const auto& second = rows[1];
  EXPECT_EQ(second.type, BookOrbitEntryType::BOOK);
  EXPECT_EQ(second.bookId, 77);
  EXPECT_TRUE(second.subtitle.empty());
  EXPECT_TRUE(second.series.empty());
  EXPECT_EQ(second.progressPercent, -1);
  EXPECT_FALSE(second.hasCover);

  EXPECT_EQ(page.total, 37);
  EXPECT_EQ(page.page, 2);
  EXPECT_EQ(page.nextUrl, "/api/v1/koreader/plugin/catalog/books?page=3&size=12&sort=title");
  EXPECT_EQ(page.prevUrl, "/api/v1/koreader/plugin/catalog/books?page=1&size=12&sort=title");
}

TEST(BookOrbitCatalogListParser, ReadsSectionRowsWithCounts) {
  BookOrbitPageInfo page;
  const auto rows = parseRows(SERIES_JSON, &page);

  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].type, BookOrbitEntryType::NAVIGATION);
  EXPECT_EQ(rows[0].title, "The Expanse");
  EXPECT_EQ(rows[0].count, 9);
  EXPECT_EQ(rows[0].href, "/api/v1/koreader/plugin/catalog/books?seriesId=9&sort=series");
  EXPECT_EQ(rows[1].count, 10);

  // A last page must not offer paging rows.
  EXPECT_TRUE(page.nextUrl.empty());
  EXPECT_TRUE(page.prevUrl.empty());
}

TEST(BookOrbitCatalogListParser, PreservesDashboardShelfGroups) {
  constexpr char json[] = R"({
    "sections": [{"id":"all-books","title":"All Books","booksHref":"/books"}],
    "continueReading": [{"id":1,"title":"Current","authors":["A"],"detailUrl":"/books/1"}],
    "discover": [{"id":2,"title":"Suggested","authors":["B"],"detailUrl":"/books/2"}]
  })";

  const auto rows = parseRows(json, nullptr, 3);

  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].group, BookOrbitEntryGroup::STANDARD);
  EXPECT_EQ(rows[1].group, BookOrbitEntryGroup::CONTINUE_READING);
  EXPECT_EQ(rows[2].group, BookOrbitEntryGroup::DISCOVER);
  EXPECT_EQ(rows[1].title, "Current");
  EXPECT_EQ(rows[2].title, "Suggested");
}

TEST(BookOrbitCatalogListParser, ChunkBoundariesDoNotChangeResults) {
  const auto whole = parseRows(BOOKS_JSON);

  // Sizes chosen to split inside strings, numbers, keys and structural chars.
  for (const size_t chunk : {1u, 3u, 7u, 64u, 512u}) {
    BookOrbitPageInfo page;
    const auto chunked = parseRows(BOOKS_JSON, &page, chunk);
    ASSERT_EQ(chunked.size(), whole.size()) << "chunk size " << chunk;
    for (size_t i = 0; i < whole.size(); ++i) {
      EXPECT_EQ(chunked[i].title, whole[i].title) << "chunk size " << chunk;
      EXPECT_EQ(chunked[i].subtitle, whole[i].subtitle) << "chunk size " << chunk;
      EXPECT_EQ(chunked[i].series, whole[i].series) << "chunk size " << chunk;
      EXPECT_EQ(chunked[i].formats, whole[i].formats) << "chunk size " << chunk;
      EXPECT_EQ(chunked[i].bookId, whole[i].bookId) << "chunk size " << chunk;
      EXPECT_EQ(chunked[i].progressPercent, whole[i].progressPercent) << "chunk size " << chunk;
    }
    EXPECT_EQ(page.nextUrl, "/api/v1/koreader/plugin/catalog/books?page=3&size=12&sort=title");
  }
}

TEST(BookOrbitCatalogListParser, IgnoresNestedStructuresInsideRows) {
  // The detail-shaped fields a future server may add to a listing (nested
  // objects and arrays of objects with their own "title") must not overwrite
  // the row's own fields, or a related book's name would end up on the row.
  constexpr char json[] = R"({
    "items": [
      {"id": 5, "title": "Real Title", "authors": ["A. Author"],
       "collections": [{"id": 1, "name": "Favourites", "title": "Not The Row Title"}],
       "progress": {"fileId": 3, "percentage": 99, "title": "Nope"},
       "relatedSections": [{"id": "series", "title": "More in series",
                            "books": [{"id": 6, "title": "Another Book", "hasCover": true}]}],
       "formats": ["epub"], "hasCover": false,
       "detailUrl": "/api/v1/koreader/plugin/catalog/books/5"}
    ],
    "total": 1
  })";

  const auto rows = parseRows(json);

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].title, "Real Title");
  EXPECT_EQ(rows[0].bookId, 5);
  EXPECT_EQ(rows[0].subtitle, "A. Author");
  EXPECT_EQ(rows[0].formats, "epub");
  EXPECT_FALSE(rows[0].hasCover);
  EXPECT_EQ(rows[0].progressPercent, -1);
}

TEST(BookOrbitCatalogListParser, DropsRowsTheReaderCouldNotUse) {
  // A row with no title has nothing to draw; a navigation row with no
  // destination has nothing to open. Either would be a blank selectable line.
  constexpr char json[] = R"({
    "items": [
      {"id": "orphan", "section": "libraries"},
      {"id": "no-target", "title": "Nowhere"},
      {"id": "good", "title": "Fiction", "href": "/api/v1/koreader/plugin/catalog/sections/libraries"}
    ]
  })";

  const auto rows = parseRows(json);

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0].title, "Fiction");
}

TEST(BookOrbitCatalogListParser, ResetClearsPreviousListing) {
  std::vector<BookOrbitEntry> rows;
  BookOrbitCatalogListParser parser([&rows](const BookOrbitEntry& entry) { rows.push_back(entry); });

  parser.feed(BOOKS_JSON, sizeof(BOOKS_JSON) - 1);
  ASSERT_EQ(rows.size(), 2u);
  ASSERT_EQ(parser.pageInfo().total, 37);

  rows.clear();
  parser.reset();
  parser.feed(SERIES_JSON, sizeof(SERIES_JSON) - 1);

  EXPECT_EQ(rows.size(), 2u);
  EXPECT_EQ(parser.entryCount(), 2u);
  EXPECT_EQ(parser.pageInfo().total, -1);  // series pages carry no total
  EXPECT_TRUE(parser.pageInfo().nextUrl.empty());
}
