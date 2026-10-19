#include <gtest/gtest.h>

#include <string>

#include <HalClock.h>
#include <WiFi.h>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "activities/ActivityManager.h"

constexpr int ESP_ERR_NO_MEM = 0x101;

// Compile production implementations directly so host tests exercise real worker and silent-push
// state transitions with deterministic SDK stubs.
#include "../../src/activities/reader/KOReaderSyncWorker.cpp"
#include "../../src/activities/reader/KOReaderAutoSync.cpp"

BookmarkWorkerTestSettings SETTINGS;
WifiCredentialStore WIFI_STORE;
ActivityManager activityManager;
KOReaderCredentialStore KOReaderCredentialStore::instance;
void KOReaderCredentialStore::setCredentials(const std::string& user, const std::string& pass) {
  username = user;
  password = pass;
}
std::string KOReaderCredentialStore::getMd5Password() const { return password; }
bool KOReaderCredentialStore::hasCredentials() const { return !username.empty() && !password.empty(); }
void KOReaderCredentialStore::setServerUrl(const std::string& url) { serverUrl = url; }
std::string KOReaderCredentialStore::getBaseUrl() const { return serverUrl; }


namespace HalClock {
bool ensureUsableForTls(const char*) { return true; }
bool isSynced() { return true; }
time_t now() { return 1780000000; }
bool isColdBoot() { return true; }
uint64_t retainedMonotonicUs() { return static_cast<uint64_t>(millis()) * 1000; }
}  // namespace HalClock

namespace BookOrbitReadingStatsSync {
void drain() {}
}  // namespace BookOrbitReadingStatsSync


namespace BookOrbitBookStateSync {
BookOrbitCatalogClient::Error syncHash(const std::string&, BookOrbitBookStateResult&) {
  return BookOrbitCatalogClient::OK;
}
BookOrbitCatalogClient::Error syncActiveHash(const std::string&, BookOrbitBookStateResult&) {
  return BookOrbitCatalogClient::OK;
}
void drain() {}
}  // namespace BookOrbitBookStateSync
void KOReaderSyncClient::setSkipTlsValidation(bool) {}
KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string&, KOReaderProgress&) {
  return OK;
}
KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress&) { return OK; }
const char* KOReaderSyncClient::errorString(Error) { return "test"; }
int KOReaderSyncClient::lastHttpCode = 0;
int KOReaderSyncClient::lastEspError = 0;

void BookOrbitCatalogClient::setSkipTlsValidation(bool) {}
void BookOrbitCatalogClient::beginPersistentSession() {}
void BookOrbitCatalogClient::endPersistentSession() {}
BookOrbitCatalogClient::Error BookOrbitCatalogClient::acknowledgeBookmarks(const BookOrbitBookmarkAckRequest&,
                                                                           std::string&) {
  return OK;
}
BookOrbitCatalogClient::Error BookOrbitCatalogClient::exchangeBookmarks(const BookOrbitBookmarkExchangeRequest&,
                                                                         BookOrbitBookmarkExchangeResponse&) {
  return OK;
}
const char* BookOrbitCatalogClient::errorString(Error) { return "test"; }

SemaphoreHandle_t xSemaphoreCreateMutex() {
  static BookmarkWorkerTestSemaphore semaphore;
  return &semaphore;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t* handle,
                                   BaseType_t) {
  if (handle != nullptr) *handle = reinterpret_cast<TaskHandle_t>(1);
  return pdPASS;
}
void xTaskNotifyGive(TaskHandle_t) {}
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 0; }
void vTaskDelay(TickType_t) {}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 0; }
void vTaskDelete(TaskHandle_t) {}

TEST(KOReaderWorkerOwnership, EmptyContinuationReleasesOnlyWorkerOwnedRadio) {
  KOReaderSyncJob emptyContinuation;
  emptyContinuation.kind = KOReaderSyncJob::BOOKMARK_CONTINUE;

  WiFi.mode(WIFI_MODE_STA);
  WiFi.setConnected(true);
  activityManager.currentActivityUsesWifi_ = true;
  workerOwnsRadio = true;
  bookmarkRadioHeld = true;
  runJob(emptyContinuation);
  EXPECT_EQ(WiFi.getMode(), WIFI_MODE_STA);
  EXPECT_EQ(WiFi.disconnectCount, 0);
  EXPECT_FALSE(workerOwnsRadio);
  EXPECT_FALSE(bookmarkRadioHeld);

  activityManager.currentActivityUsesWifi_ = false;
  workerOwnsRadio = true;
  WiFi.setConnected(true);
  runJob(emptyContinuation);
  EXPECT_EQ(WiFi.getMode(), WIFI_MODE_NULL);
  EXPECT_EQ(WiFi.disconnectCount, 1);
  EXPECT_FALSE(workerOwnsRadio);
  EXPECT_FALSE(bookmarkRadioHeld);
}

TEST(KOReaderAutoSync, SuccessfulPushRetainsOnlyTurnsMadeWhileInFlight) {
  constexpr char bookPath[] = "/books/inflight.epub";
  KOReaderCredentialStore& credentials = KOREADER_STORE;
  credentials.setCredentials("reader", "secret");
  credentials.setServerUrl("http://localhost");
  credentials.setPushIntervalPages(2);
  

  AutoSyncState& state = AUTOSYNC_STATE;
  state.markSynced(bookPath, 0, 0);
  state.seedLastSeen(0, 0);
  state.noteProgress(bookPath, 0, 1);
  state.noteProgress(bookPath, 0, 2);
  const int32_t turnsAtPushStart = state.getTurnsSincePush();
  ASSERT_EQ(turnsAtPushStart, 2);

  KOReaderSyncJob completedPush;
  completedPush.kind = KOReaderSyncJob::SILENT_PUSH;
  completedPush.result = KOReaderSyncClient::OK;
  completedPush.path = bookPath;
  completedPush.spine = 0;
  completedPush.page = 2;
  completedPush.turnsSincePushAtStart = turnsAtPushStart;
  state.noteProgress(bookPath, 0, 3);
  state.noteProgress(bookPath, 0, 4);

  ASSERT_TRUE(KOReaderAutoSync::finalizeSilentPush(completedPush));
  EXPECT_EQ(state.getTurnsSincePush(), 2);
  EXPECT_TRUE(state.isPending());

  state.markSynced(bookPath, 0, 4);
  state.seedLastSeen(0, 4);
  state.noteProgress(bookPath, 0, 5);
  state.noteProgress(bookPath, 0, 6);
  KOReaderSyncJob returnedToSnapshot = completedPush;
  returnedToSnapshot.page = 6;
  returnedToSnapshot.turnsSincePushAtStart = state.getTurnsSincePush();
  state.noteProgress(bookPath, 0, 5);
  state.noteProgress(bookPath, 0, 6);

  ASSERT_TRUE(KOReaderAutoSync::finalizeSilentPush(returnedToSnapshot));
  EXPECT_EQ(state.getTurnsSincePush(), 2);
  EXPECT_FALSE(state.isPending());
}
