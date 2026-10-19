#include <gtest/gtest.h>

#include <Epub.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include "esp_heap_caps.h"
#include <HalStorage.h>
#include "../../lib/BookOrbitCatalog/BookOrbitCatalogClient.h"
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "BookOrbitBookmarkSync.h"
#include "GlobalBookmarkIndex.h"
#include "ScriptedTransport.h"

namespace fs = std::filesystem;

namespace {
bool hostClockSynced = false;
time_t hostClockNow = 0;

const std::string kFixture = std::string(CORPUS_DIR) + "/test_wrapped_paragraphs.epub";

std::string testRoot() {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return (fs::temp_directory_path() / ("bookorbit_bookmark_sync_smoke_" + std::to_string(stamp))).string();
}

Section::BuildParams sectionParams() {
  Section::BuildParams p;
  p.fontId = 0;
  p.lineCompression = 1.0f;
  p.extraParagraphSpacing = false;
  p.paragraphAlignment = 0;
  p.viewportWidth = 480;
  p.viewportHeight = 800;
  p.hyphenationEnabled = false;
  p.fontSizeNormalization = false;
  p.embeddedStyle = false;
  p.bionicReadingEnabled = false;
  p.inlineFootnotePreviews = false;
  p.imageRendering = 0;
  return p;
}

void setHostClock(const bool synced, const time_t now) {
  hostClockSynced = synced;
  hostClockNow = now;
}

bool writePod(FsFile& file, const auto& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
}

bool writeLegacyV2Bookmark(const std::string& cachePath, const uint16_t spineIndex, const uint16_t page) {
  if (!Storage.mkdir(cachePath.c_str()) && !Storage.exists(cachePath.c_str())) return false;
  FsFile file;
  if (!Storage.openFileForWrite("BKM", cachePath + "/bookmarks.bin", file)) return false;
  const uint8_t version = 2;
  const uint16_t count = 1;
  const uint16_t nameLength = 0;
  const bool ok = writePod(file, version) && writePod(file, count) && writePod(file, spineIndex) &&
                  writePod(file, page) && writePod(file, nameLength);
  return file.close() && ok;
}

std::optional<uint16_t> pageWithParagraph(const Section& section, const uint16_t excludedPage = UINT16_MAX) {
  for (uint16_t page = 0; page < section.pageCount; ++page) {
    if (page != excludedPage && section.getParagraphIndexForPage(page)) return page;
  }
  return std::nullopt;
}

const Bookmark* findRemote(const BookmarkStore& store, const int32_t serverId) {
  for (const Bookmark& bookmark : store.getAll()) {
    if (bookmark.remoteServerId == serverId) return &bookmark;
  }
  return nullptr;
}

const Bookmark* findRemote(const GlobalBookmarkIndex::Entry& entry, const int32_t serverId) {
  for (const Bookmark& bookmark : entry.bookmarks) {
    if (bookmark.remoteServerId == serverId) return &bookmark;
  }
  return nullptr;
}

const GlobalBookmarkIndex::Entry* findGlobalBook(const std::string& path) {
  for (const auto& entry : GLOBAL_BOOKMARKS.getEntries()) {
    if (entry.sourcePath == path) return &entry;
  }
  return nullptr;
}
std::string exchangePageJson(const std::string& hash, const std::string& position, const std::string& key,
                             const std::string& datetime) {
  std::string json = "{\"results\":[{\"hash\":" + jsonQuote(hash) + ",\"more\":false,\"padding\":[";
  for (size_t index = 0; index < 2500; ++index) {
    if (index != 0) json += ',';
    json += '0';
  }
  json += "],\"toApply\":{\"add\":[{\"serverId\":77,\"pos\":" + jsonQuote(position) +
          ",\"pageno\":9001,\"title\":\"remote note\"}],\"delete\":[";
  for (int32_t index = 0; index < 8; ++index) {
    if (index != 0) json += ',';
    json += "{\"serverId\":" + std::to_string(800 + index) + ",\"key\":" + jsonQuote(key) +
            ",\"datetime\":" + jsonQuote(datetime) + "}";
  }
  json += "]}}],\"unmatched\":[]}";
  return json;
}

std::string exchangeMixedJson(const std::string& hash, const std::string& longPosition,
                              const std::string& longTitle, const std::string& normalPosition,
                              const int32_t deletedServerId, const std::string& deleteKey,
                              const std::string& deleteDatetime) {
  return "{\"results\":[{\"hash\":" + jsonQuote(hash) +
         ",\"more\":false,\"toApply\":{\"add\":[{\"serverId\":77,\"pos\":" + jsonQuote(longPosition) +
         ",\"pageno\":9001,\"title\":" + jsonQuote(longTitle) +
         "},{\"serverId\":78,\"pos\":" + jsonQuote(normalPosition) +
         ",\"pageno\":9002,\"title\":\"normal label\"}],\"delete\":[{\"serverId\":" +
         std::to_string(deletedServerId) + ",\"key\":" + jsonQuote(deleteKey) +
         ",\"datetime\":" + jsonQuote(deleteDatetime) + "}]}}],\"unmatched\":[]}";
}

std::string exchangeNoChangesJson(const std::string& hash) {
  return "{\"results\":[{\"hash\":" + jsonQuote(hash) +
         ",\"more\":false,\"toApply\":{\"add\":[],\"delete\":[]}}],\"unmatched\":[]}";
}

std::string exchangeZeroAddJson(const std::string& hash, const std::string& position) {
  return "{\"results\":[{\"hash\":" + jsonQuote(hash) +
         ",\"more\":false,\"toApply\":{\"add\":[{\"serverId\":0,\"pos\":" + jsonQuote(position) +
         ",\"pageno\":1,\"title\":\"invalid\"}],\"delete\":[]}}],\"unmatched\":[]}";
}

std::string exchangeDeleteJson(const std::string& hash, const int32_t serverId, const std::string& key,
                              const std::string& datetime) {
  return "{\"results\":[{\"hash\":" + jsonQuote(hash) +
         ",\"more\":false,\"toApply\":{\"add\":[],\"delete\":[{\"serverId\":" + std::to_string(serverId) +
         ",\"key\":" + jsonQuote(key) + ",\"datetime\":" + jsonQuote(datetime) +
         "}]}}],\"unmatched\":[]}";
}

std::string ackJson(const std::string& hash, const size_t acked) {
  return "{\"results\":[{\"hash\":" + jsonQuote(hash) + ",\"acked\":" + std::to_string(acked) +
         "}],\"unmatched\":[]}";
}

void enqueueJsonResponse(const std::string& body) {
  ScriptedWiFiTransport::enqueue(httpChunkedResponse(body));
}

const std::string& lastCapturedRequest() { return ScriptedWiFiTransport::history().back().request; }


std::string capturedRequestBody() { return httpRequestBody(lastCapturedRequest()); }

std::string clientAckResponse(const BookOrbitBookmarkAckBook& ack) {
  size_t accepted = 0;
  for (const BookOrbitBookmarkAckApplied& item : ack.applied) accepted += item.applied;
  for (const BookOrbitBookmarkAckDeleted& item : ack.deleted) accepted += item.applied;
  return ackJson(ack.hash, accepted);
}
struct StorageScope {
  explicit StorageScope(const std::string& root) : root(root) {}
  ~StorageScope() {
    setHostClock(false, 0);
    ScriptedWiFiTransport::reset();
    KOREADER_STORE.setCredentials({}, {});
    KOREADER_STORE.setServerUrl({});
    Storage.removeDir("/.crosspoint");
    GLOBAL_BOOKMARKS.load();
    Storage.setCrosspointRoot("");
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  std::string root;
};
}  // namespace

namespace HalClock {
bool isSynced() { return hostClockSynced; }
bool isPlausibleForTls() { return true; }
time_t now() { return hostClockNow; }
}  // namespace HalClock

std::string KOReaderSyncClient::deviceId() { return "crosspoint-native-test"; }

KOReaderCredentialStore KOReaderCredentialStore::instance;
void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}
std::string KOReaderCredentialStore::getMd5Password() const { return password; }
bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }
void KOReaderCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }
std::string KOReaderCredentialStore::getBaseUrl() const { return serverUrl; }


TEST(BookOrbitCatalogClient, FetchDetailKeepsFileHashFromFilteredResponse) {
  const std::string root = testRoot();
  fs::create_directories(root);
  Storage.setCrosspointRoot(root);
  StorageScope storageScope(root);

  ScriptedWiFiTransport::reset();
  KOREADER_STORE.setCredentials("detail-user", "0123456789abcdef0123456789abcdef");
  KOREADER_STORE.setServerUrl("http://127.0.0.1/api/v1/koreader");

  const std::string fileHash = "0123456789abcdef0123456789abcdef";
  enqueueJsonResponse(
      R"({"id":19,"title":"The Book","authors":["Author"],"files":[{"id":42,"format":"epub","fileHash":")" +
      fileHash + R"(","sizeBytes":1234,"downloadUrl":"http://files.example/book.epub"}]})");
  BookOrbitBookDetail detail;
  ASSERT_EQ(BookOrbitCatalogClient::fetchDetail("http://127.0.0.1/api/v1/koreader/plugin/catalog/books/19", detail),
            BookOrbitCatalogClient::OK);

  ASSERT_EQ(detail.files.size(), 1u);
  EXPECT_EQ(detail.files[0].fileHash, fileHash);
}

TEST(BookOrbitBookmarkSync, ExchangeMigratesSavesMapsNavigatesRenamesAndDeletes) {
  const std::string root = testRoot();
  fs::create_directories(root);
  Storage.setCrosspointRoot(root);
  StorageScope storageScope(root);
  Storage.removeDir("/.crosspoint");
  GLOBAL_BOOKMARKS.load();

  ScriptedWiFiTransport::reset();
  KOREADER_STORE.setCredentials("bookmark-user", "0123456789abcdef0123456789abcdef");
  KOREADER_STORE.setServerUrl("http://127.0.0.1/api/v1/koreader");

  const std::string cachePath = root + "/book-cache";
  auto epub = std::make_shared<Epub>(kFixture, cachePath);
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  Section section(epub, 1, renderer);
  ASSERT_TRUE(section.createSectionFile(sectionParams(), {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(sectionParams()));
  ASSERT_GT(section.pageCount, 1);

  const auto legacyPage = pageWithParagraph(section);
  ASSERT_TRUE(legacyPage.has_value());
  const std::string bookmarkPath = epub->getCachePath();
  Section::CachedPageInfo legacyPageInfo;
  ASSERT_TRUE(Section::getCachedPageInfoFromAnyLayout(bookmarkPath, 1, *legacyPage, legacyPageInfo));
  EXPECT_EQ(legacyPageInfo.pageCount, section.pageCount);
  ASSERT_TRUE(writeLegacyV2Bookmark(bookmarkPath, 1, *legacyPage));
  setHostClock(false, 0);

  BookmarkStore store;
  store.load(bookmarkPath);
  ASSERT_EQ(store.getAll().size(), 1u);
  EXPECT_TRUE(store.getAll()[0].syncMetadata.empty());
  BookOrbitBookmarkExchangeBook unsyncedRequest;
  size_t totalChanges = 0;
  uint32_t revision = 0;
  EXPECT_FALSE(BookOrbitBookmarkSync::prepareExchange(store, epub, std::string(32, 'a'), 0, unsyncedRequest,
                                                       totalChanges, revision));
  EXPECT_TRUE(store.getAll()[0].syncMetadata.empty());

  setHostClock(true, 1780000000);
  BookOrbitBookmarkExchangeBook request;
  ASSERT_TRUE(BookOrbitBookmarkSync::prepareExchange(store, epub, std::string(32, 'a'), 0, request, totalChanges,
                                                       revision));
  ASSERT_TRUE(request.keysComplete);
  ASSERT_EQ(request.keys.size(), 1u);
  ASSERT_EQ(request.changes.size(), 1u);
  ASSERT_EQ(totalChanges, 1u);
  EXPECT_EQ(request.changes[0].page, *legacyPage);

  BookmarkStore afterRestart;
  afterRestart.load(bookmarkPath);
  ASSERT_EQ(afterRestart.revision(), revision);
  ASSERT_EQ(afterRestart.getAll().size(), 1u);
  BookOrbitBookmarkSync::PositionMetadataView legacyMetadata;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(afterRestart.getAll()[0].syncMetadata, legacyMetadata));
  ASSERT_EQ(legacyMetadata.datetime.size(), 19u);
  int legacySpine = -1;
  float legacyProgress = 0.0f;
  uint16_t legacyLocalPage = 0;
  uint16_t legacyParagraph = 0;
  ASSERT_TRUE(BookOrbitBookmarkSync::resolvePositionForNavigation(
      epub, bookmarkPath, std::string(legacyMetadata.position), legacySpine, legacyProgress, legacyLocalPage,
      &legacyParagraph));
  EXPECT_EQ(legacySpine, 1);
  EXPECT_EQ(legacyLocalPage, *legacyPage);

  const GlobalBookmarkIndex::Entry* globalBook = findGlobalBook(epub->getPath());
  ASSERT_NE(globalBook, nullptr);
  ASSERT_EQ(globalBook->bookmarks.size(), 1u);
  EXPECT_EQ(globalBook->bookmarks[0].syncMetadata, afterRestart.getAll()[0].syncMetadata);
  GLOBAL_BOOKMARKS.load();  // Simulate restart: the home/global list re-reads the persisted index.
  globalBook = findGlobalBook(epub->getPath());
  ASSERT_NE(globalBook, nullptr);
  ASSERT_EQ(globalBook->bookmarks[0].syncMetadata, afterRestart.getAll()[0].syncMetadata);

  const auto remotePage = pageWithParagraph(section, *legacyPage);
  ASSERT_TRUE(remotePage.has_value());
  const std::string remotePacked =
      BookOrbitBookmarkSync::makeLocalPositionMetadata(epub, 1, *remotePage, section.pageCount);
  BookOrbitBookmarkSync::PositionMetadataView remotePosition;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(remotePacked, remotePosition));
  ASSERT_EQ(remotePosition.datetime.size(), 19u);

  const std::string remoteKey = BookOrbitBookmarkSync::keyFor(remotePosition.datetime, remotePosition.position);
  const std::string unrelatedDeleteKey(32, 'b');
  const std::string exchangeJson = exchangePageJson(request.hash, std::string(remotePosition.position),
                                                    unrelatedDeleteKey, std::string(remotePosition.datetime));
  ASSERT_GT(exchangeJson.size(), 4096u);
  BookOrbitBookmarkExchangeRequest clientRequest;
  clientRequest.books.push_back(request);
  enqueueJsonResponse(exchangeJson);
  BookOrbitBookmarkExchangeResponse clientResponse;
  ASSERT_EQ(BookOrbitCatalogClient::exchangeBookmarks(clientRequest, clientResponse), BookOrbitCatalogClient::OK);
  ASSERT_EQ(ScriptedWiFiTransport::history().size(), 1u);
  const std::string& exchangeWireRequest = lastCapturedRequest();
  EXPECT_NE(exchangeWireRequest.find("POST /api/v1/koreader/plugin/bookmarks/exchange HTTP/1.1"),
            std::string::npos);
  EXPECT_NE(exchangeWireRequest.find("x-auth-user: bookmark-user"), std::string::npos);
  EXPECT_NE(exchangeWireRequest.find("x-auth-key: 0123456789abcdef0123456789abcdef"), std::string::npos);
  const std::string serializedExchange = capturedRequestBody();
  EXPECT_NE(serializedExchange.find("\"keysComplete\":true"), std::string::npos);
  EXPECT_NE(serializedExchange.find("\"changes\":["), std::string::npos);
  EXPECT_NE(serializedExchange.find("\"pos\":"), std::string::npos);
  EXPECT_NE(serializedExchange.find("\"deviceId\":\"crosspoint-native-test\""), std::string::npos);
  ASSERT_EQ(clientResponse.results.size(), 1u);
  EXPECT_EQ(clientResponse.results[0].hash, request.hash);
  EXPECT_TRUE(clientResponse.results[0].more);
  ASSERT_EQ(clientResponse.results[0].add.size(), 1u);
  ASSERT_EQ(clientResponse.results[0].remove.size(), 7u);
  EXPECT_EQ(clientResponse.results[0].add[0].serverId, 77);
  EXPECT_EQ(clientResponse.results[0].add[0].page, 9001);
  EXPECT_EQ(clientResponse.results[0].add[0].position, remotePosition.position);

  BookOrbitBookmarkExchangeResult response = std::move(clientResponse.results[0]);
  BookOrbitBookmarkSync::ApplyResult applied;
  ASSERT_TRUE(BookOrbitBookmarkSync::applyResponse(afterRestart, epub, epub->getPath(), bookmarkPath, "Fixture",
                                                    afterRestart.revision(), response, applied));
  ASSERT_EQ(applied.ack.applied.size(), 1u);
  ASSERT_TRUE(applied.ack.applied[0].applied);
  EXPECT_EQ(applied.ack.applied[0].serverId, 77);
  EXPECT_EQ(applied.ack.applied[0].key, remoteKey);
  ASSERT_EQ(applied.ack.deleted.size(), 7u);
  for (const BookOrbitBookmarkAckDeleted& deleted : applied.ack.deleted) EXPECT_TRUE(deleted.applied);

  BookmarkStore durableBeforeAck;
  durableBeforeAck.load(bookmarkPath);
  ASSERT_EQ(durableBeforeAck.getAll().size(), 2u);
  ASSERT_NE(findRemote(durableBeforeAck, 77), nullptr);

  BookOrbitBookmarkAckRequest ackRequest;
  ackRequest.books.push_back(applied.ack);
  enqueueJsonResponse(ackJson(request.hash, 0));
  std::string ackResponse;
  EXPECT_EQ(BookOrbitCatalogClient::acknowledgeBookmarks(ackRequest, ackResponse), BookOrbitCatalogClient::JSON_ERROR);
  ASSERT_EQ(ScriptedWiFiTransport::history().size(), 2u);
  EXPECT_NE(lastCapturedRequest().find("POST /api/v1/koreader/plugin/bookmarks/exchange-ack HTTP/1.1"),
            std::string::npos);
  EXPECT_NE(capturedRequestBody().find("\"serverId\":77"), std::string::npos);

  const std::string acceptedAckJson = clientAckResponse(applied.ack);
  enqueueJsonResponse(acceptedAckJson);
  ASSERT_EQ(BookOrbitCatalogClient::acknowledgeBookmarks(ackRequest, ackResponse), BookOrbitCatalogClient::OK);
  EXPECT_EQ(ackResponse, acceptedAckJson);
  ASSERT_EQ(ScriptedWiFiTransport::history().size(), 3u);
  EXPECT_NE(capturedRequestBody().find("\"applied\":["), std::string::npos);
  EXPECT_NE(capturedRequestBody().find("\"deleted\":["), std::string::npos);

  enqueueJsonResponse(exchangeZeroAddJson(request.hash, std::string(remotePosition.position)));
  BookOrbitBookmarkExchangeResponse invalidAddResponse;
  EXPECT_EQ(BookOrbitCatalogClient::exchangeBookmarks(clientRequest, invalidAddResponse),
            BookOrbitCatalogClient::JSON_ERROR);
  EXPECT_EQ(ScriptedWiFiTransport::history().size(), 4u);

  const Bookmark* remote = findRemote(afterRestart, 77);
  ASSERT_NE(remote, nullptr);
  EXPECT_NE(remote->pageNumber, 9001);
  ASSERT_NE(remote->pageNumber, BookmarkStore::UNKNOWN_PAGE_NUMBER);
  BookOrbitBookmarkSync::PositionMetadataView savedRemotePosition;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(remote->syncMetadata, savedRemotePosition));

  int starredSpine = -1;
  float starredProgress = 0.0f;
  uint16_t starredPage = 0;
  uint16_t starredParagraph = 0;
  ASSERT_TRUE(BookOrbitBookmarkSync::resolvePositionForNavigation(
      epub, bookmarkPath, std::string(savedRemotePosition.position), starredSpine, starredProgress, starredPage,
      &starredParagraph));
  EXPECT_EQ(starredSpine, 1);
  ASSERT_NE(starredParagraph, 0);
  const auto actualStarredPage = section.getPageForParagraphIndex(starredParagraph);
  ASSERT_TRUE(actualStarredPage.has_value());
  EXPECT_EQ(starredPage, *actualStarredPage);
  EXPECT_EQ(remote->pageNumber, *actualStarredPage);

  ASSERT_TRUE(afterRestart.save());
  BookmarkStore starredPagesReload;
  starredPagesReload.load(bookmarkPath);
  remote = findRemote(starredPagesReload, 77);
  ASSERT_NE(remote, nullptr);
  EXPECT_EQ(remote->syncMetadata, std::string(savedRemotePosition.datetime) + "|" +
                                      std::string(savedRemotePosition.position));

  GLOBAL_BOOKMARKS.load();
  globalBook = findGlobalBook(epub->getPath());
  ASSERT_NE(globalBook, nullptr);
  const Bookmark* globalRemote = findRemote(*globalBook, 77);
  ASSERT_NE(globalRemote, nullptr);
  BookOrbitBookmarkSync::PositionMetadataView globalPosition;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(globalRemote->syncMetadata, globalPosition));
  int globalSpine = -1;
  float globalProgress = 0.0f;
  uint16_t globalPage = 0;
  uint16_t globalParagraph = 0;
  ASSERT_TRUE(BookOrbitBookmarkSync::resolvePositionForNavigation(
      epub, bookmarkPath, std::string(globalPosition.position), globalSpine, globalProgress, globalPage,
      &globalParagraph));
  EXPECT_EQ(globalSpine, starredSpine);
  EXPECT_EQ(globalPage, *actualStarredPage);
  EXPECT_EQ(globalParagraph, starredParagraph);

  size_t remoteIndex = 0;
  while (remoteIndex < starredPagesReload.getAll().size() &&
         starredPagesReload.getAll()[remoteIndex].remoteServerId != 77) {
    ++remoteIndex;
  }
  ASSERT_LT(remoteIndex, starredPagesReload.getAll().size());
  starredPagesReload.rename(remoteIndex, "renamed locally");
  ASSERT_TRUE(starredPagesReload.save());
  GLOBAL_BOOKMARKS.syncFromStore(starredPagesReload, epub->getPath(), bookmarkPath, "Fixture", false);
  BookmarkStore renamedReload;
  renamedReload.load(bookmarkPath);
  remote = findRemote(renamedReload, 77);
  ASSERT_NE(remote, nullptr);
  EXPECT_EQ(remote->name, "renamed locally");
  EXPECT_EQ(remote->syncMetadata, std::string(savedRemotePosition.datetime) + "|" +
                                      std::string(savedRemotePosition.position));

  const std::string savedRemoteKey =
      BookOrbitBookmarkSync::keyFor(savedRemotePosition.datetime, savedRemotePosition.position);
  enqueueJsonResponse(exchangeDeleteJson(request.hash, 77, savedRemoteKey,
                                         std::string(savedRemotePosition.datetime)));
  BookOrbitBookmarkExchangeResponse deletionResponse;
  ASSERT_EQ(BookOrbitCatalogClient::exchangeBookmarks(clientRequest, deletionResponse), BookOrbitCatalogClient::OK);
  ASSERT_EQ(deletionResponse.results.size(), 1u);
  ASSERT_EQ(deletionResponse.results[0].remove.size(), 1u);
  EXPECT_EQ(deletionResponse.results[0].remove[0].serverId, 77);
  BookOrbitBookmarkSync::ApplyResult deleted;
  ASSERT_TRUE(BookOrbitBookmarkSync::applyResponse(renamedReload, epub, epub->getPath(), bookmarkPath, "Fixture",
                                                    renamedReload.revision(), deletionResponse.results[0], deleted));
  ASSERT_EQ(deleted.ack.deleted.size(), 1u);
  EXPECT_EQ(deleted.ack.deleted[0].serverId, 77);
  EXPECT_TRUE(deleted.ack.deleted[0].applied);
  EXPECT_EQ(findRemote(renamedReload, 77), nullptr);
  EXPECT_EQ(renamedReload.getAll().size(), 1u);

  BookOrbitBookmarkAckRequest deletionAck;
  deletionAck.books.push_back(deleted.ack);
  const std::string deletionAckJson = clientAckResponse(deleted.ack);
  enqueueJsonResponse(deletionAckJson);
  std::string deletionAckResponse;
  ASSERT_EQ(BookOrbitCatalogClient::acknowledgeBookmarks(deletionAck, deletionAckResponse),
            BookOrbitCatalogClient::OK);
  EXPECT_EQ(deletionAckResponse, deletionAckJson);
  ASSERT_EQ(ScriptedWiFiTransport::history().size(), 6u);
  EXPECT_NE(capturedRequestBody().find("\"deleted\":[{\"serverId\":77"), std::string::npos);

  GLOBAL_BOOKMARKS.load();
  globalBook = findGlobalBook(epub->getPath());
  ASSERT_NE(globalBook, nullptr);
  EXPECT_EQ(findRemote(*globalBook, 77), nullptr);
  EXPECT_EQ(globalBook->bookmarks.size(), 1u);

}
TEST(BookOrbitBookmarkSync, PersistsAndEchoesFullUnicodeRemoteTitleWithMixedChanges) {
  const std::string root = testRoot();
  fs::create_directories(root);
  Storage.setCrosspointRoot(root);
  StorageScope storageScope(root);
  Storage.removeDir("/.crosspoint");
  GLOBAL_BOOKMARKS.load();

  ScriptedWiFiTransport::reset();
  KOREADER_STORE.setCredentials("bookmark-user", "0123456789abcdef0123456789abcdef");
  KOREADER_STORE.setServerUrl("http://127.0.0.1/api/v1/koreader");
  setHostClock(true, 1780000000);

  const std::string fixtureCache = root + "/book-cache";
  auto epub = std::make_shared<Epub>(kFixture, fixtureCache);
  ASSERT_TRUE(epub->load(true));
  epub->setupCacheDir();
  GfxRenderer renderer;
  Section section(epub, 1, renderer);
  ASSERT_TRUE(section.createSectionFile(sectionParams(), {}, /*skipEviction=*/true));
  ASSERT_TRUE(section.loadSectionFile(sectionParams()));
  ASSERT_GT(section.pageCount, 1);

  const auto longPage = pageWithParagraph(section);
  ASSERT_TRUE(longPage.has_value());
  const auto normalPage = pageWithParagraph(section, *longPage);
  ASSERT_TRUE(normalPage.has_value());
  const std::string longPacked = BookOrbitBookmarkSync::makeLocalPositionMetadata(
      epub, 1, *longPage, section.pageCount);
  const std::string normalPacked = BookOrbitBookmarkSync::makeLocalPositionMetadata(
      epub, 1, *normalPage, section.pageCount);
  BookOrbitBookmarkSync::PositionMetadataView longMetadata;
  BookOrbitBookmarkSync::PositionMetadataView normalMetadata;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(longPacked, longMetadata));
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(normalPacked, normalMetadata));
  ASSERT_NE(longMetadata.position, normalMetadata.position);

  const std::string deletedPacked = BookOrbitBookmarkSync::makeLocalPositionMetadata(epub, 0, 0, 1);
  BookOrbitBookmarkSync::PositionMetadataView deletedMetadata;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(deletedPacked, deletedMetadata));

  std::string longTitle;
  longTitle.reserve(2000);
  for (size_t i = 0; i < 500; ++i) longTitle.append("\xF0\x9F\x8C\x8D");
  ASSERT_EQ(longTitle.size(), 2000u);

  const std::string hash(32, 'c');
  BookmarkStore store;
  store.load(epub->getCachePath());
  Bookmark oldRemote;
  oldRemote.spineIndex = 0;
  oldRemote.pageNumber = 0;
  oldRemote.name = "delete me";
  oldRemote.remoteServerId = 900;
  oldRemote.syncMetadata = deletedPacked;
  ASSERT_TRUE(store.applyRemoteAdd(std::move(oldRemote)));
  ASSERT_TRUE(store.save());

  BookOrbitBookmarkSync::PositionMetadataView deletedPosition;
  ASSERT_TRUE(BookOrbitBookmarkSync::decodePositionMetadata(store.getAll()[0].syncMetadata, deletedPosition));
  const std::string deleteKey =
      BookOrbitBookmarkSync::keyFor(deletedPosition.datetime, deletedPosition.position);
  const std::string deleteDatetime(deletedPosition.datetime);

  BookOrbitBookmarkExchangeBook initialBook;
  size_t initialTotalChanges = 0;
  uint32_t initialRevision = 0;
  ASSERT_TRUE(BookOrbitBookmarkSync::prepareExchange(store, epub, hash, 0, initialBook, initialTotalChanges,
                                                       initialRevision));
  BookOrbitBookmarkExchangeRequest initialRequest;
  initialRequest.books.push_back(std::move(initialBook));
  enqueueJsonResponse(exchangeMixedJson(hash, std::string(longMetadata.position), longTitle,
                                        std::string(normalMetadata.position), 900, deleteKey, deleteDatetime));
  BookOrbitBookmarkExchangeResponse parsed;
  ASSERT_EQ(BookOrbitCatalogClient::exchangeBookmarks(initialRequest, parsed), BookOrbitCatalogClient::OK);
  ASSERT_EQ(parsed.results.size(), 1u);

  BookOrbitBookmarkSync::ApplyResult applied;
  ASSERT_TRUE(BookOrbitBookmarkSync::applyResponse(store, epub, epub->getPath(), epub->getCachePath(), epub->getTitle(),
                                                    initialRevision, parsed.results.front(), applied));
  ASSERT_EQ(applied.ack.applied.size(), 2u);
  ASSERT_TRUE(applied.ack.applied[0].applied);
  ASSERT_TRUE(applied.ack.applied[1].applied);
  ASSERT_EQ(applied.ack.deleted.size(), 1u);
  EXPECT_TRUE(applied.ack.deleted[0].applied);

  BookmarkStore restarted;
  restarted.load(epub->getCachePath());
  ASSERT_EQ(restarted.getAll().size(), 2u);
  const Bookmark* longRemote = findRemote(restarted, 77);
  ASSERT_NE(longRemote, nullptr);
  const Bookmark* normalRemote = findRemote(restarted, 78);
  ASSERT_NE(normalRemote, nullptr);
  EXPECT_EQ(normalRemote->name, "normal label");
  EXPECT_EQ(findRemote(restarted, 900), nullptr);
  {
    HostLargestFreeBlock lowHeap(1024);
    BookmarkStore constrained;
    constrained.load(epub->getCachePath());
    EXPECT_FALSE(constrained.isLoadValid());
    EXPECT_FALSE(constrained.save());

    BookOrbitBookmarkExchangeBook blockedRequest;
    size_t blockedChanges = 0;
    uint32_t blockedRevision = 0;
    EXPECT_FALSE(BookOrbitBookmarkSync::prepareExchange(constrained, epub, hash, 0, blockedRequest, blockedChanges,
                                                        blockedRevision));
    EXPECT_TRUE(blockedRequest.keys.empty());
    EXPECT_FALSE(blockedRequest.keysComplete);
  }

  BookOrbitBookmarkAckRequest acknowledgment;
  acknowledgment.books.push_back(applied.ack);
  enqueueJsonResponse(clientAckResponse(applied.ack));
  std::string ackResponse;
  ASSERT_EQ(BookOrbitCatalogClient::acknowledgeBookmarks(acknowledgment, ackResponse), BookOrbitCatalogClient::OK);

  const std::string livePath = epub->getCachePath() + "/bookmarks.bin";
  const std::string backupPath = livePath + ".bak";
  const std::string oldSnapshotPath = root + "/old-bookmarks.bin";
  std::error_code copyError;
  ASSERT_TRUE(fs::copy_file(livePath, oldSnapshotPath, copyError)) << copyError.message();
  ASSERT_TRUE(fs::copy_file(livePath, backupPath, copyError)) << copyError.message();
  FsFile corruptLive;
  ASSERT_TRUE(Storage.openFileForWrite("BKM", livePath, corruptLive));
  const uint8_t invalidVersion = UINT8_MAX;
  ASSERT_EQ(corruptLive.write(&invalidVersion, sizeof(invalidVersion)), sizeof(invalidVersion));
  ASSERT_TRUE(corruptLive.close());

  BookmarkStore recovered;
  recovered.load(epub->getCachePath());
  ASSERT_EQ(recovered.getAll().size(), 2u);
  const Bookmark* recoveredLongRemote = findRemote(recovered, 77);
  ASSERT_NE(recoveredLongRemote, nullptr);
  EXPECT_EQ(recoveredLongRemote->name, longTitle);
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));

  Bookmark newLive;
  newLive.spineIndex = 1;
  newLive.pageNumber = 0;
  newLive.name = "newer live";
  newLive.remoteServerId = 79;
  newLive.syncMetadata = deletedPacked;
  ASSERT_TRUE(recovered.applyRemoteAdd(std::move(newLive)));
  ASSERT_TRUE(recovered.save());

  BookmarkStore livePreferred;
  copyError.clear();
  ASSERT_TRUE(fs::copy_file(oldSnapshotPath, backupPath, copyError)) << copyError.message();
  livePreferred.load(epub->getCachePath());
  ASSERT_EQ(livePreferred.getAll().size(), 3u);
  const Bookmark* preferredRemote = findRemote(livePreferred, 79);
  ASSERT_NE(preferredRemote, nullptr);
  const Bookmark* preferredLongRemote = findRemote(livePreferred, 77);
  ASSERT_NE(preferredLongRemote, nullptr);
  EXPECT_EQ(preferredLongRemote->name, longTitle);
  EXPECT_FALSE(Storage.exists(backupPath.c_str()));

  BookOrbitBookmarkExchangeBook outboundBook;
  size_t totalChanges = 0;
  uint32_t revision = 0;
  ASSERT_TRUE(BookOrbitBookmarkSync::prepareExchange(livePreferred, epub, hash, 0, outboundBook, totalChanges, revision));
  ASSERT_EQ(totalChanges, 3u);
  const auto longChange = std::find_if(outboundBook.changes.begin(), outboundBook.changes.end(),
                                       [&longTitle](const BookOrbitBookmarkChange& change) {
                                         return change.note == longTitle;
                                       });
  ASSERT_NE(longChange, outboundBook.changes.end());

  BookOrbitBookmarkExchangeRequest outbound;
  outbound.books.push_back(std::move(outboundBook));
  enqueueJsonResponse(exchangeNoChangesJson(hash));
  BookOrbitBookmarkExchangeResponse noChanges;
  ASSERT_EQ(BookOrbitCatalogClient::exchangeBookmarks(outbound, noChanges), BookOrbitCatalogClient::OK);
  EXPECT_NE(capturedRequestBody().find(jsonQuote(longTitle)), std::string::npos);
}
