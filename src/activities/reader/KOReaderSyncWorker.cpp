#include "KOReaderSyncWorker.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <Arduino.h>
#include <BookOrbitCatalogClient.h>
#include <HalClock.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <utility>

#include "BookOrbitBookStateSync.h"
#include "BookOrbitBookmarkSync.h"
#include "BookOrbitReadingStatsSync.h"
#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"

namespace {
constexpr unsigned long AUTO_CONNECT_TIMEOUT_MS = 10000;

constexpr uint32_t WORKER_IDLE_EXIT_MS = KOReaderAutoSync::MIN_INTERVAL_PUSH_GAP_MS / 2;
constexpr uint32_t BOOKMARK_ACK_IDLE_EXIT_MS = 15000;

bool workerOwnsRadio = false;
bool bookmarkRadioHeld = false;

SemaphoreHandle_t slotMutex = nullptr;
TaskHandle_t workerTask = nullptr;
KOReaderSyncJob slot;
bool slotBusy = false;
bool jobDone = false;
uint64_t activeSeq = 0;
uint64_t nextSeq = 1;

std::atomic<bool> cachedBusy{false};

void updateCachedBusy() { cachedBusy.store(slotBusy && !jobDone, std::memory_order_relaxed); }

void logInternalHeap(const char* stage) {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  LOG_DBG("KOSyncWorker", "Internal heap[%s]: free=%lu contig=%lu", stage,
          static_cast<unsigned long>(heap_caps_get_free_size(caps)),
          static_cast<unsigned long>(heap_caps_get_largest_free_block(caps)));
}

class SlotLock {
 public:
  SlotLock() { xSemaphoreTake(slotMutex, portMAX_DELAY); }
  ~SlotLock() { xSemaphoreGive(slotMutex); }
  SlotLock(const SlotLock&) = delete;
  SlotLock& operator=(const SlotLock&) = delete;
};

bool connectToSavedNetwork(const unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.setSleep(false);
    return true;
  }

  if (const wifi_mode_t mode = WiFi.getMode(); mode != WIFI_MODE_NULL && mode != WIFI_MODE_STA) {
    LOG_DBG("KOSyncWorker", "Radio is in AP mode. Skipping connection");
    return false;
  }

  const auto& store = WIFI_STORE;
  const std::string last = store.getLastConnectedSsid();
  const WifiCredential* credential = last.empty() ? nullptr : store.findCredential(last);
  if (credential == nullptr && !store.getCredentials().empty()) {
    credential = &store.getCredentials().front();
  }
  if (credential == nullptr) {
    LOG_DBG("KOSyncWorker", "No saved WiFi network to auto-connect");
    return false;
  }

  const std::string ssid = credential->ssid;
  const std::string password = credential->password;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("KOSyncWorker", "Auto-connect to %s timed out", ssid.c_str());
    WiFi.disconnect(false);
    return false;
  }

  WiFi.setSleep(false);
  LOG_DBG("KOSyncWorker", "Auto-connected to %s", ssid.c_str());
  return true;
}

bool radioStillOursToTearDown() { return WiFi.getMode() == WIFI_MODE_STA && !activityManager.wifiActivityOnStack(); }
void releaseWorkerRadio() {
  if (workerOwnsRadio && radioStillOursToTearDown()) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  workerOwnsRadio = false;
  bookmarkRadioHeld = false;
}

void runJob(KOReaderSyncJob& job) {
  job.bookmarkResponseReady = false;
  job.bookmarkSucceeded = true;
  if (job.kind == KOReaderSyncJob::BOOKMARK_CONTINUE && !job.hasBookmarkAck && !job.hasBookmarkExchange) {
    job.attemptOutcome = KOReaderSyncJob::DEFERRED;
    releaseWorkerRadio();
    return;
  }
  if (job.kind != KOReaderSyncJob::BOOKMARK_CONTINUE && !KOReaderAutoSync::automaticAttemptAllowed()) {
    job.attemptOutcome = KOReaderSyncJob::DEFERRED;
    job.result = KOReaderSyncClient::NETWORK_ERROR;
    releaseWorkerRadio();
    return;
  }
  if (activityManager.wifiActivityOnStack()) {
    LOG_DBG("KOSyncWorker", "A network activity owns the radio. Skipping this job");
    job.result = KOReaderSyncClient::NETWORK_ERROR;
    job.attemptOutcome = KOReaderSyncJob::DEFERRED;
    job.bookmarkSucceeded = false;
    releaseWorkerRadio();
    return;
  }

  logInternalHeap("before_radio_up");
  const bool startedRadio = WiFi.getMode() == WIFI_MODE_NULL;
  const unsigned long connectTimeout = job.connectTimeoutMs > 0 ? job.connectTimeoutMs : AUTO_CONNECT_TIMEOUT_MS;
  if (!connectToSavedNetwork(connectTimeout)) {
    job.result = KOReaderSyncClient::NETWORK_ERROR;
    job.bookmarkSucceeded = false;
    if (WiFi.getMode() != WIFI_MODE_NULL && WiFi.getMode() != WIFI_MODE_STA) {
      job.attemptOutcome = KOReaderSyncJob::DEFERRED;
    }
    if (job.kind != KOReaderSyncJob::BOOKMARK_CONTINUE) KOReaderAutoSync::recordAutomaticResult(job);
    if (startedRadio && radioStillOursToTearDown()) WiFi.mode(WIFI_OFF);
    releaseWorkerRadio();
    return;
  }
  if (startedRadio) workerOwnsRadio = true;

  logInternalHeap("after_assoc");
  KOReaderSyncClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  HalClock::ensureUsableForTls(SETTINGS.ntpServer);

  switch (job.kind) {
    case KOReaderSyncJob::FETCH_PROGRESS:
      job.result = KOReaderSyncClient::getProgress(job.primaryHash, job.remote);
      job.httpStatus = KOReaderSyncClient::lastHttpCode;
      job.transportError = KOReaderSyncClient::lastEspError;
      if (job.smartProbe && !job.altHash.empty() && job.altHash != job.primaryHash) {
        job.altResult = KOReaderSyncClient::getProgress(job.altHash, job.altRemote);
      }
      break;
    case KOReaderSyncJob::SILENT_PUSH:
      job.result = KOReaderSyncClient::updateProgress(job.progress);
      job.httpStatus = KOReaderSyncClient::lastHttpCode;
      job.transportError = KOReaderSyncClient::lastEspError;
      break;
    case KOReaderSyncJob::BOOKMARK_CONTINUE:
      job.result = KOReaderSyncClient::OK;
      break;
  }
  if (job.kind != KOReaderSyncJob::BOOKMARK_CONTINUE) {
    if (job.result == KOReaderSyncClient::NETWORK_ERROR && job.transportError == ESP_ERR_NO_MEM &&
        !(job.kind == KOReaderSyncJob::FETCH_PROGRESS && job.smartProbe && job.altResult == KOReaderSyncClient::OK)) {
      job.attemptOutcome = KOReaderSyncJob::DEFERRED;
    }
    KOReaderAutoSync::recordAutomaticResult(job);
  }
  if (job.kind != KOReaderSyncJob::BOOKMARK_CONTINUE &&
      (job.result == KOReaderSyncClient::OK ||
       (job.kind == KOReaderSyncJob::FETCH_PROGRESS && job.result == KOReaderSyncClient::NOT_FOUND))) {
    if (!job.bookOrbitHash.empty()) {
      BookOrbitBookStateResult result;
      const auto stateError = BookOrbitBookStateSync::syncActiveHash(job.bookOrbitHash, result);
      if (stateError != BookOrbitCatalogClient::OK) {
        LOG_INF("KOSyncWorker", "Active BookOrbit state refresh failed (%s)",
                BookOrbitCatalogClient::errorString(stateError));
      }
    }
    BookOrbitReadingStatsSync::drain();
    BookOrbitBookStateSync::drain();
  }

  bool canExchange = true;
  if (job.hasBookmarkAck) {
    BookOrbitBookmarkAckRequest ack;
    ack.books.reserve(1);
    ack.books.push_back(std::move(job.bookmarkAck));
    std::string response;
    BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
    BookOrbitCatalogClient::beginPersistentSession();
    const auto result = BookOrbitCatalogClient::acknowledgeBookmarks(ack, response);
    BookOrbitCatalogClient::endPersistentSession();
    canExchange = result == BookOrbitCatalogClient::OK;
    job.bookmarkSucceeded = canExchange;
    if (!canExchange) {
      job.bookmarkError = static_cast<int>(result);
      job.bookmarkHttpStatus = BookOrbitCatalogClient::lastHttpCode;
      LOG_ERR("KOSyncWorker", "Bookmark acknowledgment failed (%s)", BookOrbitCatalogClient::errorString(result));
    }
  }

  if (canExchange && job.hasBookmarkExchange) {
    BookOrbitBookmarkExchangeRequest request;
    request.books.reserve(1);
    request.books.push_back(std::move(job.bookmarkExchange));
    BookOrbitBookmarkExchangeResponse response;
    BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
    BookOrbitCatalogClient::beginPersistentSession();
    const auto result = BookOrbitCatalogClient::exchangeBookmarks(request, response);
    BookOrbitCatalogClient::endPersistentSession();
    if (result == BookOrbitCatalogClient::OK) {
      job.bookmarkResponse = {};
      for (BookOrbitBookmarkExchangeResult& item : response.results) {
        if (item.hash == request.books.front().hash) {
          job.bookmarkResponse = std::move(item);
          break;
        }
      }
      if (job.bookmarkResponse.hash.empty()) job.bookmarkResponse.hash = request.books.front().hash;
      job.bookmarkResponseReady = true;
    } else {
      job.bookmarkSucceeded = false;
      job.bookmarkError = static_cast<int>(result);
      job.bookmarkHttpStatus = BookOrbitCatalogClient::lastHttpCode;
      LOG_ERR("KOSyncWorker", "Bookmark exchange failed (%s)", BookOrbitCatalogClient::errorString(result));
    }
  }
  logInternalHeap("after_request");

  bookmarkRadioHeld = job.bookmarkResponseReady && job.bookmarkSucceeded;
  if (!bookmarkRadioHeld) releaseWorkerRadio();
}

void syncWorkerTrampoline(void*) {
  for (;;) {
    const TickType_t idleTimeout = pdMS_TO_TICKS(bookmarkRadioHeld ? BOOKMARK_ACK_IDLE_EXIT_MS : WORKER_IDLE_EXIT_MS);
    if (ulTaskNotifyTake(pdTRUE, idleTimeout) == 0) {
      bool exiting = false;
      {
        const SlotLock lock;
        if (!slotBusy || jobDone) {
          if (bookmarkRadioHeld) releaseWorkerRadio();
          workerTask = nullptr;
          exiting = true;
        }
      }
      if (exiting) {
        LOG_DBG("KOSyncWorker", "Idle, freeing the worker task (stack headroom was %u bytes)",
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        vTaskDelete(nullptr);
      }
      continue;
    }
    KOReaderSyncJob job;
    {
      const SlotLock lock;
      if (!slotBusy) {
        continue;
      }
      job = std::move(slot);
    }
    runJob(job);
    {
      const SlotLock lock;
      slot = std::move(job);
      jobDone = true;
      updateCachedBusy();
    }
  }
}

// caller has to hold slotMutex
void ensureTask() {
  if (workerTask != nullptr) {
    return;
  }
#if defined(configNUM_CORES) && configNUM_CORES > 1
  constexpr BaseType_t workerCore = 1;
#else
  constexpr BaseType_t workerCore = 0;
#endif
  if (xTaskCreatePinnedToCore(&syncWorkerTrampoline, "KOSyncWorker", KOReaderSyncWorker::WORKER_STACK_BYTES, nullptr, 1,
                              &workerTask, workerCore) != pdPASS) {
    LOG_ERR("KOSyncWorker", "Failed to create sync worker task");
    workerTask = nullptr;
  }
}

bool ensureMutex() {
  if (slotMutex == nullptr) {
    slotMutex = xSemaphoreCreateMutex();
  }
  return slotMutex != nullptr;
}

}  // namespace

namespace KOReaderSyncWorker {
bool isBusy() { return cachedBusy.load(std::memory_order_relaxed); }

bool post(KOReaderSyncJob&& job, uint64_t* seqOut) {
  if (job.kind != KOReaderSyncJob::BOOKMARK_CONTINUE && !KOReaderAutoSync::automaticAttemptAllowed()) {
    return false;
  }
  if (!ensureMutex()) {
    return false;
  }
  uint64_t seq = 0;
  TaskHandle_t target = nullptr;
  {
    const SlotLock lock;
    if (slotBusy && !jobDone) {
      return false;
    }
    ensureTask();
    if (workerTask == nullptr) {
      return false;
    }
    slot = std::move(job);
    slotBusy = true;
    jobDone = false;
    activeSeq = nextSeq++;
    seq = activeSeq;
    target = workerTask;
    updateCachedBusy();
  }
  xTaskNotifyGive(target);
  if (seqOut != nullptr) {
    *seqOut = seq;
  }
  return true;
}

bool isDone(const uint64_t seq) {
  if (slotMutex == nullptr) return true;
  const SlotLock lock;
  return !(slotBusy && activeSeq == seq && !jobDone);
}

bool waitFor(const uint64_t seq, const unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (!isDone(seq)) {
    if (millis() - start >= timeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

KOReaderSyncJob consume(const uint64_t seq) {
  KOReaderSyncJob out;
  out.result = KOReaderSyncClient::NETWORK_ERROR;
  if (slotMutex == nullptr) return out;
  const SlotLock lock;
  if (slotBusy && activeSeq == seq && jobDone) {
    out = std::move(slot);
    slot = KOReaderSyncJob{};
    slotBusy = false;
    updateCachedBusy();
  }
  return out;
}

bool drain(const unsigned long timeoutMs) {
  const unsigned long start = millis();
  while (isBusy()) {
    if (millis() - start >= timeoutMs) {
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

}  // namespace KOReaderSyncWorker

#endif
