#include <BookOrbitCatalogSettings.h>
#include <BookOrbitCatalogSnapshotParser.h>
#include <BookOrbitCatalogStore.h>
#include <HalStorage.h>
#include <KOReaderCredentialStore.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {
bool failBuild = false;
bool failSettingsLoad = false;
bool incompletePage = false;
bool omitMembershipForBookOne = false;
bool emitUnselectedMembershipForBookThree = false;
bool failSnapshotOnPageTwo = false;
bool changeGenerationOnPageTwo = false;
bool repeatNextCursor = false;
uint32_t calls = 0;
uint32_t bookCount = 2101;
std::vector<int32_t> continueReadingBooks = {1, 1, 2, 2101};
std::vector<int32_t> discoverBooks = {2, 3, 2101};
std::map<int32_t, std::string> formatOverrides;
std::map<int32_t, int16_t> progressOverrides;
std::map<std::string, std::vector<int32_t>> scopedBooks;
std::vector<std::string> requestedQueries;
BookOrbitCatalogSettings savedSettings;

struct SourceFixture {
  BookOrbitCatalogSettings::SourceKind kind;
  int32_t id;
  std::string title;
  int32_t bookCount;
};

const char* sourceIdKey(BookOrbitCatalogSettings::SourceKind kind) {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library:
      return "libraryId";
    case BookOrbitCatalogSettings::SourceKind::Collection:
      return "collectionId";
    case BookOrbitCatalogSettings::SourceKind::SmartScope:
      return "smartScopeId";
  }
  return "";
}

void resetSettings() {
  savedSettings.supportedOnly = true;
  savedSettings.scope = BookOrbitCatalogSettings::Scope::All;
  savedSettings.sources.clear();
  failSettingsLoad = false;
}

void resetCatalogData() {
  failBuild = false;
  incompletePage = false;
  omitMembershipForBookOne = false;
  emitUnselectedMembershipForBookThree = false;
  failSnapshotOnPageTwo = false;
  changeGenerationOnPageTwo = false;
  repeatNextCursor = false;
  calls = 0;
  bookCount = 2101;
  continueReadingBooks = {1, 1, 2, 2101};
  discoverBooks = {2, 3, 2101};
  formatOverrides.clear();
  progressOverrides.clear();
  scopedBooks.clear();
  requestedQueries.clear();
  resetSettings();
}

BookOrbitEntry makeBook(int32_t id) {
  BookOrbitEntry entry;
  entry.type = BookOrbitEntryType::BOOK;
  entry.bookId = id;
  entry.title = "Book " + std::to_string(id);
  entry.subtitle = "Author " + std::to_string(id % 2);
  entry.series = "Series " + std::to_string(id % 2);
  entry.href = "books/" + std::to_string(id);
  const auto format = formatOverrides.find(id);
  entry.formats = format == formatOverrides.end() ? "EPUB" : format->second;
  const auto progress = progressOverrides.find(id);
  if (progress != progressOverrides.end()) entry.progressPercent = progress->second;
  return entry;
}

std::string queryValue(const std::string& query, const char* key) {
  const std::string marker = std::string(key) + "=";
  const size_t question = query.find('?');
  size_t start = question == std::string::npos ? 0 : question + 1;
  while (start < query.size()) {
    const size_t end = query.find('&', start);
    const size_t finish = end == std::string::npos ? query.size() : end;
    if (query.compare(start, marker.size(), marker) == 0)
      return query.substr(start + marker.size(), finish - start - marker.size());
    start = finish + 1;
  }
  return {};
}

uint32_t requestedPageIndex(const std::string& query) {
  const std::string cursor = queryValue(query, "cursor");
  if (cursor.size() < 2 || cursor[0] != 'p') return 0;
  return static_cast<uint32_t>(std::stoul(cursor.substr(1)));
}

const std::vector<int32_t>& sourceBookIds(BookOrbitCatalogSettings::SourceKind kind, int32_t id) {
  static const std::vector<int32_t> empty;
  const std::string key = std::string(sourceIdKey(kind)) + "=" + std::to_string(id);
  const auto found = scopedBooks.find(key);
  return found == scopedBooks.end() ? empty : found->second;
}

std::vector<SourceFixture> snapshotSources() {
  std::vector<SourceFixture> sources;
  if (savedSettings.scope == BookOrbitCatalogSettings::Scope::Selected) {
    sources.reserve(savedSettings.sources.size());
    for (const auto& source : savedSettings.sources) {
      sources.push_back(
          {source.kind, source.id, source.title, static_cast<int32_t>(sourceBookIds(source.kind, source.id).size())});
    }
    return sources;
  }
  sources = {{BookOrbitCatalogSettings::SourceKind::Library, 1, "Library One", 0},
             {BookOrbitCatalogSettings::SourceKind::Library, 2, "Library Two", 0},
             {BookOrbitCatalogSettings::SourceKind::Collection, 5, "Collection Five", 0},
             {BookOrbitCatalogSettings::SourceKind::SmartScope, 9, "Recent Nine", 0}};
  return sources;
}

std::vector<int32_t> snapshotBooks() {
  std::vector<int32_t> ids;
  if (savedSettings.scope == BookOrbitCatalogSettings::Scope::Selected) {
    for (const auto& source : savedSettings.sources) {
      const auto& sourceIds = sourceBookIds(source.kind, source.id);
      ids.insert(ids.end(), sourceIds.begin(), sourceIds.end());
    }
  } else {
    ids.reserve(bookCount);
    for (uint32_t id = 1; id <= bookCount; ++id) ids.push_back(static_cast<int32_t>(id));
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  if (savedSettings.supportedOnly) {
    ids.erase(
        std::remove_if(ids.begin(), ids.end(),
                       [](int32_t id) { return !BookOrbitCatalogSettings::supportsFormats(makeBook(id).formats); }),
        ids.end());
  }
  return ids;
}

bool emitSources(const BookOrbitSnapshotCallbacks& callbacks, const std::vector<SourceFixture>& sources) {
  for (const auto& source : sources)
    if (!callbacks.onSource(callbacks.context, source.kind, source.id, source.title, source.bookCount)) return false;
  return true;
}

}  // namespace

bool BookOrbitCatalogSettings::load() {
  if (failSettingsLoad) return false;
  supportedOnly = savedSettings.supportedOnly;
  scope = savedSettings.scope;
  sources = savedSettings.sources;
  return true;
}

int BookOrbitCatalogClient::lastHttpCode = 0;

BookOrbitCatalogClient::Error BookOrbitCatalogClient::fetchSnapshot(const std::string& query,
                                                                    const BookOrbitSnapshotCallbacks& callbacks,
                                                                    BookOrbitSnapshotPage& page) {
  ++calls;
  requestedQueries.push_back(query);
  if (failBuild) return BookOrbitCatalogClient::NETWORK_ERROR;

  const uint32_t pageIndex = requestedPageIndex(query);
  if (failSnapshotOnPageTwo && pageIndex == 1) return BookOrbitCatalogClient::NETWORK_ERROR;
  const std::vector<int32_t> ids = snapshotBooks();
  const std::vector<SourceFixture> sources = snapshotSources();
  const uint32_t start = pageIndex * 100;
  const uint32_t end = std::min<uint32_t>(static_cast<uint32_t>(ids.size()), start + 100);

  page = {};
  page.version = 1;
  page.total = static_cast<int32_t>(ids.size());
  page.generation = changeGenerationOnPageTwo && pageIndex == 1 ? "changed-generation" : "frozen-generation";
  page.etag = "fixture-etag";
  if (end < ids.size()) {
    page.nextCursor = repeatNextCursor && pageIndex > 0 ? "p1" : "p" + std::to_string(pageIndex + 1);
  }

  for (const int32_t id : continueReadingBooks)
    if (!callbacks.onShelf(callbacks.context, BookOrbitEntryGroup::CONTINUE_READING, id))
      return BookOrbitCatalogClient::JSON_ERROR;
  for (const int32_t id : discoverBooks)
    if (!callbacks.onShelf(callbacks.context, BookOrbitEntryGroup::DISCOVER, id))
      return BookOrbitCatalogClient::JSON_ERROR;

  for (uint32_t index = start; index < end; ++index) {
    const int32_t id = ids[index];
    if (incompletePage && index == start + 99) continue;
    const BookOrbitEntry entry = makeBook(id);
    if (!callbacks.onBook(callbacks.context, entry)) return BookOrbitCatalogClient::JSON_ERROR;
    if (!omitMembershipForBookOne || id != 1) {
      for (const auto& source : sources) {
        const auto& members = sourceBookIds(source.kind, source.id);
        if (std::find(members.begin(), members.end(), id) != members.end() &&
            !callbacks.onMembership(callbacks.context, id, source.kind, source.id))
          return BookOrbitCatalogClient::JSON_ERROR;
      }
    }
    if (emitUnselectedMembershipForBookThree && id == 3 &&
        !callbacks.onMembership(callbacks.context, id, BookOrbitCatalogSettings::SourceKind::Library, 99))
      return BookOrbitCatalogClient::JSON_ERROR;
  }
  if (!emitSources(callbacks, sources)) return BookOrbitCatalogClient::JSON_ERROR;
  return BookOrbitCatalogClient::OK;
}

class CatalogStoreSnapshot : public testing::Test {
 protected:
  void SetUp() override {
    Storage.clear();
    KOREADER_STORE.set("https://bookorbit.test", "reader");
    resetCatalogData();
  }
};

TEST(OfflineCatalogFormats, AcceptsOnlyExactReadableTokens) {
  EXPECT_TRUE(BookOrbitCatalogSettings::supportsFormats("PDF · EPUB"));
  EXPECT_TRUE(BookOrbitCatalogSettings::supportsFormats("archive · XtC · ZIP"));
  EXPECT_TRUE(BookOrbitCatalogSettings::supportsFormats("xtch"));
  EXPECT_TRUE(BookOrbitCatalogSettings::supportsFormats("TXT"));
  EXPECT_TRUE(BookOrbitCatalogSettings::supportsFormats("other · md"));
  EXPECT_FALSE(BookOrbitCatalogSettings::supportsFormats("PDF · MOBI"));
  EXPECT_FALSE(BookOrbitCatalogSettings::supportsFormats("epubx"));
  EXPECT_FALSE(BookOrbitCatalogSettings::supportsFormats(""));
}

TEST_F(CatalogStoreSnapshot, SnapshotPagesBuildCanonicalOfflineCatalogAndNavigation) {
  progressOverrides[1] = 42;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  EXPECT_TRUE(BookOrbitCatalogStore::available());
  EXPECT_EQ(calls, 22u);
  EXPECT_EQ(requestedQueries.front(), "scope=all&size=100&formats=epub,xtc,xtch,txt,md");
  EXPECT_EQ(queryValue(requestedQueries[1], "cursor"), "p1");

  BookOrbitEntry entry;
  ASSERT_TRUE(BookOrbitCatalogStore::findBook(2101, entry));
  EXPECT_EQ(entry.title, "Book 2101");
  std::vector<int32_t> ids;
  ids.reserve(100);
  BookOrbitPageInfo page;
  ASSERT_TRUE(BookOrbitCatalogStore::search(
      "Book", 176, [&](const BookOrbitEntry& book) { ids.push_back(book.bookId); }, page));
  ASSERT_EQ(ids, (std::vector<int32_t>{2101}));
  EXPECT_EQ(page.total, 2101);

  ids.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "books?sort=title&page=1&size=100", [&](const BookOrbitEntry& book) { ids.push_back(book.bookId); }, page));
  std::vector<int32_t> expected;
  expected.reserve(100);
  for (int32_t id = 1; id <= 100; ++id) expected.push_back(id);
  EXPECT_EQ(ids, expected);
  EXPECT_EQ(page.total, 2101);
  EXPECT_EQ(page.page, 1);
  ASSERT_FALSE(page.nextUrl.empty());
  ids.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      page.nextUrl, [&](const BookOrbitEntry& book) { ids.push_back(book.bookId); }, page));
  ASSERT_EQ(ids.size(), 100u);
  EXPECT_EQ(ids.front(), 101);
  EXPECT_EQ(ids.back(), 200);
  EXPECT_EQ(page.total, 2101);
  EXPECT_EQ(page.page, 2);

  std::vector<std::string> navigation;
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/libraries", [&](const BookOrbitEntry& item) { navigation.push_back(item.title); }, page));
  EXPECT_EQ(navigation, (std::vector<std::string>{"Library One", "Library Two"}));
  navigation.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/authors", [&](const BookOrbitEntry& item) { navigation.push_back(item.title); }, page));
  EXPECT_EQ(navigation, (std::vector<std::string>{"Author 1", "Author 0"}));
  EXPECT_EQ(page.total, 2);
  navigation.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/series", [&](const BookOrbitEntry& item) { navigation.push_back(item.title); }, page));
  EXPECT_EQ(navigation, (std::vector<std::string>{"Series 1", "Series 0"}));

  std::vector<BookOrbitEntry> dashboard;
  std::vector<std::string> rootTitles;
  ASSERT_TRUE(BookOrbitCatalogStore::dashboard([&](const BookOrbitEntry& item) {
    if (item.type == BookOrbitEntryType::BOOK)
      dashboard.push_back(item);
    else
      rootTitles.push_back(item.title);
  }));
  EXPECT_EQ(rootTitles, (std::vector<std::string>{"Libraries", "Collections", "Smart scopes", "Authors", "Series"}));
  ASSERT_EQ(dashboard.size(), 6u);
  EXPECT_EQ(dashboard[0].bookId, 1);
  EXPECT_EQ(dashboard[0].title, "Book 1");
  EXPECT_EQ(dashboard[0].progressPercent, 42);
  EXPECT_EQ(dashboard[0].group, BookOrbitEntryGroup::CONTINUE_READING);
  EXPECT_EQ(dashboard[2].bookId, 2101);
  EXPECT_EQ(dashboard[3].bookId, 2);
  EXPECT_EQ(dashboard[3].group, BookOrbitEntryGroup::DISCOVER);
}

TEST_F(CatalogStoreSnapshot, SupportedFormatUnionUsesExactlyReadableWireFilters) {
  bookCount = 6;
  formatOverrides = {{1, "PDF"}, {2, "EPUB"}, {3, "XTC"}, {4, "XTCH"}, {5, "TXT"}, {6, "MD"}};
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  ASSERT_EQ(requestedQueries.size(), 1u);
  EXPECT_EQ(requestedQueries[0], "scope=all&size=100&formats=epub,xtc,xtch,txt,md");
  BookOrbitEntry entry;
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(1, entry));
  for (int32_t id = 2; id <= 6; ++id) EXPECT_TRUE(BookOrbitCatalogStore::findBook(id, entry));

  savedSettings.supportedOnly = false;
  requestedQueries.clear();
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  EXPECT_EQ(requestedQueries[0], "scope=all&size=100");
  for (int32_t id = 1; id <= 6; ++id) EXPECT_TRUE(BookOrbitCatalogStore::findBook(id, entry));
}

TEST_F(CatalogStoreSnapshot, SelectedScopeUnionsSourcesAndCountsMembershipRows) {
  savedSettings.scope = BookOrbitCatalogSettings::Scope::Selected;
  savedSettings.sources = {{BookOrbitCatalogSettings::SourceKind::Library, 1, "Library One"},
                           {BookOrbitCatalogSettings::SourceKind::Library, 2, "Library Two"},
                           {BookOrbitCatalogSettings::SourceKind::Collection, 5, "Collection Five"},
                           {BookOrbitCatalogSettings::SourceKind::SmartScope, 9, "Recent Nine"}};
  scopedBooks["libraryId=1"] = {1, 2, 3, 4};
  scopedBooks["libraryId=2"] = {3, 6};
  scopedBooks["collectionId=5"] = {3, 5};
  scopedBooks["smartScopeId=9"] = {3, 7};
  emitUnselectedMembershipForBookThree = true;
  formatOverrides = {{2, "PDF"}, {4, "PDF"}, {6, "PDF"}, {7, "PDF"}};
  progressOverrides[1] = 42;
  continueReadingBooks = {1, 2, 3};
  discoverBooks = {3, 5};

  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  ASSERT_EQ(requestedQueries.size(), 1u);
  EXPECT_EQ(requestedQueries[0],
            "scope=selected&size=100&formats=epub,xtc,xtch,txt,md&libraryIds=1,2&collectionIds=5&smartScopeIds=9");

  BookOrbitEntry entry;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(1, entry));
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(5, entry));
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(2, entry));
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(4, entry));

  BookOrbitPageInfo page;
  std::vector<int32_t> ids;
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "books?libraryId=1&sort=title&size=100", [&](const BookOrbitEntry& book) { ids.push_back(book.bookId); }, page));
  EXPECT_EQ(ids, (std::vector<int32_t>{1, 3}));
  EXPECT_EQ(page.total, 2);
  ids.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "books?libraryId=2&sort=title&size=100", [&](const BookOrbitEntry& book) { ids.push_back(book.bookId); }, page));
  EXPECT_EQ(ids, (std::vector<int32_t>{3}));
  EXPECT_EQ(page.total, 1);

  std::vector<std::string> names;
  std::vector<int32_t> counts;
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/libraries",
      [&](const BookOrbitEntry& source) {
        names.push_back(source.title);
        counts.push_back(source.count);
      },
      page));
  EXPECT_EQ(names, (std::vector<std::string>{"Library One", "Library Two"}));
  EXPECT_EQ(counts, (std::vector<int32_t>{2, 1}));
  names.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/collections", [&](const BookOrbitEntry& source) { names.push_back(source.title); }, page));
  EXPECT_EQ(names, (std::vector<std::string>{"Collection Five"}));
  names.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/smart-scopes", [&](const BookOrbitEntry& source) { names.push_back(source.title); }, page));
  EXPECT_EQ(names, (std::vector<std::string>{"Recent Nine"}));

  std::vector<BookOrbitEntry> shelf;
  ASSERT_TRUE(BookOrbitCatalogStore::dashboard([&](const BookOrbitEntry& item) {
    if (item.type == BookOrbitEntryType::BOOK) shelf.push_back(item);
  }));
  ASSERT_EQ(shelf.size(), 4u);
  EXPECT_EQ(shelf[0].bookId, 1);
  EXPECT_EQ(shelf[0].title, "Book 1");
  EXPECT_EQ(shelf[0].progressPercent, 42);
  EXPECT_EQ(shelf[0].group, BookOrbitEntryGroup::CONTINUE_READING);
  EXPECT_EQ(shelf[1].bookId, 3);
  EXPECT_EQ(shelf[2].bookId, 3);
  EXPECT_EQ(shelf[3].bookId, 5);
}

TEST_F(CatalogStoreSnapshot, SelectedScopeRequiresMembershipForEveryBook) {
  savedSettings.scope = BookOrbitCatalogSettings::Scope::Selected;
  savedSettings.sources = {{BookOrbitCatalogSettings::SourceKind::Library, 1, "Library One"}};
  scopedBooks["libraryId=1"] = {1};
  omitMembershipForBookOne = true;
  EXPECT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::JSON_ERROR);
  EXPECT_FALSE(BookOrbitCatalogStore::available());
}

TEST_F(CatalogStoreSnapshot, DashboardRetainsEveryServerBoundedShelfEntry) {
  bookCount = 12;
  continueReadingBooks = {1, 2, 3, 4, 5};
  discoverBooks.clear();
  for (int32_t id = 1; id <= 12; ++id) discoverBooks.push_back(id);
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);

  uint8_t continueCount = 0;
  uint8_t discoverCount = 0;
  ASSERT_TRUE(BookOrbitCatalogStore::dashboard([&](const BookOrbitEntry& item) {
    if (item.type != BookOrbitEntryType::BOOK) return;
    if (item.group == BookOrbitEntryGroup::CONTINUE_READING)
      ++continueCount;
    else if (item.group == BookOrbitEntryGroup::DISCOVER)
      ++discoverCount;
  }));
  EXPECT_EQ(continueCount, 5);
  EXPECT_EQ(discoverCount, 12);
}

TEST_F(CatalogStoreSnapshot, SourceMembershipAndCanonicalCatalogPageBeyondOneHundred) {
  savedSettings.scope = BookOrbitCatalogSettings::Scope::Selected;
  savedSettings.sources = {{BookOrbitCatalogSettings::SourceKind::Library, 1, "Library One"}};
  auto& books = scopedBooks["libraryId=1"];
  books.reserve(125);
  for (int32_t id = 1; id <= 125; ++id) books.push_back(id);
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);

  BookOrbitPageInfo page;
  std::vector<int32_t> ids;
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "books?libraryId=1&sort=title&size=100", [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  ASSERT_EQ(ids.size(), 100u);
  EXPECT_EQ(page.total, 125);
  ASSERT_FALSE(page.nextUrl.empty());
  ids.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      page.nextUrl, [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_EQ(ids.size(), 25u);
  EXPECT_EQ(ids.front(), 101);
  EXPECT_EQ(ids.back(), 125);
  EXPECT_EQ(page.total, 125);

  ids.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "books?sort=title&size=100", [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_EQ(ids.size(), 100u);
  EXPECT_EQ(page.total, 125);
  BookOrbitEntry last;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(125, last));
}

TEST_F(CatalogStoreSnapshot, SelectedScopeWithNoSourcesPublishesAnEmptyCatalog) {
  savedSettings.scope = BookOrbitCatalogSettings::Scope::Selected;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  ASSERT_EQ(requestedQueries.size(), 1u);
  EXPECT_EQ(requestedQueries[0], "scope=selected&size=100&formats=epub,xtc,xtch,txt,md");
  BookOrbitEntry entry;
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(1, entry));
  BookOrbitPageInfo page;
  std::vector<int32_t> ids;
  ASSERT_TRUE(
      BookOrbitCatalogStore::search("Book", 1, [&](const BookOrbitEntry& book) { ids.push_back(book.bookId); }, page));
  EXPECT_TRUE(ids.empty());
  EXPECT_EQ(page.total, 0);
  std::vector<BookOrbitEntry> dashboard;
  ASSERT_TRUE(BookOrbitCatalogStore::dashboard([&](const BookOrbitEntry& item) { dashboard.push_back(item); }));
  EXPECT_TRUE(std::none_of(dashboard.begin(), dashboard.end(),
                           [](const BookOrbitEntry& item) { return item.type == BookOrbitEntryType::BOOK; }));
}

TEST_F(CatalogStoreSnapshot, LocalAuthorAndSeriesAggregatesUseIncludedCanonicalRows) {
  savedSettings.scope = BookOrbitCatalogSettings::Scope::Selected;
  savedSettings.sources = {{BookOrbitCatalogSettings::SourceKind::Library, 1, "Library One"}};
  scopedBooks["libraryId=1"] = {1, 2, 3};
  formatOverrides = {{1, "EPUB"}, {2, "TXT"}, {3, "PDF"}};
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  BookOrbitPageInfo page;
  std::vector<std::string> names;
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/authors", [&](const BookOrbitEntry& item) { names.push_back(item.title); }, page));
  EXPECT_EQ(names, (std::vector<std::string>{"Author 1", "Author 0"}));
  EXPECT_EQ(page.total, 2);
  std::vector<int32_t> ids;
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "@offline:author:1:Author 1", [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_EQ(ids, (std::vector<int32_t>{1}));
  names.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "sections/series", [&](const BookOrbitEntry& item) { names.push_back(item.title); }, page));
  EXPECT_EQ(names, (std::vector<std::string>{"Series 1", "Series 0"}));
  ids.clear();
  ASSERT_TRUE(BookOrbitCatalogStore::listing(
      "@offline:series-books:1:Series 0", [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_EQ(ids, (std::vector<int32_t>{2}));
}

TEST_F(CatalogStoreSnapshot, FailedRefreshPreservesBooksAndShelvesFromPublishedSnapshot) {
  bookCount = 3;
  continueReadingBooks = {1};
  discoverBooks = {2};
  progressOverrides[1] = 42;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);

  bookCount = 150;
  failSnapshotOnPageTwo = true;
  EXPECT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::NETWORK_ERROR);
  BookOrbitEntry entry;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(4, entry));
  std::vector<int32_t> shelf;
  ASSERT_TRUE(BookOrbitCatalogStore::dashboard([&](const BookOrbitEntry& item) {
    if (item.type == BookOrbitEntryType::BOOK) shelf.push_back(item.bookId);
  }));
  EXPECT_EQ(shelf, (std::vector<int32_t>{1, 2}));
  ASSERT_TRUE(BookOrbitCatalogStore::findBook(1, entry));
  EXPECT_EQ(entry.progressPercent, 42);
}

TEST_F(CatalogStoreSnapshot, FailedCacheFileCreationPreservesPublishedBooksAndAllowsRetry) {
  constexpr const char* paths[] = {
      "/.crosspoint/bookorbit/catalog.ids.work",
      "/.crosspoint/bookorbit/source-library.ids.work",
      "/.crosspoint/bookorbit/source-collection.ids.work",
      "/.crosspoint/bookorbit/source-smart-scope.ids.work",
      "/.crosspoint/bookorbit/catalog.tmp",
      "/.crosspoint/bookorbit/catalog.ids.tmp",
      "/.crosspoint/bookorbit/catalog.ids.counts.work",
  };
  for (const char* path : paths) {
    SCOPED_TRACE(path);
    bookCount = 3;
    ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
    bookCount = 6;
    Storage.failWritesTo(path);
    EXPECT_NE(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
    BookOrbitEntry entry;
    ASSERT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
    EXPECT_EQ(entry.title, "Book 3");
    EXPECT_FALSE(BookOrbitCatalogStore::findBook(4, entry));
    Storage.failWritesTo("");
    ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
    ASSERT_TRUE(BookOrbitCatalogStore::findBook(6, entry));
    EXPECT_EQ(entry.title, "Book 6");
  }
}

TEST_F(CatalogStoreSnapshot, FrozenGenerationMismatchCannotReplacePublishedSnapshot) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  bookCount = 150;
  changeGenerationOnPageTwo = true;
  EXPECT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::JSON_ERROR);
  BookOrbitEntry entry;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(4, entry));
}

TEST_F(CatalogStoreSnapshot, CursorCycleCannotReplacePublishedSnapshot) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  bookCount = 301;
  repeatNextCursor = true;
  EXPECT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::JSON_ERROR);
  BookOrbitEntry entry;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(4, entry));
}

TEST_F(CatalogStoreSnapshot, SettingsLoadFailurePreservesPublishedSnapshot) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  failSettingsLoad = true;
  EXPECT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::JSON_ERROR);
  failSettingsLoad = false;
  BookOrbitEntry entry;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
}

TEST_F(CatalogStoreSnapshot, RecoversValidBackupWhenLiveFileIsCorrupt) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  ASSERT_TRUE(Storage.rename("/.crosspoint/bookorbit/catalog.bin", "/.crosspoint/bookorbit/catalog.bak"));
  Storage.put("/.crosspoint/bookorbit/catalog.bin", std::vector<uint8_t>{0, 1, 2});
  EXPECT_TRUE(BookOrbitCatalogStore::available());
  BookOrbitEntry entry;
  EXPECT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
}

TEST_F(CatalogStoreSnapshot, EmptyLibraryReplacesPreviouslyPopulatedSnapshot) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  bookCount = 0;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  BookOrbitEntry entry;
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(3, entry));
  BookOrbitPageInfo page;
  std::vector<int32_t> ids;
  ASSERT_TRUE(
      BookOrbitCatalogStore::search("Book", 1, [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_TRUE(ids.empty());
  EXPECT_EQ(page.total, 0);
  EXPECT_TRUE(page.nextUrl.empty());
}

TEST_F(CatalogStoreSnapshot, IncompleteSnapshotPageCannotReplaceCompleteSnapshot) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  bookCount = 150;
  incompletePage = true;
  EXPECT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::JSON_ERROR);
  BookOrbitEntry entry;
  ASSERT_TRUE(BookOrbitCatalogStore::findBook(3, entry));
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(4, entry));
}

TEST_F(CatalogStoreSnapshot, AccountChangesHidePublishedCache) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  KOREADER_STORE.set("https://bookorbit.test", "different-reader");
  EXPECT_FALSE(BookOrbitCatalogStore::available());
  BookOrbitEntry entry;
  EXPECT_FALSE(BookOrbitCatalogStore::findBook(3, entry));
}

TEST_F(CatalogStoreSnapshot, CancellingStagedRefreshKeepsPublishedBooksAndAllowsAnotherRefresh) {
  bookCount = 3;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  bookCount = 150;
  ASSERT_TRUE(BookOrbitCatalogStore::beginRebuild());
  BookOrbitCatalogStore::RebuildProgress progress;
  while (progress.completedPages < 2)
    ASSERT_EQ(BookOrbitCatalogStore::rebuildStep(&progress), BookOrbitCatalogStore::RebuildStatus::InProgress);
  BookOrbitCatalogStore::cancelRebuild();
  EXPECT_EQ(BookOrbitCatalogStore::rebuildStep(), BookOrbitCatalogStore::RebuildStatus::Cancelled);

  BookOrbitPageInfo page;
  std::vector<int32_t> ids;
  ASSERT_TRUE(
      BookOrbitCatalogStore::search("", 1, [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_EQ(ids, (std::vector<int32_t>{1, 2, 3}));
  EXPECT_EQ(page.total, 3);

  bookCount = 5;
  ASSERT_EQ(BookOrbitCatalogStore::rebuild(nullptr, nullptr), BookOrbitCatalogClient::OK);
  ids.clear();
  ASSERT_TRUE(
      BookOrbitCatalogStore::search("", 1, [&](const BookOrbitEntry& item) { ids.push_back(item.bookId); }, page));
  EXPECT_EQ(ids, (std::vector<int32_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(page.total, 5);
}
