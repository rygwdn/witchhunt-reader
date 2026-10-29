#include "OpdsStatsSync.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointRoots.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>

#include <array>
#include <ctime>
#include <memory>
#include <new>

#include "CrossPointSettings.h"
#include "OpdsProgressSync.h"
#include "util/UrlUtils.h"

namespace {
constexpr const char* MEDIA_TYPE = "application/vnd.bookorbit.reading-stats+json";
constexpr size_t BATCH_SIZE = 8;
constexpr size_t MAX_BYTES = 8192;
std::string directoryFor(const OpdsServer& server) {
  uint64_t hash = 14695981039346656037ULL;
  const auto account = server.url + "\n" + server.username;
  for (unsigned char byte : account) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  char path[80];
  snprintf(path, sizeof(path), "/.crosspoint/opds-stats/%016llx", static_cast<unsigned long long>(hash));
  return path;
}
bool readRecord(const std::string& path, JsonDocument& record) {
  std::string body;
  return Storage.readFileToString("OPDS_STATS", path, MAX_BYTES, body) && !deserializeJson(record, body);
}
void timestamp(int64_t epoch, char* output, size_t size) {
  const time_t value = epoch;
  struct tm utc;
  gmtime_r(&value, &utc);
  strftime(output, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
}
struct Pending {
  std::string path, id;
};
struct UploadBatch {
  JsonDocument document;
  std::array<Pending, BATCH_SIZE> records;
  std::string url;
  size_t count = 0;
};
}  // namespace

void OpdsStatsSync::queue(const std::string& bookPath, const Session& session) {
  if (bookPath.empty() || !session.id || session.started < 1704067200 || session.ended <= session.started ||
      session.ended - session.started > 604800 || session.activeSeconds == 0 ||
      session.activeSeconds > static_cast<uint64_t>(session.ended - session.started))
    return;
  OpdsProgressSync::StatsAssociation association;
  if (!OpdsProgressSync::statsAssociation(bookPath, association)) return;
  OpdsServer account;
  account.url = association.server;
  account.username = association.username;
  const auto directory = directoryFor(account);
  if (!Storage.ensureDirectoryExists(directory.c_str())) return;
  const auto path = directory + "/" + session.id + ".json";
  if (Storage.exists(path.c_str())) return;
  JsonDocument record;
  record["server"] = association.server;
  record["username"] = association.username;
  record["url"] = association.url;
  record["device"]["id"] = OpdsProgressSync::deviceId();
  record["device"]["name"] = "Witchhunt Reader";
  auto value = record["session"].to<JsonObject>();
  value["id"] = session.id;
  char date[32];
  timestamp(session.started, date, sizeof(date));
  value["started"] = date;
  timestamp(session.ended, date, sizeof(date));
  value["ended"] = date;
  value["activeSeconds"] = session.activeSeconds;
  value["pagesTurned"] = session.pagesTurned;
  if (session.startProgress >= 0) value["startProgression"] = session.startProgress / 100.0f;
  if (session.endProgress >= 0) value["endProgression"] = session.endProgress / 100.0f;
  const auto temporary = path + ".tmp";
  HalFile file;
  if (!Storage.openFileForWrite("OPDS_STATS", temporary, file)) return;
  const bool complete = serializeJson(record, file) == measureJson(record);
  file.flush();
  file.close();
  if (!complete || !Storage.rename(temporary.c_str(), path.c_str())) {
    LOG_ERR("OPDS_STATS", "Could not persist reading session; temporary record retained");
  }
}

void OpdsStatsSync::flush(const OpdsServer& server) {
  if (WiFi.status() != WL_CONNECTED) return;
  const auto path = directoryFor(server);
  if (!Storage.exists(path.c_str())) return;
  // JSON plus eight acknowledgement paths exceed the reader task's small stack.
  // Allocate one bounded batch and one HTTP client for the entire upload pass.
  std::unique_ptr<UploadBatch> batch(new (std::nothrow) UploadBatch());
  if (!batch) {
    LOG_ERR("OPDS_STATS", "No memory for stats batch");
    return;
  }
  auto http = std::make_unique<crosspoint::SecureHttpClient>();
  http->setCACert(SETTINGS.skipHttpsValidation ? nullptr : CROSSPOINT_ROOTS_PEM);
  http->setAllowInsecureFallback(false);
  http->setAllowCertificateDateErrors(time(nullptr) < 1704067200);
  http->setMaxRedirects(0);
  http->setTimeout(10000);
  http->setMaxBufferedBody(MAX_BYTES);
  http->setBasicAuth(server.username, server.password);
  for (unsigned pass = 0; pass < 4; ++pass) {
    batch->document.clear();
    batch->count = 0;
    batch->url.clear();
    batch->document["version"] = 1;
    auto sessions = batch->document["sessions"].to<JsonArray>();
    auto directory = Storage.open(path.c_str());
    if (!directory || !directory.isDirectory()) return;
    unsigned inspected = 0;
    while (batch->count < BATCH_SIZE && inspected++ < 256) {
      auto file = directory.openNextFile();
      if (!file) break;
      char name[64];
      file.getName(name, sizeof(name));
      file.close();
      const auto filename = std::string(name);
      const auto recordPath = path + "/" + filename;
      JsonDocument record;
      if (filename.ends_with(".json.tmp")) {
        if (readRecord(recordPath, record)) {
          const auto destination = recordPath.substr(0, recordPath.size() - 4);
          if (Storage.exists(destination.c_str()))
            Storage.remove(recordPath.c_str());
          else
            Storage.rename(recordPath.c_str(), destination.c_str());
        }
        continue;
      }
      if (!filename.ends_with(".json") || !readRecord(recordPath, record)) continue;
      const auto url = record["url"].as<std::string>();
      if (record["server"].as<std::string>() != server.url || record["username"].as<std::string>() != server.username ||
          UrlUtils::extractHost(url) != UrlUtils::extractHost(server.url) || !record["session"]["id"].is<const char*>())
        continue;
      if (!batch->count) {
        batch->url = url;
        batch->document["device"] = record["device"];
      }
      if (batch->url != url ||
          batch->document["device"]["id"].as<std::string>() != record["device"]["id"].as<std::string>() ||
          batch->document["device"]["name"].as<std::string>() != record["device"]["name"].as<std::string>())
        continue;
      if (measureJson(batch->document) + measureJson(record["session"]) > 6144) break;
      sessions.add(record["session"]);
      auto& pending = batch->records[batch->count++];
      pending.path = recordPath;
      pending.id = record["session"]["id"].as<std::string>();
    }
    directory.close();
    if (!batch->count) return;
    std::string payload;
    payload.reserve(4096);
    serializeJson(batch->document, payload);
    http->clearHeaders();
    http->addHeader("Content-Type", MEDIA_TYPE);
    http->addHeader("Accept", MEDIA_TYPE);
    const int status = http->POST(batch->url, payload);
    if (status != 200 && status != 201) {
      LOG_ERR("OPDS_STATS", "Stats upload failed (HTTP %d); sessions retained for retry", status);
      return;
    }
    JsonDocument receipt;
    if (deserializeJson(receipt, http->getBody()) || receipt["version"].as<int>() != 1 ||
        !receipt["accepted"].is<JsonArray>() || receipt["accepted"].size() > batch->count)
      return;
    unsigned acknowledged = 0;
    for (const auto id : receipt["accepted"].as<JsonArray>()) {
      if (!id.is<const char*>()) continue;
      for (size_t i = 0; i < batch->count; ++i) {
        if (batch->records[i].id == id.as<const char*>() && Storage.remove(batch->records[i].path.c_str()))
          ++acknowledged;
      }
    }
    if (!acknowledged) return;
  }
}
