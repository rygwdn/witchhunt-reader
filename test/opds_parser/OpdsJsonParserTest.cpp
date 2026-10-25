#include <gtest/gtest.h>

#include "../../src/network/OpdsSyncPolicy.h"
#include "OpdsJsonParser.h"

namespace {
void feed(OpdsJsonParser& parser, const std::string& body, size_t chunk = 1) {
  for (size_t i = 0; i < body.size(); i += chunk)
    parser.feed(reinterpret_cast<const uint8_t*>(body.data() + i), std::min(chunk, body.size() - i));
}
}  // namespace

TEST(OpdsJsonParser, StreamsDashboardWithSeriesProgressionAndUnicode) {
  OpdsJsonParser parser;
  std::vector<OpdsEntry> entries;
  parser.onEntryParsed = [&](OpdsEntry entry) { entries.push_back(std::move(entry)); };
  feed(
      parser,
      R"({"metadata":{"title":"Dashboard"},"links":[{"rel":["next"],"href":"?page=2"},{"rel":"search","href":"search{?query}","type":"application/opds+json"}],"groups":[{"metadata":{"title":"Up Next"},"publications":[{"metadata":{"title":"Caf\u00e9 \ud83d\udcd6","identifier":"urn:book:7","author":[{"name":"Ada"},"Bob"],"belongsTo":{"series":{"name":"Saga","position":2,"links":[{"href":"/series/9"}]}}},"images":[{"href":"/cover.jpg","type":"image/jpeg"}],"links":[{"href":"/book.epub","rel":["acquisition"],"type":"application/epub+zip"},{"rel":"http://opds-spec.org/progression","href":"/progression","type":"application/opds-progression+json"}]}]},{"metadata":{"title":"Browse"},"navigation":[{"title":"Series","href":"/series","type":"application/opds+json"}]}]})");
  ASSERT_TRUE(parser.finish());
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].title, "Café 📖");
  EXPECT_EQ(entries[0].author, "Ada, Bob");
  EXPECT_EQ(entries[0].groupTitle, "Up Next");
  EXPECT_EQ(entries[0].seriesTitle, "Saga #2");
  EXPECT_EQ(entries[0].seriesHref, "/series/9");
  EXPECT_EQ(entries[0].progressionHref, "/progression");
  ASSERT_EQ(entries[0].acquisitionLinks.size(), 1u);
  EXPECT_EQ(entries[0].imageHref, "/cover.jpg");
  EXPECT_EQ(entries[1].groupTitle, "Browse");
  EXPECT_EQ(parser.getNextPageUrl(), "?page=2");
  EXPECT_EQ(parser.getSearchTemplate(), "search{?query}");
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

TEST(OpdsJsonParser, TruncatesLongDescriptionsButKeepsFileProgressionHints) {
  OpdsJsonParser parser;
  OpdsEntry result;
  parser.onEntryParsed = [&](OpdsEntry entry) { result = std::move(entry); };
  feed(
      parser,
      "{\"publications\":[{\"metadata\":{\"title\":\"A\",\"https://bookorbit.app/ns/"
      "readingProgress\":0.42,\"description\":\"" +
          std::string(10000, 'a') +
          R"("},"links":[{"href":"a.epub","rel":"acquisition","type":"application/epub+zip","properties":{"https://bookorbit.app/ns/progression":"/progress?fileId=12"}}]}]})");
  ASSERT_TRUE(parser.finish());
  EXPECT_EQ(result.progressPercent, 42);
  EXPECT_LE(result.summary.size(), 4095u);
  ASSERT_EQ(result.acquisitionLinks.size(), 1u);
  EXPECT_EQ(result.acquisitionLinks[0].progressionHref, "/progress?fileId=12");
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

TEST(OpdsJsonParser, DiscoversStatsEndpointAndKeepsFileHintsSeparate) {
  OpdsJsonParser parser;
  std::vector<OpdsEntry> entries;
  parser.onEntryParsed = [&](OpdsEntry entry) { entries.push_back(std::move(entry)); };
  feed(
      parser,
      R"({"publications":[{"metadata":{"title":"Book"},"links":[{"rel":"https://bookorbit.app/rel/reading-stats","type":"application/vnd.bookorbit.reading-stats+json","href":"/stats"},{"rel":"acquisition","type":"application/epub+zip","href":"/book.epub","properties":{"https://bookorbit.app/ns/reading-stats":"/stats?fileId=12"}},{"rel":"acquisition","type":"text/plain","href":"/book.txt","properties":{"https://bookorbit.app/ns/reading-stats":"/stats?fileId=13"}}]}]})");
  ASSERT_TRUE(parser.finish());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].statsHref, "/stats");
  ASSERT_EQ(entries[0].acquisitionLinks.size(), 2u);
  EXPECT_EQ(entries[0].acquisitionLinks[0].statsHref, "/stats?fileId=12");
  EXPECT_EQ(entries[0].acquisitionLinks[1].statsHref, "/stats?fileId=13");
}

TEST(OpdsSyncPolicy, CompletionSurvivesOlderReaderCloseAndSameMillisecondReplay) {
  EXPECT_FALSE(OpdsSyncPolicy::replacesProgress("2026-01-01T12:00:01.000Z", 1.0f, "2026-01-01T12:00:00.999Z", 0.99f));
  EXPECT_FALSE(OpdsSyncPolicy::replacesProgress("2026-01-01T12:00:01.000Z", 1.0f, "2026-01-01T12:00:01.000Z", 0.99f));
  EXPECT_TRUE(OpdsSyncPolicy::replacesProgress("2026-01-01T12:00:01.000Z", 0.99f, "2026-01-01T12:00:01.000Z", 1.0f));
  EXPECT_TRUE(OpdsSyncPolicy::replacesProgress("2026-01-01T12:00:01.000Z", 1.0f, "2026-01-02T12:00:00.000Z", 0.1f));
}

TEST(OpdsSyncPolicy, OpdsAutomaticSyncDoesNotRequireKoReaderCredentials) {
  EXPECT_TRUE(OpdsSyncPolicy::automaticEnabled(false, true, true));
  EXPECT_TRUE(OpdsSyncPolicy::automaticEnabled(true, false, true));
  EXPECT_FALSE(OpdsSyncPolicy::automaticEnabled(false, false, true));
  EXPECT_FALSE(OpdsSyncPolicy::automaticEnabled(true, true, false));
}
TEST(OpdsSyncPolicy, InFlightUploadCannotAcknowledgeNewerQueuedPageOrCompletion) {
  EXPECT_FALSE(
      OpdsSyncPolicy::matchesQueuedProgress("2026-01-01T12:00:00.000Z", 0.4f, "2026-01-01T12:00:01.000Z", 0.5f));
  EXPECT_FALSE(
      OpdsSyncPolicy::matchesQueuedProgress("2026-01-01T12:00:00.000Z", 0.99f, "2026-01-01T12:00:00.000Z", 1.0f));
  EXPECT_TRUE(
      OpdsSyncPolicy::matchesQueuedProgress("2026-01-01T12:00:00.000Z", 0.4f, "2026-01-01T12:00:00.000Z", 0.4f));
}

TEST(OpdsSyncPolicy, WakePullToleratesLocalPageRoundingWithoutHidingADifferentPage) {
  EXPECT_TRUE(OpdsSyncPolicy::sameReadingPage(0.495f, 0.505f, 0.5f, 0.001f));
  EXPECT_FALSE(OpdsSyncPolicy::sameReadingPage(0.495f, 0.505f, 0.48f, 0.001f));
  EXPECT_FALSE(OpdsSyncPolicy::sameReadingPage(0.495f, 0.505f, 0.52f, 0.001f));
}
