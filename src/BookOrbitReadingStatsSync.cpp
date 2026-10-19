#include "BookOrbitReadingStatsSync.h"

#include <ArduinoJson.h>
#include <BookOrbitCatalogClient.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>

#include <atomic>
#include <string>
#include <vector>
#include <utility>

#include "BookOrbitReadingStatsOutbox.h"
#include "CrossPointSettings.h"


namespace {
std::atomic_flag drainActive = ATOMIC_FLAG_INIT;

class DrainLock {
 public:
  DrainLock() : acquired(!drainActive.test_and_set(std::memory_order_acquire)) {}
  ~DrainLock() {
    if (acquired) drainActive.clear(std::memory_order_release);
  }
  bool owns() const { return acquired; }

 private:
  bool acquired;
};
// Validate the server acknowledgment before the queue removes any event.
enum class BatchAcknowledgment { Accepted, Unmatched, Invalid };

BatchAcknowledgment acknowledgeResponse(const std::string& response, const BookOrbitPageStatsBook& batch) {
  JsonDocument doc;
  if (deserializeJson(doc, response) || !doc.is<JsonObjectConst>()) return BatchAcknowledgment::Invalid;

  const JsonArrayConst unmatched = doc["unmatched"].as<JsonArrayConst>();
  const JsonArrayConst results = doc["results"].as<JsonArrayConst>();
  if (unmatched.isNull() || results.isNull()) return BatchAcknowledgment::Invalid;
  if (unmatched.size() == 1 && results.size() == 0) {
    const char* hash = unmatched[0].as<const char*>();
    return hash && batch.hash == hash ? BatchAcknowledgment::Unmatched : BatchAcknowledgment::Invalid;
  }
  if (unmatched.size() != 0 || results.size() != 1) return BatchAcknowledgment::Invalid;
  const JsonObjectConst result = results[0].as<JsonObjectConst>();
  const char* responseHash = result["hash"].as<const char*>();
  if (!responseHash || batch.hash != responseHash) return BatchAcknowledgment::Invalid;
  if (!result["accepted"].is<uint32_t>() || !result["duplicates"].is<uint32_t>() ||
      !result["watermark"].is<uint32_t>()) {
    return BatchAcknowledgment::Invalid;
  }

  const uint32_t accepted = result["accepted"].as<uint32_t>();
  const uint32_t duplicates = result["duplicates"].as<uint32_t>();
  if (static_cast<uint64_t>(accepted) + duplicates != batch.events.size()) return BatchAcknowledgment::Invalid;
  uint32_t latest = 0;
  for (const BookOrbitPageStatEvent& event : batch.events) {
    if (event.startTime > latest) latest = event.startTime;
  }
  return result["watermark"].as<uint32_t>() >= latest ? BatchAcknowledgment::Accepted : BatchAcknowledgment::Invalid;
}
}  // namespace

namespace BookOrbitReadingStatsSync {

void drain() {
  if (!KOREADER_STORE.hasCredentials() || !KOREADER_STORE.getCatalogCapable()) return;
  const DrainLock lock;
  if (!lock.owns()) return;
  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  BookOrbitCatalogClient::beginPersistentSession();

  std::string cursor;
  bool includeCursor = true;
  for (;;) {
    BookOrbitPageStatsBook batch;
    if (!BookOrbitReadingStatsOutbox::nextBatch(batch, cursor, includeCursor)) break;
    std::vector<BookOrbitPageStatsBook> uploads;
    uploads.reserve(1);
    uploads.push_back(std::move(batch));
    const BookOrbitPageStatsBook& current = uploads.front();
    std::string response;
    const auto result = BookOrbitCatalogClient::uploadPageStats(uploads, response);
    if (result != BookOrbitCatalogClient::OK) {
      LOG_ERR("BOStats", "Page-stat upload failed (%s); events remain queued",
              BookOrbitCatalogClient::errorString(result));
      break;
    }
    const BatchAcknowledgment acknowledgment = acknowledgeResponse(response, current);
    if (acknowledgment != BatchAcknowledgment::Accepted) {
      cursor = current.hash;
      includeCursor = false;
      LOG_INF("BOStats", "%s page-stat book; skipping it for this drain",
              acknowledgment == BatchAcknowledgment::Unmatched ? "Unmatched" : "Unacknowledged");
      continue;
    }
    if (!BookOrbitReadingStatsOutbox::acknowledge(current.hash, current.events)) {
      LOG_ERR("BOStats", "Could not retire acknowledged page-stat events; retry is idempotent");
      break;
    }
    cursor = current.hash;
    includeCursor = true;
  }

  BookOrbitCatalogClient::endPersistentSession();

}
}  // namespace BookOrbitReadingStatsSync
