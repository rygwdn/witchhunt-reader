#include "KOReaderAutoSync.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_heap_caps.h>
#include <esp_attr.h>
#include <freertos/FreeRTOS.h>

#include <utility>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderSyncMetadata.h"
#include "KOReaderRetryPolicy.h"
#include "KOReaderSyncWorker.h"

AutoSyncState AutoSyncState::instance;

namespace {
constexpr char AUTOSYNC_FILE_JSON[] = "/.crosspoint/autosync.json";

// The client's already gating the wolfssl handshake, so this is just for association. Numbers
// based on the worker's logs on a x4 pro.
constexpr uint32_t ASSOCIATION_HEADROOM_BYTES = 56 * 1024;
constexpr uint32_t BG_SESSION_MIN_FREE_INTERNAL_BYTES =
    KOReaderSyncWorker::WORKER_STACK_BYTES + ASSOCIATION_HEADROOM_BYTES;
// Largest single block the session needs. Association spreads across whatever is free, but the
// request draws one chunk. Measured 14kb at its worst on a x4 pro.
constexpr uint32_t BG_SESSION_MIN_CONTIG_INTERNAL_BYTES = 24 * 1024;

KOReaderSyncJob buildSilentPushJob(const std::string& epubPath, const KOReaderPosition& localKoPos, const int spine,
                                   const int page, const std::string& title, const std::string& authors,
                                   const std::string& bookOrbitHash) {
  KOReaderSyncJob job;
  job.kind = KOReaderSyncJob::SILENT_PUSH;
  job.path = epubPath;
  job.bookOrbitHash = bookOrbitHash;
  job.spine = spine;
  job.page = page;
  job.turnsSincePushAtStart = AUTOSYNC_STATE.getTurnsSincePush();
  job.progress.document = KOReaderAutoSync::documentHashFor(epubPath);
  job.progress.progress = localKoPos.xpath;
  job.progress.percentage = localKoPos.percentage;
  if (KOREADER_STORE.getSendMetadata()) {
    job.progress.metadata = KOReaderSyncMetadata::forBook(epubPath, title, authors);
  }
  return job;
}

KOReaderSyncJob pendingSleepPush;


constexpr uint32_t RETRY_POLICY_MAGIC = 0x4B525031;  // "KRP1"

struct RetainedRetryPolicy {
  uint32_t magic;
  uint32_t checksum;
  KOReaderRetryPolicy::State state;
};

RTC_NOINIT_ATTR RetainedRetryPolicy retainedRetryPolicy;
portMUX_TYPE retryPolicyMux = portMUX_INITIALIZER_UNLOCKED;
// Normal BSS is cleared once per boot; cache the reset-reason decision so a POWERON reason
// cannot erase retained policy again on every reader-loop gate.
bool retryPolicyBootChecked = false;

uint32_t retryPolicyChecksum() {
  uint32_t hash = 2166136261u;
  const auto append = [&hash](const uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      hash ^= (value >> shift) & 0xffu;
      hash *= 16777619u;
    }
  };
  append(retainedRetryPolicy.magic);
  append(static_cast<uint32_t>(retainedRetryPolicy.state.retryDeadlineUs));
  append(static_cast<uint32_t>(retainedRetryPolicy.state.retryDeadlineUs >> 32));
  append(static_cast<uint32_t>(retainedRetryPolicy.state.lastObservedUs));
  append(static_cast<uint32_t>(retainedRetryPolicy.state.lastObservedUs >> 32));
  append(retainedRetryPolicy.state.failureCount);
  append(retainedRetryPolicy.state.suspended);
  return hash;
}

bool retryPolicyStateValid() {
  return retainedRetryPolicy.magic == RETRY_POLICY_MAGIC &&
         retainedRetryPolicy.state.failureCount <= KOReaderRetryPolicy::MAX_FAILURE_COUNT &&
         retainedRetryPolicy.state.suspended <= 1 && retainedRetryPolicy.checksum == retryPolicyChecksum();
}

void resetRetainedRetryPolicy(const uint64_t nowUs) {
  KOReaderRetryPolicy::reset(retainedRetryPolicy.state, nowUs);
  retainedRetryPolicy.magic = RETRY_POLICY_MAGIC;
  retainedRetryPolicy.checksum = retryPolicyChecksum();
}

void prepareRetryPolicy(const uint64_t nowUs) {
  if (!retryPolicyBootChecked) {
    retryPolicyBootChecked = true;
    if (HalClock::isColdBoot() || !retryPolicyStateValid()) {
      resetRetainedRetryPolicy(nowUs);
    }
  }
  if (!retryPolicyStateValid()) {
    resetRetainedRetryPolicy(nowUs);
  }
  KOReaderRetryPolicy::observeClock(retainedRetryPolicy.state, nowUs);
  retainedRetryPolicy.checksum = retryPolicyChecksum();
}

KOReaderRetryPolicy::AttemptOutcome policyOutcomeFor(const KOReaderSyncJob::AttemptOutcome outcome) {
  switch (outcome) {
    case KOReaderSyncJob::COMPLETED:
      return KOReaderRetryPolicy::AttemptOutcome::COMPLETED;
    case KOReaderSyncJob::DEFERRED:
      return KOReaderRetryPolicy::AttemptOutcome::DEFERRED;
    case KOReaderSyncJob::CANCELLED:
      return KOReaderRetryPolicy::AttemptOutcome::CANCELLED;
  }
  return KOReaderRetryPolicy::AttemptOutcome::CANCELLED;
}
}  // namespace

bool AutoSyncState::saveToFile() {
  JsonDocument doc;
  doc["lastDocPath"] = lastDocPath;
  doc["lastPushedSpine"] = lastPushedSpine;
  doc["lastPushedPage"] = lastPushedPage;
  doc["pending"] = pending;
  doc["lastSyncFailed"] = lastSyncFailedFlag;
  doc["lastSeenSpine"] = lastSeenSpine;
  doc["lastSeenPage"] = lastSeenPage;
  doc["turnsSincePush"] = turnsSincePush;

  String json;
  serializeJson(doc, json);
  Storage.mkdir("/.crosspoint");
  const bool written = Storage.writeFile(AUTOSYNC_FILE_JSON, json);
  if (written) {
    dirty = false;
  }
  return written;
}

bool AutoSyncState::loadFromFile() {
  if (!Storage.exists(AUTOSYNC_FILE_JSON)) {
    return false;
  }
  const String json = Storage.readFile(AUTOSYNC_FILE_JSON);
  if (json.isEmpty()) {
    return false;
  }
  JsonDocument doc;
  if (const auto error = deserializeJson(doc, json.c_str())) {
    LOG_ERR("AutoSync", "JSON parse error: %s", error.c_str());
    return false;
  }
  lastDocPath = doc["lastDocPath"] | std::string("");
  lastPushedSpine = doc["lastPushedSpine"] | -1;
  lastPushedPage = doc["lastPushedPage"] | -1;
  pending = doc["pending"] | false;
  lastSyncFailedFlag = doc["lastSyncFailed"] | false;
  lastSeenSpine = doc["lastSeenSpine"] | -2;
  lastSeenPage = doc["lastSeenPage"] | -2;
  turnsSincePush = doc["turnsSincePush"] | 0;
  lastNoteWasTurnFlag = false;
  return true;
}

bool AutoSyncState::isPositionSynced(const std::string& bookPath, const int spine, const int page) const {
  return !lastDocPath.empty() && bookPath == lastDocPath && spine == lastPushedSpine && page == lastPushedPage;
}

void AutoSyncState::noteProgress(const std::string& bookPath, const int spine, const int page) {
  if (!KOReaderAutoSync::sleepPushEnabled() && !KOReaderAutoSync::intervalPushEnabled()) {
    return;
  }
  lastNoteWasTurnFlag = spine != lastSeenSpine || page != lastSeenPage;
  if (lastNoteWasTurnFlag) {
    lastSeenSpine = spine;
    lastSeenPage = page;

    if (KOReaderAutoSync::intervalPushEnabled()) {
      turnsSincePush++;
      dirty = true;
    }
  }
  if (!pending && (bookPath != lastDocPath || spine != lastPushedSpine || page != lastPushedPage)) {
    pending = true;
    saveToFile();
  }
}

void AutoSyncState::flush() {
  if (dirty) {
    saveToFile();
  }
}

void AutoSyncState::markSyncFailed() {
  if (lastSyncFailedFlag) {
    return;
  }
  lastSyncFailedFlag = true;
  saveToFile();
}

void AutoSyncState::markSynced(const std::string& bookPath, const int spine, const int page,
                               const int32_t turnsAtPushStart) {
  // Discard the threshold already covered by this upload, not turns recorded while it ran.
  const bool turnsDuringPush = turnsAtPushStart >= 0 && turnsSincePush > turnsAtPushStart;
  pending = turnsDuringPush && (lastSeenSpine != spine || lastSeenPage != page);
  lastSyncFailedFlag = false;
  lastDocPath = bookPath;
  lastPushedSpine = spine;
  lastPushedPage = page;

  if (turnsDuringPush) {
    turnsSincePush -= turnsAtPushStart;
  } else {
    lastSeenSpine = spine;
    lastSeenPage = page;
    turnsSincePush = 0;
  }
  KOReaderAutoSync::resetRetryPolicy();
  saveToFile();
}

void AutoSyncState::seedLastSeen(const int spine, const int page) {
  lastSeenSpine = spine;
  lastSeenPage = page;
  lastNoteWasTurnFlag = false;
}

DocumentMatchMethod KOReaderAutoSync::effectiveMatchMethod(const std::string& epubPath) {
  return KOReaderDocumentId::loadLearnedMatchMethod(epubPath).value_or(KOREADER_STORE.getMatchMethod());
}

std::string KOReaderAutoSync::documentHashFor(const std::string& epubPath) {
  return effectiveMatchMethod(epubPath) == DocumentMatchMethod::FILENAME
             ? KOReaderDocumentId::calculateFromFilename(epubPath)
             : KOReaderDocumentId::calculate(epubPath);
}

bool KOReaderAutoSync::jobActive() { return KOReaderSyncWorker::isBusy(); }

bool KOReaderAutoSync::automaticAttemptAllowed() {
  portENTER_CRITICAL(&retryPolicyMux);
  const uint64_t nowUs = HalClock::retainedMonotonicUs();
  prepareRetryPolicy(nowUs);
  const bool allowed = KOReaderRetryPolicy::attemptAllowed(retainedRetryPolicy.state, nowUs);
  retainedRetryPolicy.checksum = retryPolicyChecksum();
  portEXIT_CRITICAL(&retryPolicyMux);
  return allowed;
}

void KOReaderAutoSync::resetRetryPolicy() {
  portENTER_CRITICAL(&retryPolicyMux);
  const uint64_t nowUs = HalClock::retainedMonotonicUs();
  retryPolicyBootChecked = true;
  resetRetainedRetryPolicy(nowUs);
  portEXIT_CRITICAL(&retryPolicyMux);
}

void KOReaderAutoSync::recordAutomaticResult(const KOReaderSyncJob& job) {
  if (job.kind == KOReaderSyncJob::BOOKMARK_CONTINUE) return;
  const KOReaderRetryPolicy::AttemptOutcome outcome = policyOutcomeFor(job.attemptOutcome);
  const bool alternateFoundProgress = job.kind == KOReaderSyncJob::FETCH_PROGRESS && job.smartProbe &&
                                      job.altResult == KOReaderSyncClient::OK &&
                                      outcome == KOReaderRetryPolicy::AttemptOutcome::COMPLETED;
  const KOReaderRetryPolicy::ResultClass result =
      alternateFoundProgress
          ? KOReaderRetryPolicy::ResultClass::SUCCESS
          : KOReaderRetryPolicy::classify(job.result, job.kind == KOReaderSyncJob::FETCH_PROGRESS, job.httpStatus,
                                          job.transportError, outcome);
  if (result == KOReaderRetryPolicy::ResultClass::NO_CHANGE) return;
  uint8_t failureCount = 0;
  uint32_t cooldownMinutes = 0;
  portENTER_CRITICAL(&retryPolicyMux);
  const uint64_t completedAtUs = HalClock::retainedMonotonicUs();
  prepareRetryPolicy(completedAtUs);
  KOReaderRetryPolicy::record(retainedRetryPolicy.state, result, completedAtUs);
  if (result == KOReaderRetryPolicy::ResultClass::TRANSIENT_FAILURE) {
    failureCount = retainedRetryPolicy.state.failureCount;
    cooldownMinutes = static_cast<uint32_t>(
        KOReaderRetryPolicy::retryDelayUs(failureCount) / KOReaderRetryPolicy::MINUTE_US);
  }
  retainedRetryPolicy.checksum = retryPolicyChecksum();
  portEXIT_CRITICAL(&retryPolicyMux);

  if (result == KOReaderRetryPolicy::ResultClass::TRANSIENT_FAILURE) {
    LOG_INF("AutoSync", "Automatic sync retry in %lu min after %s (failure %u, HTTP %d, transport %d)",
            static_cast<unsigned long>(cooldownMinutes), KOReaderSyncClient::errorString(job.result),
            static_cast<unsigned>(failureCount), job.httpStatus, job.transportError);
  } else if (result == KOReaderRetryPolicy::ResultClass::SUSPEND) {
    LOG_INF("AutoSync", "Automatic sync suspended after %s (HTTP %d, transport %d)",
            KOReaderSyncClient::errorString(job.result), job.httpStatus, job.transportError);
  }
}


bool KOReaderAutoSync::heapAllowsBackgroundSession(const char* what) {
  constexpr uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  const uint32_t freeInternal = heap_caps_get_free_size(caps);
  const uint32_t contigInternal = heap_caps_get_largest_free_block(caps);
  if (freeInternal < BG_SESSION_MIN_FREE_INTERNAL_BYTES || contigInternal < BG_SESSION_MIN_CONTIG_INTERNAL_BYTES) {
    LOG_DBG("AutoSync", "%s skipped: internal heap too tight (free=%lu contig=%lu)", what,
            static_cast<unsigned long>(freeInternal), static_cast<unsigned long>(contigInternal));
    return false;
  }
  return true;
}

bool KOReaderAutoSync::pullEnabled() { return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getSyncOnWake(); }

bool KOReaderAutoSync::sleepPushEnabled() { return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getSyncOnSleep(); }

bool KOReaderAutoSync::intervalPushEnabled() {
  return KOREADER_STORE.hasCredentials() && KOREADER_STORE.getPushIntervalPages() > 0;
}

KOReaderSyncJob KOReaderAutoSync::makeSilentPushJob(const std::string& epubPath,
                                                    const KOReaderPosition& localKoPos, const int spine,
                                                    const int page, const std::string& title,
                                                    const std::string& authors,
                                                    const std::string& bookOrbitHash) {
  return buildSilentPushJob(epubPath, localKoPos, spine, page, title, authors, bookOrbitHash);
}

uint64_t KOReaderAutoSync::pushSilentAsync(KOReaderSyncJob&& job) {
  if (!automaticAttemptAllowed()) return 0;
  if (!KOREADER_STORE.hasCredentials()) return 0;
  if (!heapAllowsBackgroundSession("Silent push")) return 0;
  if (job.progress.document.empty()) {
    LOG_ERR("AutoSync", "Document hash failed for %s", job.path.c_str());
    return 0;
  }
  if (job.progress.progress.empty()) {
    LOG_ERR("AutoSync", "Local mapping produced no xpath for %s; skipping push", job.path.c_str());
    return 0;
  }
  LOG_DBG("AutoSync", "Silent push queued: %s (%.6f)", job.path.c_str(), job.progress.percentage);
  uint64_t seq = 0;
  if (!KOReaderSyncWorker::post(std::move(job), &seq)) {
    LOG_DBG("AutoSync", "Push deferred: sync worker busy");
    return 0;
  }
  return seq;
}


bool KOReaderAutoSync::finalizeSilentPush(const KOReaderSyncJob& job) {
  if (job.kind != KOReaderSyncJob::SILENT_PUSH) return false;
  if (job.result != KOReaderSyncClient::OK) {
    LOG_INF("AutoSync", "Silent push failed: %s", KOReaderSyncClient::errorString(job.result));
    AUTOSYNC_STATE.markSyncFailed();
    return false;
  }
  AUTOSYNC_STATE.markSynced(job.path, job.spine, job.page, job.turnsSincePushAtStart);
  LOG_DBG("AutoSync", "Silent push OK: %s (%.6f)", job.path.c_str(), job.progress.percentage);
  return true;
}

bool KOReaderAutoSync::finalizeSilentPush(const uint64_t seq) {
  if (seq == 0) return false;
  const KOReaderSyncJob job = KOReaderSyncWorker::consume(seq);
  return finalizeSilentPush(job);
}

bool KOReaderAutoSync::stashSleepPush(KOReaderSyncJob&& job) {
  if (!automaticAttemptAllowed()) return false;
  if (!KOREADER_STORE.hasCredentials() || job.progress.document.empty() || job.progress.progress.empty()) return false;
  job.connectTimeoutMs = SLEEP_PUSH_CONNECT_TIMEOUT_MS;
  pendingSleepPush = std::move(job);
  return true;
}


uint64_t KOReaderAutoSync::pushStashedSleepPushAsync() {
  if (pendingSleepPush.path.empty()) return 0;
  if (!automaticAttemptAllowed()) {
    pendingSleepPush = KOReaderSyncJob{};
    return 0;
  }
  KOReaderSyncJob job = std::move(pendingSleepPush);
  pendingSleepPush = KOReaderSyncJob{};
  uint64_t seq = 0;
  if (!KOReaderSyncWorker::post(std::move(job), &seq)) {
    LOG_DBG("AutoSync", "Sleep push deferred: sync worker busy; join tail will retry");
    return 0;
  }
  return seq;
}

bool KOReaderAutoSync::sleepPushPending(const uint64_t seq) {
  return seq != 0 ? !KOReaderSyncWorker::isDone(seq) : !pendingSleepPush.path.empty();
}

void KOReaderAutoSync::joinStashedSleepPush(const uint64_t postedSeq) {
  uint64_t seq = postedSeq;
  unsigned long budget = SLEEP_PUSH_JOIN_TIMEOUT_MS;
  if (seq == 0) {
    if (!automaticAttemptAllowed()) return;
    if (pendingSleepPush.path.empty()) {
      return;
    }

    LOG_DBG("AutoSync", "Retrying deferred sleep push on the join tail");
    const unsigned long start = millis();
    KOReaderSyncWorker::drain(budget);
    const bool posted = KOReaderSyncWorker::post(std::move(pendingSleepPush), &seq);

    pendingSleepPush = KOReaderSyncJob{};
    if (!posted) {
      LOG_DBG("AutoSync", "Sleep push retry timed out; deferring to the next boundary");
      AUTOSYNC_STATE.markSyncFailed();
      return;
    }
    const unsigned long elapsed = millis() - start;
    budget = elapsed < budget ? budget - elapsed : 0;
  }
  if (!KOReaderSyncWorker::waitFor(seq, budget)) {
    LOG_DBG("AutoSync", "Sleep push still running at the join deadline; deferring to the next boundary");
    AUTOSYNC_STATE.markSyncFailed();
    return;
  }
  if (finalizeSilentPush(seq)) {
    LOG_DBG("AutoSync", "Sleep push OK");
  }
}

#endif
