#include <gtest/gtest.h>

#include <algorithm>

#include "OpdsJsonParser.h"

namespace {
void feed(OpdsJsonParser& parser, const std::string& body, size_t chunk = 1) {
  for (size_t i = 0; i < body.size(); i += chunk)
    parser.feed(reinterpret_cast<const uint8_t*>(body.data() + i), std::min(chunk, body.size() - i));
}
}  // namespace

TEST(OpdsJsonParser, StreamsDashboardWithSeriesProgressAndUnicode) {
  OpdsJsonParser parser;
  std::vector<OpdsEntry> entries;
  parser.onEntryParsed = [&](OpdsEntry entry) { entries.push_back(std::move(entry)); };
  feed(
      parser,
      R"({"metadata":{"title":"Dashboard"},"links":[{"rel":["next"],"href":"?page=2"},{"rel":"search","href":"search{?query}","type":"application/opds+json"}],"groups":[{"metadata":{"title":"Up Next"},"publications":[{"metadata":{"title":"Caf\u00e9 \ud83d\udcd6","identifier":"urn:book:7","author":[{"name":"Ada"},"Bob"],"belongsTo":{"series":{"name":"Saga","position":2,"links":[{"href":"/series/9"}]}},"https://bookorbit.app/ns/readingProgress":0.42},"images":[{"href":"/cover.jpg","type":"image/jpeg"}],"links":[{"href":"/book.epub","rel":["acquisition"],"type":"application/epub+zip"}]}]},{"metadata":{"title":"Browse"},"navigation":[{"title":"All books","href":"/all"}]}]})");
  ASSERT_TRUE(parser.finish());
  ASSERT_EQ(entries.size(), 3u);
  EXPECT_EQ(entries[0].title, "Café 📖");
  EXPECT_EQ(entries[0].author, "Ada, Bob");
  EXPECT_EQ(entries[0].groupTitle, "Up Next");
  EXPECT_EQ(entries[0].seriesTitle, "Saga #2");
  EXPECT_EQ(entries[0].seriesHref, "/series/9");
  EXPECT_EQ(entries[0].progressPercent, 42);
  ASSERT_EQ(entries[0].acquisitionLinks.size(), 1u);
  EXPECT_EQ(entries[0].acquisitionLinks[0].formatKey, "epub");
  EXPECT_EQ(entries[0].imageHref, "/cover.jpg");
  EXPECT_EQ(entries[1].title, "All books");
  EXPECT_EQ(entries[1].type, OpdsEntryType::NAVIGATION);
  EXPECT_EQ(entries[1].groupTitle, "Browse");
  EXPECT_EQ(parser.getNextPageUrl(), "?page=2");
  EXPECT_EQ(parser.getSearchTemplate(), "search{?query}");
}

// Regression: a series' name and position are siblings the feed may emit in
// either order; the label must compose to "Name #position" regardless, so the
// position has to wait for the series object to close.
TEST(OpdsJsonParser, ComposesSeriesLabelForBothKeyOrders) {
  const char* bodies[] = {
      R"({"publications":[{"metadata":{"title":"A","belongsTo":{"series":{"position":2,"name":"Saga"}}}}]})",
      R"({"publications":[{"metadata":{"title":"A","belongsTo":{"series":{"name":"Saga","position":2}}}}]})",
  };
  for (const std::string body : bodies) {
    OpdsJsonParser parser;
    OpdsEntry result;
    parser.onEntryParsed = [&](OpdsEntry entry) { result = std::move(entry); };
    feed(parser, body);
    ASSERT_TRUE(parser.finish()) << body;
    EXPECT_EQ(result.seriesTitle, "Saga #2") << body;
  }
  // No position: the label stays the bare name.
  OpdsJsonParser parser;
  OpdsEntry result;
  parser.onEntryParsed = [&](OpdsEntry entry) { result = std::move(entry); };
  feed(parser, R"({"publications":[{"metadata":{"title":"A","belongsTo":{"series":{"name":"Saga"}}}}]})");
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(result.seriesTitle, "Saga");
}

TEST(OpdsJsonParser, RejectsMalformedAndTruncatedFeeds) {
  for (const std::string body :
       {R"({"navigation":[{"title":"Series","href":"/series"}])", R"({"navigation":[],})", R"({"navigation":[,]})",
        R"({"navigation":[] "links":[]})", R"({"navigation":[],"bad":"\ud800"})", R"({"navigation":[],"bad":01})",
        R"({"navigation":[],"bad":truex})", R"({"navigation":[]} garbage)", R"({"navigation":[],"bad":"\x"})"}) {
    OpdsJsonParser parser;
    feed(parser, body);
    EXPECT_FALSE(parser.finish()) << body;
  }
}

TEST(OpdsJsonParser, RejectsOversizedUrlsInsteadOfTruncating) {
  OpdsJsonParser parser;
  feed(parser, "{\"navigation\":[{\"title\":\"Bad\",\"href\":\"" + std::string(5000, 'a') + "\"}]}");
  EXPECT_FALSE(parser.finish());
}

TEST(OpdsJsonParser, DoesNotRetainPublicationsAcrossLargeFeeds) {
  OpdsJsonParser parser;
  unsigned count = 0;
  parser.onEntryParsed = [&](OpdsEntry) { ++count; };
  feed(parser, "{\"publications\":[");
  for (unsigned i = 0; i < 5000; ++i) {
    if (i) feed(parser, ",");
    feed(parser,
         R"({"links":[{"type":"application/epub+zip","rel":"acquisition","href":"a.epub"}],"metadata":{"title":"A"}})",
         97);
  }
  feed(parser, "]}");
  EXPECT_TRUE(parser.finish());
  EXPECT_EQ(count, 5000u);
}

TEST(OpdsJsonParser, TruncatesLongDescriptionsAndCarriesProgressPercent) {
  OpdsJsonParser parser;
  OpdsEntry result;
  parser.onEntryParsed = [&](OpdsEntry entry) { result = std::move(entry); };
  feed(parser, "{\"publications\":[{\"metadata\":{\"title\":\"A\",\"https://bookorbit.app/ns/"
               "readingProgress\":0.42,\"description\":\"" +
                   std::string(10000, 'a') + "\"}}]}");
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(result.progressPercent, 42);
  EXPECT_LE(result.summary.size(), 4095u);
}

TEST(OpdsJsonParser, ExposesFacetLinksAsNavigableEntries) {
  OpdsJsonParser parser;
  std::vector<OpdsEntry> entries;
  parser.onEntryParsed = [&](OpdsEntry entry) { entries.push_back(std::move(entry)); };
  feed(
      parser,
      R"({"metadata":{"title":"Books"},"publications":[],"facets":[{"metadata":{"title":"Sort"},"links":[{"title":"Recent","href":"?sort=recent","type":"application/opds+json"}]}]})");
  ASSERT_TRUE(parser.finish());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].title, "Recent");
  EXPECT_EQ(entries[0].href, "?sort=recent");
}
