#include "BookOrbitBookStateSync.h"

#include <HalClock.h>
#include <Logging.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>

#include <atomic>
#include <ctime>
#include <string>
#include <utility>

#include "BookOrbitBookStateStore.h"
#include "CrossPointSettings.h"

namespace {
// Serialize canonical state round-trips so an older response cannot race a newer refresh.
std::atomic_flag syncActive = ATOMIC_FLAG_INIT;
// finishReviewUpdate requires an output record, but this path consumes only the
// durable write; keep its decoded-state scratch off the worker/UI stack.
BookOrbitBookStateStore::State canonicalScratch;

class SyncLock {
 public:
  SyncLock() : acquired(!syncActive.test_and_set(std::memory_order_acquire)) {}
  ~SyncLock() {
    if (acquired) syncActive.clear(std::memory_order_release);
  }
  bool owns() const { return acquired; }

 private:
  bool acquired;
};

struct ReviewNoteSink {
  const std::string& hash;
  BookOrbitBookStateStore::ReviewNoteWriter writer;
  bool started = false;
  bool ended = false;
};

bool receiveReviewNote(void* raw, const uint8_t* bytes, const size_t length, const bool final) {
  auto& sink = *static_cast<ReviewNoteSink*>(raw);
  if (!sink.started) {
    if (!BookOrbitBookStateStore::beginReviewUpdate(sink.hash, "", sink.writer)) return false;
    sink.started = true;
  }
  if (length && !BookOrbitBookStateStore::writeReviewChunk(sink.writer, bytes, length)) return false;
  if (final) sink.ended = true;
  return true;
}

BookOrbitCatalogClient::ReadStatus localStatusFor(const BookOrbitBookStateStatus status) {
  switch (status) {
    case BookOrbitBookStateStatus::Complete:
      return BookOrbitCatalogClient::ReadStatus::Read;
    case BookOrbitBookStateStatus::Abandoned:
      return BookOrbitCatalogClient::ReadStatus::Abandoned;
    case BookOrbitBookStateStatus::Reading:
      return BookOrbitCatalogClient::ReadStatus::Reading;
  }
  return BookOrbitCatalogClient::ReadStatus::Reading;
}

BookOrbitCatalogClient::Error fetchCanonicalStatus(const std::string& hash, const int32_t bookId) {
  BookOrbitBookDetail detail;
  const std::string path = KOREADER_STORE.getBaseUrl() + "/plugin/catalog/books/" + std::to_string(bookId);
  const auto error = BookOrbitCatalogClient::fetchDetail(path, detail);
  if (error != BookOrbitCatalogClient::OK) return error;
  if (detail.bookId != bookId || detail.readStatus.empty()) return BookOrbitCatalogClient::JSON_ERROR;
  const auto status = BookOrbitCatalogClient::readStatusFromKey(detail.readStatus);
  if (!BookOrbitBookStateStore::rememberCanonicalStatus(hash, status)) {
    LOG_ERR("BOState", "Could not cache canonical status for %s", hash.c_str());
    return BookOrbitCatalogClient::NETWORK_ERROR;
  }
  return BookOrbitCatalogClient::OK;
}

BookOrbitCatalogClient::Error sendOne(const BookOrbitBookStateStore::State& state,
                                      const BookOrbitMatch& mapping, const BookOrbitBookStateUpload& upload,
                                      const bool refreshStatus, const bool acknowledgeStatus,
                                      const bool acknowledgeRating, BookOrbitBookStateResult* resultOut) {
  ReviewNoteSink noteSink{state.hash, {}, false, false};
  BookOrbitBookStateResult result;
  const auto error = BookOrbitCatalogClient::uploadBookState(upload, receiveReviewNote, &noteSink, result);
  if (error != BookOrbitCatalogClient::OK || result.unmatched) {
    if (noteSink.started) BookOrbitBookStateStore::abortReviewUpdate(noteSink.writer);
    return error != BookOrbitCatalogClient::OK ? error : BookOrbitCatalogClient::NOT_FOUND;
  }
  if (!result.reviewNotePresent || !noteSink.started || !noteSink.ended) {
    if (noteSink.started) BookOrbitBookStateStore::abortReviewUpdate(noteSink.writer);
    return BookOrbitCatalogClient::JSON_ERROR;
  }
  if (!BookOrbitBookStateStore::finishReviewUpdate(noteSink.writer, result.reviewNoteSet, result.reviewUpdatedAt,
                                                    canonicalScratch)) {
    canonicalScratch = {};
    return BookOrbitCatalogClient::NETWORK_ERROR;
  }
  canonicalScratch = {};
  if (!BookOrbitBookStateStore::rememberCanonicalRating(state.hash, result.ratingSet, result.rating,
                                                         result.ratingUpdatedAt)) {
    LOG_ERR("BOState", "Could not cache canonical rating for %s", state.hash.c_str());
    return BookOrbitCatalogClient::NETWORK_ERROR;
  }

  if (upload.hasStatus && result.statusApplied) {
    if (!BookOrbitBookStateStore::rememberCanonicalStatus(state.hash, localStatusFor(upload.status))) {
      LOG_ERR("BOState", "Could not cache accepted status for %s", state.hash.c_str());
      return BookOrbitCatalogClient::NETWORK_ERROR;
    }
  } else if (refreshStatus || upload.hasStatus) {
    const auto statusError = fetchCanonicalStatus(state.hash, mapping.bookId);
    if (statusError != BookOrbitCatalogClient::OK) return statusError;
  }

  // Persist every canonical value before retiring the exact in-flight revisions.
  if ((acknowledgeStatus || acknowledgeRating) &&
      !BookOrbitBookStateStore::acknowledge(state, acknowledgeStatus, acknowledgeRating)) {
    LOG_ERR("BOState", "Could not acknowledge state for %s", state.hash.c_str());
    return BookOrbitCatalogClient::NETWORK_ERROR;
  }
  if (resultOut) *resultOut = std::move(result);
  return BookOrbitCatalogClient::OK;
}

enum class SyncMode { Passive, ActiveReader };

BookOrbitCatalogClient::Error sendPending(BookOrbitBookStateStore::State& state,
                                          BookOrbitBookStateResult* resultOut, const SyncMode mode) {
  BookOrbitMatchCheckResponse match;
  BookOrbitCatalogClient::Error error = BookOrbitCatalogClient::matchCheck(state.hash, match);
  if (error != BookOrbitCatalogClient::OK) return error;
  if (match.matches.empty()) return BookOrbitCatalogClient::NOT_FOUND;

  const BookOrbitMatch& mapping = match.matches.front();
  if ((state.bookId != mapping.bookId || state.bookFileId != mapping.bookFileId ||
       state.libraryVersion != match.libraryVersion) &&
      !BookOrbitBookStateStore::rememberMapping(state.hash, mapping.bookId, mapping.bookFileId, match.libraryVersion)) {
    LOG_ERR("BOState", "Could not cache match for %s", state.hash.c_str());
    return BookOrbitCatalogClient::NETWORK_ERROR;
  }

  if (state.pendingStatus && state.pendingRating && state.statusModified != state.ratingModified) {
    BookOrbitBookStateUpload statusUpload;
    statusUpload.hash = state.hash;
    statusUpload.hasStatus = true;
    statusUpload.status = state.syncStatus;
    statusUpload.statusModified = state.statusModified;
    error = sendOne(state, mapping, statusUpload, false, true, false, resultOut);
    if (error != BookOrbitCatalogClient::OK) return error;

    BookOrbitBookStateUpload ratingUpload;
    ratingUpload.hash = state.hash;
    ratingUpload.hasRating = true;
    ratingUpload.rating = state.rating;
    ratingUpload.ratingCleared = state.ratingCleared;
    ratingUpload.statusModified = state.ratingModified;
    error = sendOne(state, mapping, ratingUpload, false, false, true, resultOut);
  } else {
    BookOrbitBookStateUpload upload;
    upload.hash = state.hash;
    upload.hasStatus = state.pendingStatus;
    upload.status = state.syncStatus;
    upload.hasRating = state.pendingRating;
    upload.rating = state.rating;
    upload.ratingCleared = state.ratingCleared;
    if (upload.hasStatus) {
      upload.statusModified = state.statusModified;
    } else if (upload.hasRating) {
      upload.statusModified = state.ratingModified;
    }
    error = sendOne(state, mapping, upload, !state.pendingStatus, state.pendingStatus, state.pendingRating, resultOut);
  }
  if (error != BookOrbitCatalogClient::OK || mode != SyncMode::ActiveReader) return error;
  // Reuse the caller's string-backed record rather than allocate another State on the worker stack.
  if (!BookOrbitBookStateStore::read(state.hash, state)) return BookOrbitCatalogClient::JSON_ERROR;
  if (state.localStatusSet || state.pendingStatus || !state.canonicalStatusSet ||
      (state.canonicalStatus != BookOrbitCatalogClient::ReadStatus::Unread &&
       state.canonicalStatus != BookOrbitCatalogClient::ReadStatus::WantToRead)) {
    return error;
  }

  std::string modifiedDay;
  if (!BookOrbitBookStateSync::currentModifiedDay(modifiedDay)) return error;
  switch (BookOrbitBookStateStore::queueReaderOpened(state.hash, modifiedDay)) {
    case BookOrbitBookStateStore::QueueResult::Queued:
      break;
    case BookOrbitBookStateStore::QueueResult::Skipped:
      return error;
    case BookOrbitBookStateStore::QueueResult::Invalid:
      return BookOrbitCatalogClient::JSON_ERROR;
    case BookOrbitBookStateStore::QueueResult::IoError:
      return BookOrbitCatalogClient::NETWORK_ERROR;
  }

  if (!BookOrbitBookStateStore::read(state.hash, state)) return BookOrbitCatalogClient::JSON_ERROR;
  BookOrbitBookStateUpload readingUpload;
  readingUpload.hash = state.hash;
  readingUpload.hasStatus = true;
  readingUpload.status = state.syncStatus;
  readingUpload.statusModified = state.statusModified;
  return sendOne(state, mapping, readingUpload, false, true, false, resultOut);
}

BookOrbitCatalogClient::Error syncHashUnlocked(const std::string& hash, BookOrbitBookStateResult& result,
                                              const SyncMode mode) {
  BookOrbitBookStateStore::State state;
  if (!BookOrbitBookStateStore::read(hash, state)) return BookOrbitCatalogClient::JSON_ERROR;
  result = {};
  return sendPending(state, &result, mode);
}

BookOrbitCatalogClient::Error refreshCanonicalUnlocked(const std::string& hash, const int32_t bookId,
                                                       BookOrbitBookStateResult& result) {
  if (bookId <= 0) return BookOrbitCatalogClient::JSON_ERROR;
  BookOrbitBookStateStore::State state;
  if (!BookOrbitBookStateStore::read(hash, state)) return BookOrbitCatalogClient::JSON_ERROR;
  BookOrbitMatch mapping;
  mapping.hash = state.hash;
  mapping.bookId = bookId;
  BookOrbitBookStateUpload upload;
  upload.hash = state.hash;
  result = {};
  return sendOne(state, mapping, upload, true, false, false, &result);
}

}  // namespace

namespace BookOrbitBookStateSync {

bool currentModifiedDay(std::string& day) {
  day.clear();
  if (!HalClock::isSynced()) return false;
  const time_t now = HalClock::now();
  struct tm utc{};
  if (!gmtime_r(&now, &utc)) return false;
  char date[11]{};
  if (strftime(date, sizeof(date), "%Y-%m-%d", &utc) != 10) return false;
  day = date;
  return true;
}

BookOrbitCatalogClient::Error syncHash(const std::string& hash, BookOrbitBookStateResult& result) {
  if (!KOREADER_STORE.hasCredentials()) return BookOrbitCatalogClient::NO_CREDENTIALS;
  if (!KOREADER_STORE.getCatalogCapable()) return BookOrbitCatalogClient::NOT_SUPPORTED;
  const SyncLock lock;
  if (!lock.owns()) return BookOrbitCatalogClient::NETWORK_ERROR;

  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  return syncHashUnlocked(hash, result, SyncMode::Passive);
}

BookOrbitCatalogClient::Error refreshCanonical(const std::string& hash, const int32_t bookId,
                                               BookOrbitBookStateResult& result) {
  if (!KOREADER_STORE.hasCredentials()) return BookOrbitCatalogClient::NO_CREDENTIALS;
  if (!KOREADER_STORE.getCatalogCapable()) return BookOrbitCatalogClient::NOT_SUPPORTED;
  const SyncLock lock;
  if (!lock.owns()) return BookOrbitCatalogClient::NETWORK_ERROR;

  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  return refreshCanonicalUnlocked(hash, bookId, result);
}

BookOrbitCatalogClient::Error syncActiveHash(const std::string& hash, BookOrbitBookStateResult& result) {
  if (!KOREADER_STORE.hasCredentials()) return BookOrbitCatalogClient::NO_CREDENTIALS;
  if (!KOREADER_STORE.getCatalogCapable()) return BookOrbitCatalogClient::NOT_SUPPORTED;
  const SyncLock lock;
  if (!lock.owns()) return BookOrbitCatalogClient::NETWORK_ERROR;

  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  return syncHashUnlocked(hash, result, SyncMode::ActiveReader);
}

void drain() {
  if (!KOREADER_STORE.hasCredentials() || !KOREADER_STORE.getCatalogCapable()) return;
  const SyncLock lock;
  if (!lock.owns()) return;
  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  BookOrbitCatalogClient::beginPersistentSession();

  // Cursor and one State are the only per-book working data (one 32-byte hash
  // and one record). The SD queue is intentionally not materialized in memory.
  std::string cursor;
  bool includeCursor = true;
  for (;;) {
    BookOrbitBookStateStore::State state;
    if (!BookOrbitBookStateStore::nextPending(cursor, includeCursor, state)) break;
    const BookOrbitCatalogClient::Error error = sendPending(state, nullptr, SyncMode::Passive);
    if (error == BookOrbitCatalogClient::NOT_FOUND) {
      cursor = state.hash;
      includeCursor = false;
      LOG_DBG("BOState", "Unmatched state remains queued for %s", state.hash.c_str());
      continue;
    }
    if (error != BookOrbitCatalogClient::OK) {
      LOG_ERR("BOState", "State sync failed for %s (%s)", state.hash.c_str(),
              BookOrbitCatalogClient::errorString(error));
      break;
    }
    cursor = state.hash;
    includeCursor = true;
  }

  BookOrbitCatalogClient::endPersistentSession();
}

}  // namespace BookOrbitBookStateSync
