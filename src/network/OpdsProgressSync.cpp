#include "OpdsProgressSync.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointRoots.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SecureHttpClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sys/time.h>

#include <cmath>
#include <ctime>
#include <memory>

#include "CrossPointSettings.h"
#include "OpdsStatsSync.h"
#include "OpdsSyncPolicy.h"
#include "util/UrlUtils.h"

namespace {
constexpr const char* DIRECTORY = "/.crosspoint/opds-sync";
constexpr size_t MAX_DOCUMENT = 8192;
// Protect only short SD mutations. Never hold this lock across HTTP requests.
StaticSemaphore_t outboxMutexStorage;
SemaphoreHandle_t outboxMutex = xSemaphoreCreateMutexStatic(&outboxMutexStorage);
class OutboxLock {
 public:
  OutboxLock() { xSemaphoreTake(outboxMutex, portMAX_DELAY); }
  ~OutboxLock() { xSemaphoreGive(outboxMutex); }
};
std::string hashName(const std::string& value) {
  uint64_t hash = 14695981039346656037ULL;
  for (unsigned char c : value) {
    hash ^= c;
    hash *= 1099511628211ULL;
  }
  char name[24];
  snprintf(name, sizeof(name), "%016llx", static_cast<unsigned long long>(hash));
  return name;
}
std::string pathFor(const std::string& path, const char* suffix) {
  return std::string(DIRECTORY) + "/links/" + hashName(path) + "." + suffix + ".json";
}
std::string accountDirectory(const std::string& server, const std::string& username) {
  return std::string(DIRECTORY) + "/outbox/" + hashName(server + "\n" + username);
}
std::string pendingPath(const std::string& bookPath, const JsonDocument& mapping) {
  return accountDirectory(mapping["server"].as<std::string>(), mapping["username"].as<std::string>()) + "/" +
         hashName(bookPath) + ".pending.json";
}

bool read(const std::string& path, JsonDocument& document) {
  std::string body;
  if (Storage.readFileToString("OPDS_SYNC", path, MAX_DOCUMENT, body) && !deserializeJson(document, body)) return true;
  const auto backup = path + ".backup";
  return Storage.exists(backup.c_str()) && Storage.readFileToString("OPDS_SYNC", backup, MAX_DOCUMENT, body) &&
         !deserializeJson(document, body);
}

bool write(const std::string& path, const JsonDocument& document) {
  if (measureJson(document) > MAX_DOCUMENT) return false;
  HalFile file;
  const auto temp = path + ".tmp";
  if (!Storage.openFileForWrite("OPDS_SYNC", temp, file)) return false;
  const bool written = serializeJson(document, file) == measureJson(document);
  file.flush();
  file.close();
  if (!written) {
    Storage.remove(temp.c_str());
    return false;
  }
  const auto backup = path + ".backup";
  Storage.remove(backup.c_str());
  const bool existed = Storage.exists(path.c_str());
  if (existed && !Storage.rename(path.c_str(), backup.c_str())) return false;
  if (!Storage.rename(temp.c_str(), path.c_str())) {
    if (existed) Storage.rename(backup.c_str(), path.c_str());
    return false;
  }
  Storage.remove(backup.c_str());
  return true;
}
}  // namespace

bool OpdsProgressSync::associate(const std::string& bookPath, const OpdsServer& server, const std::string& url,
                                 const std::string& statsUrl) {
  if (url.empty() && statsUrl.empty()) return false;
  if ((!url.empty() && UrlUtils::extractHost(url) != UrlUtils::extractHost(server.url)) ||
      (!statsUrl.empty() && UrlUtils::extractHost(statsUrl) != UrlUtils::extractHost(server.url)))
    return false;
  if (!Storage.ensureDirectoryExists((std::string(DIRECTORY) + "/links").c_str())) return false;
  JsonDocument document;
  document["server"] = server.url;
  document["username"] = server.username;
  document["url"] = url;
  if (!statsUrl.empty()) document["statsUrl"] = statsUrl;
  return write(pathFor(bookPath, "link"), document);
}

int64_t OpdsProgressSync::nowMilliseconds() {
  struct timeval now;
  gettimeofday(&now, nullptr);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_usec / 1000;
}

void OpdsProgressSync::acceptRemote(const std::string& bookPath) {
  const OutboxLock lock;
  JsonDocument mapping;
  if (!read(pathFor(bookPath, "link"), mapping)) return;
  const auto pending = pendingPath(bookPath, mapping);
  Storage.remove(pending.c_str());
  Storage.remove((pending + ".backup").c_str());
  const auto slash = pending.find_last_of('/');
  const auto conflict = pending.substr(0, slash) + "/conflicts/" + pending.substr(slash + 1);
  Storage.remove(conflict.c_str());
}

bool OpdsProgressSync::queue(const std::string& bookPath, float progression, const std::string& reference,
                             int64_t readAt) {
  const OutboxLock lock;
  if (!std::isfinite(progression) || progression < 0 || progression > 1) return false;
  const auto mapping = pathFor(bookPath, "link");
  if (!associated(bookPath)) return false;
  JsonDocument document;
  if (!read(mapping, document)) return false;
  const time_t now = readAt / 1000;
  if (now < 1704067200) {
    LOG_ERR("OPDS_SYNC", "Clock is not set; refusing to invent a progress timestamp");
    return false;
  }
  struct tm utc;
  gmtime_r(&now, &utc);
  char timestamp[32];
  strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &utc);
  snprintf(timestamp + 19, sizeof(timestamp) - 19, ".%03dZ", static_cast<int>(readAt % 1000));
  // Closing the reader can replay its last page after a newer completion confirmation.
  JsonDocument pending;
  const auto existing = pendingPath(bookPath, document);
  if (read(existing, pending)) {
    const std::string previous = pending["progress"]["modified"] | "";
    if (!OpdsSyncPolicy::replacesProgress(previous, pending["progress"]["progression"].as<float>(), timestamp,
                                          progression))
      return true;
  }
  auto progress = document["progress"].to<JsonObject>();
  progress["modified"] = timestamp;
  progress["device"]["id"] = deviceId();
  progress["device"]["name"] = "Witchhunt Reader";
  progress["progression"] = progression;
  if (!reference.empty()) progress["references"].to<JsonArray>().add(reference);
  const auto destination = pendingPath(bookPath, document);
  const auto directory = destination.substr(0, destination.find_last_of('/'));
  if (!Storage.ensureDirectoryExists(directory.c_str()) || !write(destination, document)) {
    LOG_ERR("OPDS_SYNC", "Could not persist queued progress");
    return false;
  }
  return true;
}

bool OpdsProgressSync::flush(const OpdsServer& server, bool* failed) {
  if (failed) *failed = false;
  if (WiFi.status() != WL_CONNECTED) {
    if (failed) *failed = true;
    return false;
  }
  OpdsStatsSync::flush(server);
  const auto accountPath = accountDirectory(server.url, server.username);
  auto directory = Storage.open(accountPath.c_str());
  if (!directory || !directory.isDirectory()) return false;
  bool conflictFound = false;
  const auto conflictDirPath = accountPath + "/conflicts";
  auto conflicts = Storage.open(conflictDirPath.c_str());
  if (conflicts && conflicts.isDirectory()) {
    auto first = conflicts.openNextFile();
    conflictFound = static_cast<bool>(first);
    first.close();
  }
  conflicts.close();
  // A single client reuses its TLS connection. Bounded batch avoids blocking catalog opening indefinitely.
  auto http = std::make_unique<crosspoint::SecureHttpClient>();
  http->setCACert(SETTINGS.skipHttpsValidation ? nullptr : CROSSPOINT_ROOTS_PEM);
  http->setAllowInsecureFallback(false);
  http->setAllowCertificateDateErrors(time(nullptr) < 1704067200);
  http->setMaxRedirects(0);
  http->setTimeout(10000);
  http->setMaxResponseBodySize(MAX_DOCUMENT);
  http->setBasicAuth(server.username, server.password);
  unsigned sent = 0;
  unsigned inspected = 0;
  while (sent < 8 && inspected++ < 256) {
    auto file = directory.openNextFile();
    if (!file) break;
    char name[96];
    file.getName(name, sizeof(name));
    file.close();
    const std::string filename(name);
    if (filename.ends_with(".pending.json.backup")) {
      const OutboxLock lock;
      const auto backup = accountPath + "/" + filename;
      const auto original = backup.substr(0, backup.size() - 7);
      if (!Storage.exists(original.c_str())) Storage.rename(backup.c_str(), original.c_str());
      continue;
    }
    if (!filename.ends_with(".pending.json")) continue;
    const std::string path = accountPath + "/" + filename;
    JsonDocument document;
    {
      const OutboxLock lock;
      if (!read(path, document)) continue;
    }
    if (document["server"].as<std::string>() != server.url || document["username"].as<std::string>() != server.username)
      continue;
    const auto url = document["url"].as<std::string>();
    if (UrlUtils::extractHost(url) != UrlUtils::extractHost(server.url)) continue;
    std::string payload;
    payload.reserve(1024);
    serializeJson(document["progress"], payload);
    http->clearHeaders();
    http->addHeader("Content-Type", "application/opds-progression+json");
    http->addHeader("Accept", "application/opds-progression+json");
    const int status = http->PUT(url, payload);
    ++sent;
    const OutboxLock lock;
    JsonDocument latest;
    if (!read(path, latest) ||
        !OpdsSyncPolicy::matchesQueuedProgress(
            document["progress"]["modified"].as<std::string>(), document["progress"]["progression"].as<float>(),
            latest["progress"]["modified"].as<std::string>(), latest["progress"]["progression"].as<float>()))
      continue;
    if (status == 200 || status == 201) {
      JsonDocument response;
      if (!deserializeJson(response, http->getBody()) && response["progression"].is<float>() &&
          response["modified"].is<const char*>()) {
        Storage.remove(path.c_str());
        Storage.remove((path + ".backup").c_str());
      } else if (failed) {
        *failed = true;
      }
    } else if (status == 409) {
      conflictFound = true;
      Storage.ensureDirectoryExists(conflictDirPath.c_str());
      const auto conflict = conflictDirPath + "/" + filename;
      Storage.remove(conflict.c_str());
      Storage.rename(path.c_str(), conflict.c_str());
      LOG_INF("OPDS_SYNC", "Newer server position exists; retained local position as a conflict");
    } else {
      if (failed) *failed = true;
      LOG_ERR("OPDS_SYNC", "Progress upload failed (HTTP %d); retained for retry", status);
      break;
    }
  }
  directory.close();
  return conflictFound;
}

bool OpdsProgressSync::associated(const std::string& bookPath) {
  const auto mapping = pathFor(bookPath, "link");
  return Storage.exists(mapping.c_str()) || Storage.exists((mapping + ".backup").c_str());
}

bool OpdsProgressSync::fetch(const std::string& bookPath, Remote& remote, int* status) {
  if (status) *status = 0;
  JsonDocument mapping;
  if (!read(pathFor(bookPath, "link"), mapping)) return false;
  const auto url = mapping["url"].as<std::string>();
  const auto serverUrl = mapping["server"].as<std::string>();
  const auto username = mapping["username"].as<std::string>();
  const OpdsServer* account = nullptr;
  for (const auto& server : OPDS_STORE.getServers()) {
    if (server.url == serverUrl && server.username == username) {
      account = &server;
      break;
    }
  }
  if (!account || UrlUtils::extractHost(url) != UrlUtils::extractHost(account->url)) return false;
  auto http = std::make_unique<crosspoint::SecureHttpClient>();
  http->setCACert(SETTINGS.skipHttpsValidation ? nullptr : CROSSPOINT_ROOTS_PEM);
  http->setAllowInsecureFallback(false);
  http->setAllowCertificateDateErrors(time(nullptr) < 1704067200);
  http->setMaxRedirects(0);
  http->setTimeout(10000);
  http->setMaxResponseBodySize(MAX_DOCUMENT);
  http->setBasicAuth(account->username, account->password);
  http->addHeader("Accept", "application/opds-progression+json");
  const int responseStatus = http->GET(url);
  if (status) *status = responseStatus;
  if (responseStatus != 200) return false;
  // The progression draft represents an unread book with an empty HTTP 200.
  // Normalize that to no-content for the scheduler, rather than a JSON failure.
  if (http->getBody().empty()) {
    if (status) *status = 204;
    return false;
  }
  JsonDocument document;
  if (deserializeJson(document, http->getBody()) || !document["progression"].is<float>() ||
      !document["modified"].is<const char*>() || !document["device"]["id"].is<const char*>() ||
      !document["device"]["name"].is<const char*>())
    return false;
  remote.progression = document["progression"].as<float>();
  if (!std::isfinite(remote.progression) || remote.progression < 0 || remote.progression > 1) return false;
  remote.modified = document["modified"].as<std::string>();
  remote.deviceName = document["device"]["name"].as<std::string>();
  for (const auto reference : document["references"].as<JsonArray>()) {
    if (reference.is<const char*>()) {
      remote.reference = reference.as<std::string>();
      break;
    }
  }
  return true;
}

std::string OpdsProgressSync::deviceId() {
  char id[48];
  snprintf(id, sizeof(id), "urn:witchhunt:%012llx", static_cast<unsigned long long>(ESP.getEfuseMac()));
  return id;
}

bool OpdsProgressSync::statsAssociation(const std::string& bookPath, StatsAssociation& association) {
  const auto path = pathFor(bookPath, "link");
  if (!Storage.exists(path.c_str()) && !Storage.exists((path + ".backup").c_str())) return false;
  JsonDocument document;
  if (!read(path, document)) return false;
  association.server = document["server"].as<std::string>();
  association.username = document["username"].as<std::string>();
  association.url = document["statsUrl"].as<std::string>();
  return !association.url.empty() &&
         UrlUtils::extractHost(association.url) == UrlUtils::extractHost(association.server);
}

bool OpdsProgressSync::flushForBook(const std::string& bookPath) {
  if (WiFi.status() != WL_CONNECTED) return false;
  JsonDocument mapping;
  if (!read(pathFor(bookPath, "link"), mapping)) return false;
  const auto serverUrl = mapping["server"].as<std::string>();
  const auto username = mapping["username"].as<std::string>();
  for (const auto& server : OPDS_STORE.getServers()) {
    if (server.url != serverUrl || server.username != username) continue;
    bool failed = false;
    const bool conflict = flush(server, &failed);
    const OutboxLock lock;
    const auto pending = pendingPath(bookPath, mapping);
    const auto slash = pending.find_last_of('/');
    const auto conflicted = pending.substr(0, slash) + "/conflicts/" + pending.substr(slash + 1);
    return !failed && !conflict && !Storage.exists(pending.c_str()) && !Storage.exists((pending + ".backup").c_str()) &&
           !Storage.exists(conflicted.c_str());
  }
  return false;
}

bool OpdsProgressSync::copyAssociation(const std::string& from, const std::string& to) {
  const OutboxLock lock;
  JsonDocument mapping;
  return read(pathFor(from, "link"), mapping) && write(pathFor(to, "link"), mapping);
}
