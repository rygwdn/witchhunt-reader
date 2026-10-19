#include "BookOrbitSourcePickerActivity.h"

#include <I18n.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <utility>

#include "CrossPointSettings.h"
#include <BookOrbitCatalogClient.h>
#include "../../../lib/KOReaderSync/KOReaderCredentialStore.h"
#include "activities/network/WifiSelectionActivity.h"
#include "MappedInputManager.h"

namespace fui = freeink::ui;

namespace {
constexpr char CANDIDATE_FILE[] = "/.crosspoint/bookorbit/source-candidates.tmp";
constexpr size_t CANDIDATE_TITLE_BYTES = 192;
constexpr size_t CANDIDATE_RECORD_BYTES = 6 + CANDIDATE_TITLE_BYTES;
constexpr uint64_t CANDIDATES_PER_PAGE = 20;
constexpr int ROW_PREVIOUS = -1;
constexpr int ROW_NEXT = -2;

int32_t sourceIdFromHref(const std::string& href, const char* key) {
  const size_t keyLength = strlen(key);
  size_t at = href.find(key);
  if (at == std::string::npos || at + keyLength >= href.size() || href[at + keyLength] != '=') return -1;
  at += keyLength + 1;
  char* end = nullptr;
  const long value = strtol(href.c_str() + at, &end, 10);
  if (end == href.c_str() + at || value <= 0 || value > INT32_MAX || (*end != '&' && *end != '\0')) return -1;
  return static_cast<int32_t>(value);
}

const char* sectionName(BookOrbitCatalogSettings::SourceKind kind) {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library: return "libraries";
    case BookOrbitCatalogSettings::SourceKind::Collection: return "collections";
    case BookOrbitCatalogSettings::SourceKind::SmartScope: return "smart-scopes";
  }
  return "libraries";
}

void encodeCandidate(uint8_t record[CANDIDATE_RECORD_BYTES], int32_t id, std::string_view title) {
  const uint32_t bits = static_cast<uint32_t>(id);
  const uint16_t length = static_cast<uint16_t>(title.size());
  record[0] = static_cast<uint8_t>(bits);
  record[1] = static_cast<uint8_t>(bits >> 8);
  record[2] = static_cast<uint8_t>(bits >> 16);
  record[3] = static_cast<uint8_t>(bits >> 24);
  record[4] = static_cast<uint8_t>(length);
  record[5] = static_cast<uint8_t>(length >> 8);
  memcpy(record + 6, title.data(), title.size());
  memset(record + 6 + title.size(), 0, CANDIDATE_TITLE_BYTES - title.size());
}
}  // namespace

BookOrbitSourcePickerActivity::BookOrbitSourcePickerActivity(GfxRenderer& renderer,
                                                             MappedInputManager& mappedInput,
                                                             BookOrbitCatalogSettings::SourceKind sourceKind)
    : UiListActivity("BookOrbitSources", renderer, mappedInput), kind(sourceKind) {
  candidateWindow.reserve(CANDIDATES_PER_PAGE);
  rows.reserve(BookOrbitCatalogSettings::MAX_SOURCES + CANDIDATES_PER_PAGE + 3);
}

void BookOrbitSourcePickerActivity::onEnter() {
  UiListActivity::onEnter();
  if (!settings.load()) {
    error = tr(STR_BOOKORBIT_LOAD_ERROR);
    requestUpdate();
    return;
  }
  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/catalog/sections/" + sectionName(kind);
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    fetch(url);
    return;
  }
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this, url](const ActivityResult& result) {
                           if (result.isCancelled) {
                             error = tr(STR_BOOKORBIT_NETWORK_ERROR);
                             requestUpdate();
                             return;
                           }
                           fetch(url);
                         });
}

void BookOrbitSourcePickerActivity::onExit() {
  if (candidateFile.isOpen()) candidateFile.close();
  Storage.remove(CANDIDATE_FILE);
  UiListActivity::onExit();
}

void BookOrbitSourcePickerActivity::fetch(const std::string& url) {
  if (candidateFile.isOpen()) candidateFile.close();
  Storage.remove(CANDIDATE_FILE);
  candidateCount = 0;
  pageStart = 0;
  candidateWindow.clear();
  error.clear();
  if (!Storage.ensureDirectoryExists("/.crosspoint/bookorbit")) {
    error = tr(STR_BOOKORBIT_STORAGE_ERROR);
    requestUpdate();
    return;
  }
  HalFile spool;
  if (!Storage.openFileForWrite("BOOKORBIT", CANDIDATE_FILE, spool)) {
    error = tr(STR_BOOKORBIT_STORAGE_ERROR);
    requestUpdate();
    return;
  }
  bool writeFailed = false;
  BookOrbitPageInfo page;
  const auto result = BookOrbitCatalogClient::fetchListing(url, [&](const BookOrbitEntry& entry) {
    if (writeFailed || entry.type != BookOrbitEntryType::NAVIGATION) return;
    const int32_t id = sourceIdFromHref(entry.href, BookOrbitCatalogSettings::sourceQueryKey(kind));
    if (id <= 0) return;
    if (candidateCount == UINT64_MAX) {
      writeFailed = true;
      return;
    }
    std::string_view title = entry.title.empty() ? std::string_view(tr(STR_UNNAMED)) : std::string_view(entry.title);
    if (title.size() > CANDIDATE_TITLE_BYTES) {
      size_t length = CANDIDATE_TITLE_BYTES;
      while (length > 0 && (static_cast<unsigned char>(title[length]) & 0xC0) == 0x80) --length;
      title = title.substr(0, length);
    }
    uint8_t record[CANDIDATE_RECORD_BYTES];
    encodeCandidate(record, id, title);
    if (spool.write(record, sizeof(record)) != sizeof(record)) {
      writeFailed = true;
      return;
    }
    ++candidateCount;
  }, page);
  spool.flush();
  const bool closed = spool.close();
  if (result != BookOrbitCatalogClient::OK) {
    Storage.remove(CANDIDATE_FILE);
    candidateCount = 0;
    error = tr(STR_BOOKORBIT_NETWORK_ERROR);
  } else if (writeFailed || !closed) {
    Storage.remove(CANDIDATE_FILE);
    candidateCount = 0;
    error = tr(STR_BOOKORBIT_STORAGE_ERROR);
  } else if (!Storage.openFileForRead("BOOKORBIT", CANDIDATE_FILE, candidateFile)) {
    candidateCount = 0;
    error = tr(STR_BOOKORBIT_STORAGE_ERROR);
  } else if (!candidateCount && !hasSavedSource()) {
    error = tr(STR_BOOKORBIT_NO_SOURCES);
  }
  if (error.empty() && !loadCandidateWindow()) error = tr(STR_BOOKORBIT_STORAGE_ERROR);
  requestUpdate();
}

bool BookOrbitSourcePickerActivity::hasSavedSource() const {
  return std::any_of(settings.sources.begin(), settings.sources.end(),
                     [&](const auto& source) { return source.kind == kind; });
}

bool BookOrbitSourcePickerActivity::loadCandidateWindow() {
  candidateWindow.clear();
  const uint64_t end = std::min(candidateCount, pageStart + CANDIDATES_PER_PAGE);
  for (uint64_t index = pageStart; index < end; ++index) {
    uint8_t record[CANDIDATE_RECORD_BYTES];
    const uint64_t offset = index * CANDIDATE_RECORD_BYTES;
    if (!candidateFile.seek64(offset) || candidateFile.read(record, sizeof(record)) != sizeof(record)) {
      candidateWindow.clear();
      return false;
    }
    const uint32_t id = static_cast<uint32_t>(record[0]) | (static_cast<uint32_t>(record[1]) << 8) |
                        (static_cast<uint32_t>(record[2]) << 16) | (static_cast<uint32_t>(record[3]) << 24);
    const uint16_t length = static_cast<uint16_t>(record[4]) | (static_cast<uint16_t>(record[5]) << 8);
    if (!id || length == 0 || length > CANDIDATE_TITLE_BYTES) {
      candidateWindow.clear();
      return false;
    }
    candidateWindow.push_back({kind, static_cast<int32_t>(id),
                               std::string(reinterpret_cast<const char*>(record + 6), length)});
  }
  return true;
}

int BookOrbitSourcePickerActivity::listCount() const {
  const int saved = static_cast<int>(std::count_if(settings.sources.begin(), settings.sources.end(),
                                                   [&](const auto& source) { return source.kind == kind; }));
  const int available = static_cast<int>(std::count_if(candidateWindow.begin(), candidateWindow.end(), [&](const auto& candidate) {
    return std::none_of(settings.sources.begin(), settings.sources.end(), [&](const auto& source) {
      return source.kind == candidate.kind && source.id == candidate.id;
    });
  }));
  return saved + available + (pageStart > 0 ? 1 : 0) + (candidateCount - pageStart > CANDIDATES_PER_PAGE ? 1 : 0) +
         (!error.empty() ? 1 : 0);
}

const char* BookOrbitSourcePickerActivity::headerTitle() const {
  switch (kind) {
    case BookOrbitCatalogSettings::SourceKind::Library: return tr(STR_BOOKORBIT_LIBRARIES);
    case BookOrbitCatalogSettings::SourceKind::Collection: return tr(STR_BOOKORBIT_COLLECTIONS);
    case BookOrbitCatalogSettings::SourceKind::SmartScope: return tr(STR_BOOKORBIT_SMART_SCOPES);
  }
  return tr(STR_BOOKORBIT_CATALOG_SETTINGS);
}

void BookOrbitSourcePickerActivity::buildScreen(UiScreen& screen) {
  rows.clear();
  for (size_t sourceIndex = 0; sourceIndex < settings.sources.size(); ++sourceIndex) {
    const auto& source = settings.sources[sourceIndex];
    if (source.kind != kind) continue;
    fui::ListItem item;
    item.label = source.title.c_str();
    item.value = tr(STR_SELECTED);
    item.actionValue = static_cast<int16_t>(-100 - static_cast<int>(sourceIndex));
    rows.push_back(item);
  }
  for (size_t candidateIndex = 0; candidateIndex < candidateWindow.size(); ++candidateIndex) {
    const auto& candidate = candidateWindow[candidateIndex];
    if (std::any_of(settings.sources.begin(), settings.sources.end(), [&](const auto& source) {
          return source.kind == candidate.kind && source.id == candidate.id;
        })) continue;
    fui::ListItem item;
    item.label = candidate.title.c_str();
    item.actionValue = static_cast<int16_t>(candidateIndex);
    rows.push_back(item);
  }
  if (pageStart > 0) {
    fui::ListItem item;
    item.label = tr(STR_BOOKORBIT_PREVIOUS_PAGE);
    item.actionValue = ROW_PREVIOUS;
    rows.push_back(item);
  }
  if (candidateCount - pageStart > CANDIDATES_PER_PAGE) {
    fui::ListItem item;
    item.label = tr(STR_BOOKORBIT_NEXT_PAGE);
    item.actionValue = ROW_NEXT;
    rows.push_back(item);
  }
  if (!error.empty()) {
    fui::ListItem item;
    item.label = error.c_str();
    item.actionValue = static_cast<int16_t>(rows.size());
    rows.push_back(item);
  }
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  addList(screen, props);
}

void BookOrbitSourcePickerActivity::activateIndex(int index) {
  if (index < 0 || static_cast<size_t>(index) >= rows.size()) return;
  const int action = rows[static_cast<size_t>(index)].actionValue;
  if (action == ROW_PREVIOUS || action == ROW_NEXT) {
    if (action == ROW_PREVIOUS) pageStart -= CANDIDATES_PER_PAGE;
    else pageStart += CANDIDATES_PER_PAGE;
    if (!loadCandidateWindow()) error = tr(STR_BOOKORBIT_STORAGE_ERROR);
    else error.clear();
    requestUpdate();
    return;
  }
  bool adding = true;
  size_t changedIndex = 0;
  BookOrbitCatalogSettings::Source removed{kind, 0, {}};
  if (action <= -100) {
    changedIndex = static_cast<size_t>(-100 - action);
    if (changedIndex >= settings.sources.size() || settings.sources[changedIndex].kind != kind) return;
    removed = std::move(settings.sources[changedIndex]);
    settings.sources.erase(settings.sources.begin() + static_cast<std::ptrdiff_t>(changedIndex));
    adding = false;
  } else {
    const size_t candidateIndex = static_cast<size_t>(action);
    if (candidateIndex >= candidateWindow.size()) return;
    if (settings.sources.size() >= BookOrbitCatalogSettings::MAX_SOURCES) {
      error = tr(STR_BOOKORBIT_SOURCE_LIMIT);
      requestUpdate();
      return;
    }
    settings.sources.push_back(candidateWindow[candidateIndex]);
  }
  if (!settings.save()) {
    if (adding) settings.sources.pop_back();
    else settings.sources.insert(settings.sources.begin() + static_cast<std::ptrdiff_t>(changedIndex), std::move(removed));
    error = tr(STR_BOOKORBIT_SAVE_ERROR);
  } else {
    error.clear();
  }
  requestUpdate();
}
