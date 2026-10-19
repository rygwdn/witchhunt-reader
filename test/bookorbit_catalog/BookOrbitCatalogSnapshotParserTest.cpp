#include <BookOrbitCatalogSnapshotParser.h>
#include <StreamingJsonParser.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {
struct Events {
  std::vector<int32_t> books;
  std::vector<int32_t> memberships;
  std::vector<int32_t> sources;
  std::vector<int32_t> shelves;
  std::string formats;
  bool membershipAfterBook = true;
  static bool book(void* p, const BookOrbitEntry& entry) {
    auto& e = *static_cast<Events*>(p);
    e.books.push_back(entry.bookId);
    e.formats = entry.formats;
    return true;
  }
  static bool source(void* p, BookOrbitCatalogSettings::SourceKind, int32_t id, const std::string&, int32_t) {
    static_cast<Events*>(p)->sources.push_back(id);
    return true;
  }
  static bool membership(void* p, int32_t bookId, BookOrbitCatalogSettings::SourceKind, int32_t id) {
    auto& e = *static_cast<Events*>(p);
    e.membershipAfterBook &= !e.books.empty() && e.books.back() == bookId;
    e.memberships.push_back(id);
    return true;
  }
  static bool shelf(void* p, BookOrbitEntryGroup, int32_t id) {
    static_cast<Events*>(p)->shelves.push_back(id);
    return true;
  }
  BookOrbitSnapshotCallbacks callbacks() { return {this, book, source, membership, shelf}; }
};

std::string fixture() {
  return R"({"version":1,"generation":"gen-a","etag":"etag-a","total":1,
    "books":[{"memberships":{"smartScopeIds":[31],"collectionIds":[21],"libraryIds":[11]},
      "files":[{"format":"epub","id":7}],"hasCover":true,"progressPercentage":42.6,
      "seriesIndex":"2","seriesName":"Series","authors":["A","B"],"title":"Book","id":9,
      "description":"DESC"}],"sources":{"smartScopes":[{"bookCount":1,"name":"Smart","id":31}],
      "collections":[{"id":21,"name":"Collection","bookCount":1}],
      "libraries":[{"name":"Library","id":11,"bookCount":1}]},
    "shelves":{"discover":[9],"continueReading":[9]},"nextCursor":null})";
}

bool parse(const std::string& json, size_t chunk, Events& events, BookOrbitSnapshotPage& page) {
  BookOrbitCatalogSnapshotParser parser(events.callbacks());
  for (size_t offset = 0; offset < json.size(); offset += chunk) {
    parser.feed(json.data() + offset, std::min(chunk, json.size() - offset));
    if (parser.hasError()) return false;
  }
  return parser.finish(page);
}
}  // namespace

TEST(BookOrbitCatalogSnapshotParser, ParsesPageAndFieldOrderIndependently) {
  Events events;
  BookOrbitSnapshotPage page;
  ASSERT_TRUE(parse(fixture(), 1, events, page));
  EXPECT_EQ(page.version, 1u);
  EXPECT_EQ(page.total, 1);
  EXPECT_EQ(page.generation, "gen-a");
  EXPECT_EQ(page.etag, "etag-a");
  EXPECT_TRUE(page.nextCursor.empty());
  EXPECT_EQ(events.books, (std::vector<int32_t>{9}));
  EXPECT_EQ(events.memberships, (std::vector<int32_t>{11, 21, 31}));
  EXPECT_EQ(events.sources, (std::vector<int32_t>{31, 21, 11}));
  EXPECT_EQ(events.shelves, (std::vector<int32_t>{9, 9}));
  EXPECT_TRUE(events.membershipAfterBook);
}
TEST(BookOrbitCatalogSnapshotParser, DeduplicatesFormatsAcrossManyFiles) {
  std::string json = fixture();
  const std::string original = "\"files\":[{\"format\":\"epub\",\"id\":7}]";
  std::string files = "\"files\":[";
  for (int i = 1; i <= 200; ++i) {
    if (i > 1) files += ',';
    files += "{\"format\":\"Epub\",\"id\":" + std::to_string(i) + "}";
  }
  files += ",{\"format\":\"PDF\",\"id\":201}]";
  json.replace(json.find(original), original.size(), files);
  Events events;
  BookOrbitSnapshotPage page;
  ASSERT_TRUE(parse(json, 113, events, page));
  EXPECT_EQ(events.formats, "epub \xC2\xB7 pdf");
}

TEST(BookOrbitCatalogSnapshotParser, BoundsUniqueFormatCount) {
  const std::string original = "\"files\":[{\"format\":\"epub\",\"id\":7}]";
  auto withCount = [&](int count) {
    std::string files = "\"files\":[";
    for (int i = 0; i < count; ++i) {
      if (i) files += ',';
      files += "{\"format\":\"f" + std::to_string(i) + "\",\"id\":" + std::to_string(i + 1) + "}";
    }
    files += ']';
    std::string value = fixture();
    value.replace(value.find(original), original.size(), files);
    return value;
  };
  Events accepted;
  BookOrbitSnapshotPage page;
  EXPECT_TRUE(parse(withCount(20), 151, accepted, page));
  Events rejected;
  EXPECT_FALSE(parse(withCount(21), 151, rejected, page));
}

TEST(BookOrbitCatalogSnapshotParser, StreamsLargeDescriptionsWithoutRetainingThem) {
  std::string json = fixture();
  const std::string needle = "DESC";
  json.replace(json.find(needle), needle.size(), std::string(24000, 'x'));
  Events events;
  BookOrbitSnapshotPage page;
  ASSERT_TRUE(parse(json, 127, events, page));
  ASSERT_EQ(events.books.size(), 1u);
}

TEST(BookOrbitCatalogSnapshotParser, RejectsTruncationMalformedSchemaAndCallbackFailure) {
  auto json = fixture();
  Events events;
  BookOrbitSnapshotPage page;
  EXPECT_FALSE(parse(json.substr(0, json.size() - 7), 17, events, page));
  std::string invalid = fixture();
  invalid.replace(invalid.find("\"version\":1"), 11, "\"version\":2");
  Events invalidEvents;
  EXPECT_FALSE(parse(invalid, 23, invalidEvents, page));
  struct Reject {
    static bool book(void*, const BookOrbitEntry&) { return false; }
  };
  BookOrbitSnapshotCallbacks callbacks{nullptr, Reject::book, Events::source, Events::membership, Events::shelf};
  BookOrbitCatalogSnapshotParser parser(callbacks);
  const std::string valid = fixture();
  parser.feed(valid.data(), valid.size());
  EXPECT_TRUE(parser.hasError());
  EXPECT_FALSE(parser.finish(page));
}
TEST(BookOrbitCatalogSnapshotParser, EnforcesMembershipAndJoinedTextBounds) {
  auto base = fixture();
  const std::string memberKey = "\"libraryIds\":[11]";
  std::string ids = "\"libraryIds\":[";
  for (int i = 1; i <= 500; ++i) {
    if (i > 1) ids += ',';
    ids += std::to_string(i);
  }
  ids += ']';
  std::string exact = base;
  exact.replace(exact.find(memberKey), memberKey.size(), ids);
  Events accepted;
  BookOrbitSnapshotPage page;
  ASSERT_TRUE(parse(exact, 257, accepted, page));
  EXPECT_EQ(accepted.memberships.size(), 502u);

  std::string overflow = exact;
  const size_t list = overflow.find("\"libraryIds\":[") + strlen("\"libraryIds\":[");
  overflow.insert(list + overflow.find(']', list) - list, ",501");
  Events rejected;
  EXPECT_FALSE(parse(overflow, 257, rejected, page));

  std::string longAuthors = base;
  longAuthors.replace(longAuthors.find("[\"A\",\"B\"]"), 9, "[\"" + std::string(513, 'x') + "\"]");
  Events textRejected;
  EXPECT_FALSE(parse(longAuthors, 64, textRejected, page));
}

TEST(BookOrbitCatalogSnapshotParser, RejectsNullSourceRows) {
  std::string json = fixture();
  const std::string key = "\"libraries\":[";
  json.replace(json.find(key), key.size(), key + "null,");
  Events events;
  BookOrbitSnapshotPage page;
  EXPECT_FALSE(parse(json, 31, events, page));
}
TEST(BookOrbitCatalogSnapshotParser, RejectsMalformedBookAndShelfArrayElements) {
  const char* replacements[][2] = {
      {"\"books\":[{", "\"books\":[null,{"},
      {"\"continueReading\":[9]", "\"continueReading\":[null,9]"},
  };
  for (const auto& replacement : replacements) {
    std::string json = fixture();
    const std::string from = replacement[0], to = replacement[1];
    json.replace(json.find(from), from.size(), to);
    Events events;
    BookOrbitSnapshotPage page;
    EXPECT_FALSE(parse(json, 29, events, page));
  }
}
TEST(BookOrbitCatalogSnapshotParser, HandlesMaximumNestingAndParserReset) {
  std::string json = fixture();
  std::string nested = "0";
  for (int i = 0; i < 29; ++i) nested = "{\"x\":" + nested + "}";
  json.replace(json.find("\"DESC\""), 6, nested);
  Events events;
  BookOrbitSnapshotPage page;
  ASSERT_TRUE(parse(json, 47, events, page));

  std::string tooDeep = "0";
  for (int i = 0; i < 30; ++i) tooDeep = "{\"x\":" + tooDeep + "}";
  json = fixture();
  json.replace(json.find("\"DESC\""), 6, tooDeep);
  Events rejected;
  EXPECT_FALSE(parse(json, 47, rejected, page));

  StreamingJsonParser stream(JsonCallbacks{});
  const std::string partial = "{\"a\":[";
  stream.feed(partial.data(), partial.size());
  stream.reset();
  const std::string valid = "{\"x\":1}";
  stream.feed(valid.data(), valid.size());
  EXPECT_TRUE(stream.finish());
}
