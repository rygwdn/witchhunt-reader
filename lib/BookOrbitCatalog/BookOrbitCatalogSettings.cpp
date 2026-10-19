#include "BookOrbitCatalogSettings.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdio>
#include <cstring>
#include <utility>

#include "../KOReaderSync/KOReaderCredentialStore.h"

namespace {
constexpr char DIRECTORY[] = "/.crosspoint/bookorbit";
// Worst case JSON escaping expands a source title to six bytes per input byte.
constexpr size_t MAX_SETTINGS_BYTES = 48 * 1024;
constexpr size_t MAX_ACCOUNT_VALUE = 192;
constexpr size_t MAX_TITLE_BYTES = 192;

struct Paths {
  std::string live;
  std::string staged;
  std::string backup;
};

void makePaths(Paths& paths, const std::string& baseUrl, const std::string& username) {
  MD5Builder md5;
  md5.begin();
  md5.add(baseUrl.c_str());
  md5.add("\n");
  md5.add(username.c_str());
  md5.calculate();
  const String digest = md5.toString();
  char filename[96];
  snprintf(filename, sizeof(filename), "%s/%s.json", DIRECTORY, digest.c_str());
  paths.live = filename;
  paths.staged = paths.live + ".tmp";
  paths.backup = paths.live + ".bak";
}

bool validSourceKind(uint8_t kind) { return kind <= static_cast<uint8_t>(BookOrbitCatalogSettings::SourceKind::SmartScope); }

bool readAccount(JsonVariantConst root, const char* key, const std::string& expected) {
  const JsonVariantConst value = root[key];
  return value.is<const char*>() && expected == value.as<const char*>();
}

bool validDocument(const std::string& data, const std::string& baseUrl, const std::string& username) {
  JsonDocument doc;
  if (deserializeJson(doc, data) || !readAccount(doc.as<JsonVariantConst>(), "baseUrl", baseUrl) ||
      !readAccount(doc.as<JsonVariantConst>(), "username", username)) return false;
  const JsonVariantConst supported = doc["supportedOnly"];
  const JsonVariantConst scope = doc["scope"];
  const JsonArrayConst sources = doc["sources"].as<JsonArrayConst>();
  if (!supported.is<bool>() || !scope.is<uint8_t>() || scope.as<uint8_t>() > 1 || sources.isNull() ||
      sources.size() > BookOrbitCatalogSettings::MAX_SOURCES) return false;
  for (const JsonVariantConst item : sources) {
    const JsonVariantConst kind = item["kind"];
    const JsonVariantConst id = item["id"];
    const JsonVariantConst title = item["title"];
    if (!kind.is<uint8_t>() || !validSourceKind(kind.as<uint8_t>()) || !id.is<int32_t>() || id.as<int32_t>() <= 0 ||
        !title.is<const char*>() || title.as<const char*>()[0] == '\0' ||
        strlen(title.as<const char*>()) > MAX_TITLE_BYTES) return false;
  }
  return true;
}

bool validFile(const std::string& path, const std::string& baseUrl, const std::string& username) {
  std::string data;
  return Storage.readFileToString("BOOKORBIT", path, MAX_SETTINGS_BYTES, data) &&
         validDocument(data, baseUrl, username);
}

// Recover only files whose embedded identity and complete schema match this account. An unreadable
// live/backup file fails closed; it is never treated as absent and replaced with broad defaults.
bool recoverFiles(const Paths& paths, const std::string& baseUrl, const std::string& username) {
  bool liveExists = Storage.exists(paths.live.c_str());
  const bool backupExists = Storage.exists(paths.backup.c_str());
  bool liveValid = liveExists && validFile(paths.live, baseUrl, username);
  if (backupExists) {
    const bool backupValid = validFile(paths.backup, baseUrl, username);
    if (liveValid && backupValid) {
      if (!Storage.remove(paths.backup.c_str())) return false;
    } else if (backupValid) {
      if (liveExists && !Storage.remove(paths.live.c_str())) return false;
      if (!Storage.rename(paths.backup.c_str(), paths.live.c_str())) return false;
      liveExists = true;
      liveValid = true;
    } else {
      return false;
    }
  }
  const bool stagedExists = Storage.exists(paths.staged.c_str());
  if (liveValid) {
    if (stagedExists && !Storage.remove(paths.staged.c_str())) return false;
    return true;
  }
  if (stagedExists && validFile(paths.staged, baseUrl, username)) {
    if (liveExists && !Storage.remove(paths.live.c_str())) return false;
    if (!Storage.rename(paths.staged.c_str(), paths.live.c_str())) return false;
    return validFile(paths.live, baseUrl, username);
  }
  if (liveExists) return false;
  if (stagedExists && !Storage.remove(paths.staged.c_str())) return false;
  return true;
}

bool writeStaged(const std::string& path, const String& serialized) {
  HalFile file;
  if (!Storage.openFileForWrite("BOOKORBIT", path, file)) return false;
  const size_t length = serialized.length();
  const size_t written = file.write(serialized.c_str(), length);
  file.flush();
  const bool closed = file.close();
  return written == length && closed;
}
}  // namespace


bool BookOrbitCatalogSettings::load() {
  if (!Storage.ready()) {
    LOG_ERR("BOOKORBIT", "SD storage is not ready");
    return false;
  }
  const std::string baseUrl = KOREADER_STORE.getBaseUrl();
  const std::string username = KOREADER_STORE.getUsername();
  if (baseUrl.size() > MAX_ACCOUNT_VALUE || username.size() > MAX_ACCOUNT_VALUE) return false;
  Paths paths;
  makePaths(paths, baseUrl, username);
  if (!recoverFiles(paths, baseUrl, username)) {
    LOG_ERR("BOOKORBIT", "Could not recover catalog settings");
    return false;
  }
  if (!Storage.exists(paths.live.c_str())) {
    supportedOnly = true;
    scope = Scope::All;
    sources.clear();
    return true;
  }
  std::string data;
  if (!Storage.readFileToString("BOOKORBIT", paths.live, MAX_SETTINGS_BYTES, data) ||
      !validDocument(data, baseUrl, username)) {
    LOG_ERR("BOOKORBIT", "Catalog settings are malformed or belong to another account");
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, data)) return false;
  std::vector<Source> loaded;
  const JsonArrayConst savedSources = doc["sources"].as<JsonArrayConst>();
  loaded.reserve(savedSources.size());
  for (const JsonVariantConst item : savedSources) {
    loaded.push_back({static_cast<SourceKind>(item["kind"].as<uint8_t>()), item["id"].as<int32_t>(),
                      item["title"].as<const char*>()});
  }
  supportedOnly = doc["supportedOnly"].as<bool>();
  scope = static_cast<Scope>(doc["scope"].as<uint8_t>());
  sources = std::move(loaded);
  return true;
}

bool BookOrbitCatalogSettings::save() const {
  if (!Storage.ready()) {
    LOG_ERR("BOOKORBIT", "SD storage is not ready");
    return false;
  }
  const std::string baseUrl = KOREADER_STORE.getBaseUrl();
  const std::string username = KOREADER_STORE.getUsername();
  if (baseUrl.size() > MAX_ACCOUNT_VALUE || username.size() > MAX_ACCOUNT_VALUE || sources.size() > MAX_SOURCES ||
      static_cast<uint8_t>(scope) > static_cast<uint8_t>(Scope::Selected)) return false;
  for (const Source& source : sources) {
    if (source.id <= 0 || source.title.empty() || source.title.size() > MAX_TITLE_BYTES ||
        !validSourceKind(static_cast<uint8_t>(source.kind))) return false;
  }
  JsonDocument doc;
  doc["baseUrl"] = baseUrl;
  doc["username"] = username;
  doc["supportedOnly"] = supportedOnly;
  doc["scope"] = static_cast<uint8_t>(scope);
  JsonArray savedSources = doc["sources"].to<JsonArray>();
  for (const Source& source : sources) {
    JsonObject item = savedSources.add<JsonObject>();
    item["kind"] = static_cast<uint8_t>(source.kind);
    item["id"] = source.id;
    item["title"] = source.title;
  }
  String serialized;
  if (serializeJson(doc, serialized) == 0 || serialized.length() > MAX_SETTINGS_BYTES) return false;
  if (!Storage.ensureDirectoryExists(DIRECTORY)) return false;
  Paths paths;
  makePaths(paths, baseUrl, username);
  if (!recoverFiles(paths, baseUrl, username)) return false;
  if (!writeStaged(paths.staged, serialized)) {
    Storage.remove(paths.staged.c_str());
    LOG_ERR("BOOKORBIT", "Could not write staged catalog settings");
    return false;
  }
  std::string stagedData;
  if (!Storage.readFileToString("BOOKORBIT", paths.staged, MAX_SETTINGS_BYTES, stagedData) ||
      stagedData != serialized.c_str() || !validDocument(stagedData, baseUrl, username)) {
    Storage.remove(paths.staged.c_str());
    LOG_ERR("BOOKORBIT", "Staged catalog settings failed validation");
    return false;
  }
  if (!Storage.publishStagedFileWithBackup(paths.staged.c_str(), paths.live.c_str(), paths.backup.c_str())) {
    LOG_ERR("BOOKORBIT", "Could not publish catalog settings");
    return false;
  }
  if (!validFile(paths.live, baseUrl, username)) {
    Storage.remove(paths.live.c_str());
    if (Storage.exists(paths.backup.c_str())) Storage.rename(paths.backup.c_str(), paths.live.c_str());
    LOG_ERR("BOOKORBIT", "Published catalog settings failed validation");
    return false;
  }
  if (Storage.exists(paths.backup.c_str()) && !Storage.remove(paths.backup.c_str())) {
    LOG_ERR("BOOKORBIT", "Could not remove catalog settings backup");
    return false;
  }
  return true;
}
