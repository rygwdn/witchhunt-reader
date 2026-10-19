#include "BookOrbitLibraryActivity.h"

#include <Bitmap.h>
#include <BookOrbitCatalogStore.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include "../../DiskLogger.h"
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "BookOrbitBookStateStore.h"
#include "BookOrbitBookStateSync.h"
#include "BookOrbitLocalLibrary.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/ActivityManager.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/home/BookDetails.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/themes/ListTouchBand.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/OpdsFilename.h"

namespace {
constexpr size_t MAX_LISTING_ROWS = 100;
constexpr size_t MAX_DASHBOARD_NAV_ITEMS = 8;
constexpr size_t MAX_DASHBOARD_SHELF_ITEMS = 6;
constexpr int MAX_BOOK_COVER_HEIGHT = 150;

constexpr char SESSION_CACHE_PATH[] = "/.tmp_bookorbit_cache.dat";
constexpr char SHELF_CACHE_PATH[] = "/.tmp_bookorbit_shelf.dat";
constexpr char LEGACY_DASHBOARD_CACHE_PATH[] = "/.crosspoint/bookorbit_dashboard.dat";
constexpr char LEGACY_DASHBOARD_STAGING_PATH[] = "/.crosspoint/bookorbit_dashboard.tmp";
constexpr char LEGACY_DASHBOARD_BACKUP_PATH[] = "/.crosspoint/bookorbit_dashboard.bak";
constexpr char COVER_JPG_PATH[] = "/.tmp_bookorbit_cover.jpg";
constexpr char COVER_STAGING_PATH[] = "/.crosspoint/bookorbit_cover.tmp";

// Reading statuses the catalog accepts from a device, in the order the picker
// lists them (the server's own progression, from untouched to given up).
struct StatusOption {
  BookOrbitCatalogClient::ReadStatus status;
  StrId label;
};
constexpr StatusOption STATUS_OPTIONS[] = {
    {BookOrbitCatalogClient::ReadStatus::Unread, StrId::STR_BO_STATUS_UNREAD},
    {BookOrbitCatalogClient::ReadStatus::WantToRead, StrId::STR_BO_STATUS_WANT_TO_READ},
    {BookOrbitCatalogClient::ReadStatus::Reading, StrId::STR_BO_STATUS_READING},
    {BookOrbitCatalogClient::ReadStatus::OnHold, StrId::STR_BO_STATUS_ON_HOLD},
    {BookOrbitCatalogClient::ReadStatus::Read, StrId::STR_BO_STATUS_READ},
    {BookOrbitCatalogClient::ReadStatus::Abandoned, StrId::STR_BO_STATUS_ABANDONED},
};
constexpr int STATUS_OPTION_COUNT = static_cast<int>(sizeof(STATUS_OPTIONS) / sizeof(STATUS_OPTIONS[0]));
// "No rating" plus one row per star count.
constexpr int RATING_OPTION_COUNT = 6;

const std::string* statusContentHash(const BookOrbitBookDetail& book) {
  const std::string* fallback = nullptr;
  for (const BookOrbitFile& file : book.files) {
    if (file.fileHash.size() != 32) continue;
    bool valid = true;
    for (const unsigned char digit : file.fileHash) {
      if (!std::isxdigit(digit)) {
        valid = false;
        break;
      }
    }
    if (!valid) continue;
    if (file.format == "epub") return &file.fileHash;
    if (!fallback) fallback = &file.fileHash;
  }
  return fallback;
}

void applyCachedStateToDetail(BookOrbitBookDetail& book, const bool includeCanonicalStatus) {
  const std::string* hash = statusContentHash(book);
  if (!hash) return;
  // This cold detail/status path reads one bounded 228-byte cache record, never per render frame.
  BookOrbitBookStateStore::State state;
  if (!BookOrbitBookStateStore::read(*hash, state)) return;
  if (includeCanonicalStatus && state.canonicalStatusSet) {
    book.readStatus = BookOrbitCatalogClient::readStatusKey(state.canonicalStatus);
  }
  if (state.pendingStatus) book.readStatus = BookOrbitCatalogClient::readStatusKey(state.localStatus);
  if (state.pendingRating) {
    book.ratingTenths = state.ratingCleared || state.rating == 0 ? -1 : static_cast<int16_t>(state.rating * 10);
  }
}

bool stateStatusSupported(const BookOrbitCatalogClient::ReadStatus status) {
  switch (status) {
    case BookOrbitCatalogClient::ReadStatus::Reading:
    case BookOrbitCatalogClient::ReadStatus::Read:
    case BookOrbitCatalogClient::ReadStatus::Abandoned:
      return true;
    case BookOrbitCatalogClient::ReadStatus::Unread:
    case BookOrbitCatalogClient::ReadStatus::WantToRead:
    case BookOrbitCatalogClient::ReadStatus::OnHold:
      return false;
  }
  return false;
}


std::string coverPathFor(const int32_t bookId) {
  return "/.crosspoint/bookorbit_cover_" + std::to_string(bookId) + ".bmp";
}
std::string coverBackupPathFor(const std::string& livePath) { return livePath + ".bak"; }

bool validCoverBmp(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("BOCat", path.c_str(), file)) return false;
  Bitmap bitmap(file);
  return bitmap.parseHeaders() == BmpReaderError::Ok && (bitmap.getBpp() == 1 || bitmap.getBpp() == 2) &&
         bitmap.isComplete();
}

bool recoverCoverCache(const std::string& livePath) {
  const std::string backupPath = coverBackupPathFor(livePath);
  const bool haveLive = Storage.exists(livePath.c_str());
  if (!Storage.exists(backupPath.c_str())) {
    return !haveLive || validCoverBmp(livePath) || Storage.remove(livePath.c_str());
  }

  if (haveLive && validCoverBmp(livePath)) {
    Storage.remove(backupPath.c_str());
    return true;
  }
  if (haveLive && !Storage.remove(livePath.c_str())) return false;
  if (validCoverBmp(backupPath)) return Storage.rename(backupPath.c_str(), livePath.c_str());
  return Storage.remove(backupPath.c_str());
}

bool publishCoverBmp(const std::string& livePath, const int targetWidth, const int targetHeight) {
  const std::string backupPath = coverBackupPathFor(livePath);
  Storage.remove(COVER_STAGING_PATH);
  bool converted = false;
  {
    HalFile src;
    HalFile dst;
    if (Storage.openFileForRead("BOCat", COVER_JPG_PATH, src) &&
        Storage.openFileForWrite("BOCat", COVER_STAGING_PATH, dst)) {
      converted = JpegToBmpConverter::jpegFileToBmpStreamWithSize(src, dst, targetWidth, targetHeight);
      dst.flush();
      dst.close();
    }
  }
  Storage.remove(COVER_JPG_PATH);
  if (!converted || !validCoverBmp(COVER_STAGING_PATH)) {
    Storage.remove(COVER_STAGING_PATH);
    return false;
  }
  if (!Storage.publishStagedFileWithBackup(COVER_STAGING_PATH, livePath.c_str(), backupPath.c_str())) {
    Storage.remove(COVER_STAGING_PATH);
    return false;
  }
  if (Storage.exists(backupPath.c_str())) Storage.remove(backupPath.c_str());
  return true;
}

bool readableNonemptyFile(const std::string& path) {
  HalFile file;
  return Storage.openFileForRead("BOCat", path.c_str(), file) && file.size() > 0;
}

std::string downloadFolderPrefix() {
  const char* folder = SETTINGS.opdsDownloadFolder;
  if (folder[0] == '\0') return "/";
  std::string prefix = folder;
  if (prefix.back() != '/') prefix += '/';
  return prefix;
}

std::string extensionFor(const std::string& format);

std::string localPathFor(const std::string& authors, const std::string& title, const std::string& format) {
  if (format != "epub" && format != "xtc" && format != "xtch" && format != "txt" && format != "md") return {};
  return downloadFolderPrefix() + opdsBookFilename(authors, title,
                                                   static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat),
                                                   extensionFor(format));
}

std::string downloadedPathFor(const BookOrbitEntry& entry) {
  size_t start = 0;
  while (start < entry.formats.size()) {
    const size_t separator = entry.formats.find(" \xC2\xB7 ", start);
    const std::string format =
        entry.formats.substr(start, separator == std::string::npos ? std::string::npos : separator - start);
    const std::string path = localPathFor(entry.subtitle, entry.title, format);
    const std::string downloaded = BookOrbitLocalLibrary::downloadedPath(path);
    if (!downloaded.empty()) return downloaded;
    if (separator == std::string::npos) break;
    start = separator + 4;
  }
  return {};
}

constexpr std::array<const char*, 5> LOCAL_DOWNLOAD_FORMATS = {"epub", "xtc", "xtch", "txt", "md"};

bool hasLocalFormat(const BookOrbitEntry& entry, const char* format) {
  size_t start = 0;
  while (start < entry.formats.size()) {
    const size_t separator = entry.formats.find(" \xC2\xB7 ", start);
    const size_t length = separator == std::string::npos ? std::string::npos : separator - start;
    if (entry.formats.compare(start, length, format) == 0) return true;
    if (separator == std::string::npos) break;
    start = separator + 4;
  }
  return false;
}

std::string downloadedPathFor(const BookOrbitBookDetail& detail) {
  for (const auto& file : detail.files) {
    const std::string path = localPathFor(detail.authors, detail.title, file.format);
    const std::string downloaded = BookOrbitLocalLibrary::downloadedPath(path, file.sizeBytes);
    if (!downloaded.empty()) return downloaded;
  }
  return {};
}
void writeString(HalFile& f, const std::string& s) {
  const uint16_t len = static_cast<uint16_t>(std::min<size_t>(s.length(), UINT16_MAX));
  f.write(reinterpret_cast<const uint8_t*>(&len), sizeof(len));
  if (len > 0) f.write(reinterpret_cast<const void*>(s.data()), len);
}

// Reads a length-prefixed string. Sets ok=false on any read failure; later calls
// with ok=false are no-ops, so the caller sees one failure for the whole row.
std::string readString(HalFile& f, bool& ok) {
  if (!ok) return {};
  uint16_t len = 0;
  if (f.read(&len, sizeof(len)) != sizeof(len)) {
    ok = false;
    return {};
  }
  if (len == 0) return {};
  std::string s(len, '\0');
  if (f.read(s.data(), len) != static_cast<int>(len)) {
    ok = false;
    return {};
  }
  return s;
}

// Row cache layout. Dashboard files add a magic header; each row is:
//   uint8_t type | uint8_t group | int32_t bookId | int32_t count
//   int16_t progressPercent | uint8_t hasCover
//   u16+str title | subtitle | href | series | formats
void writeEntry(HalFile& f, const BookOrbitEntry& entry) {
  const uint8_t type = static_cast<uint8_t>(entry.type);
  f.write(&type, sizeof(type));
  const uint8_t group = static_cast<uint8_t>(entry.group);
  f.write(&group, sizeof(group));
  f.write(reinterpret_cast<const uint8_t*>(&entry.bookId), sizeof(entry.bookId));
  f.write(reinterpret_cast<const uint8_t*>(&entry.count), sizeof(entry.count));
  f.write(reinterpret_cast<const uint8_t*>(&entry.progressPercent), sizeof(entry.progressPercent));
  const uint8_t hasCover = entry.hasCover ? 1 : 0;
  f.write(&hasCover, sizeof(hasCover));
  writeString(f, entry.title);
  writeString(f, entry.subtitle);
  writeString(f, entry.href);
  writeString(f, entry.series);
  writeString(f, entry.formats);
}

BookOrbitEntry readEntry(HalFile& f) {
  BookOrbitEntry entry;
  uint8_t type = 0;
  uint8_t group = 0;
  if (f.read(&type, sizeof(type)) != sizeof(type)) return {};
  if (f.read(&group, sizeof(group)) != sizeof(group)) return {};
  entry.type = static_cast<BookOrbitEntryType>(type);
  entry.group = static_cast<BookOrbitEntryGroup>(group);
  uint8_t hasCover = 0;
  if (f.read(&entry.bookId, sizeof(entry.bookId)) != sizeof(entry.bookId)) return {};
  if (f.read(&entry.count, sizeof(entry.count)) != sizeof(entry.count)) return {};
  if (f.read(&entry.progressPercent, sizeof(entry.progressPercent)) != sizeof(entry.progressPercent)) return {};
  if (f.read(&hasCover, sizeof(hasCover)) != sizeof(hasCover)) return {};
  entry.hasCover = hasCover != 0;
  bool ok = true;
  entry.title = readString(f, ok);
  entry.subtitle = readString(f, ok);
  entry.href = readString(f, ok);
  entry.series = readString(f, ok);
  entry.formats = readString(f, ok);
  if (!ok) return {};
  return entry;
}


std::string ratingLabel(const int index) {
  if (index <= 0) return tr(STR_BO_RATING_NONE);
  return std::to_string(index) + " " + tr(STR_BO_STARS);
}

const char* statusLabel(const BookOrbitCatalogClient::ReadStatus status) {
  for (const auto& option : STATUS_OPTIONS) {
    if (option.status == status) return I18N.get(option.label);
  }
  return I18N.get(StrId::STR_BO_STATUS_UNREAD);
}

// Extension for a catalog file, which names its format without a dot ("epub").
std::string extensionFor(const std::string& format) {
  std::string ext = ".";
  ext += format;
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
  return ext;
}
}  // namespace

ListDeclaration BookOrbitLibraryActivity::listDeclaration() {
  return {.left = {true, StrId::STR_SEARCH}, .right = {true, StrId::STR_REFRESH}};
}

BookOrbitEntry BookOrbitLibraryActivity::getEntry(const size_t index) const {
  if (index >= entryOffsets.size() || activeCachePath.empty()) return {};
  HalFile file;
  if (!Storage.openFileForRead("BOCat", activeCachePath.c_str(), file)) return {};
  file.seek(entryOffsets[index]);
  return readEntry(file);
}

BookOrbitEntry BookOrbitLibraryActivity::getDashboardEntry(const size_t position) const {
  if (position >= dashboardOrder.size()) return {};
  const size_t row = dashboardOrder[position];
  if (row >= shelfEntryOffsets.size() || shelfCachePath.empty()) return {};
  HalFile file;
  if (!Storage.openFileForRead("BOCat", shelfCachePath.c_str(), file)) return {};
  file.seek(shelfEntryOffsets[row]);
  return readEntry(file);
}
BookOrbitEntry BookOrbitLibraryActivity::getVisibleEntry(const size_t index) const {
  if (isDashboardView) {
    const Tab tab = tabs[static_cast<size_t>(selectedTab())];
    const size_t section = tab == Tab::Continue ? 0 : 1;
    if (index >= dashboardSectionCount[section]) return {};
    return getDashboardEntry(dashboardSectionStart[section] + index);
  }
  return getEntry(index);
}

int BookOrbitLibraryActivity::listCount() const {
  if (state != BrowserState::BROWSING) return 0;
  if (isDashboardView) {
    const Tab tab = tabs[static_cast<size_t>(selectedTab())];
    const size_t section = tab == Tab::Continue ? 0 : 1;
    return dashboardSectionCount[section];
  }
  return static_cast<int>(entryOffsets.size());
}

const char* BookOrbitLibraryActivity::tabLabel(const int slot) const {
  if (slot < 0 || static_cast<size_t>(slot) >= tabCountValue) return "";
  switch (tabs[static_cast<size_t>(slot)]) {
    case Tab::Continue: return tr(STR_BO_CONTINUE_READING);
    case Tab::Discover: return tr(STR_BO_DISCOVER);
    case Tab::New: return tr(STR_TAB_NEW);
    case Tab::Books: return tr(STR_TAB_BOOKS);
    case Tab::Browse: return tr(STR_BO_BROWSE);
  }
  return "";
}

const char* BookOrbitLibraryActivity::headerTitle() const { return tabLabel(selectedTab()); }

const char* BookOrbitLibraryActivity::footerConfirmLabel() const {
  if (const_cast<BookOrbitLibraryActivity*>(this)->tabsFocused()) return tr(STR_NEXT);
  return listCount() == 0 ? "" : tr(STR_OPEN);
}

void BookOrbitLibraryActivity::customizeTabBar(UiScreen&, freeink::ui::TabBarProps& props) {
  props.text.maxLines = 1;
}

void BookOrbitLibraryActivity::onTabSelected(const int slot) {
  if (slot < 0 || static_cast<size_t>(slot) >= tabCountValue) return;
  const Tab tab = tabs[static_cast<size_t>(slot)];
  state = BrowserState::BROWSING;
  const size_t tabSlot = static_cast<size_t>(slot);
  if (contexts[tabSlot].depth > 0) {
    isDashboardView = false;
    fetchListing(contexts[tabSlot].url, false);
    return;
  }
  if (tab == Tab::Continue || tab == Tab::Discover) {
    isDashboardView = true;
    currentUrl.clear();
    {
      RenderLock lock(*this);
      rowWindow.clear();
      rowWindowFirst = 0;
    }
    requestUpdate();
    ensureRowsFor(activeNav().selected, 0);
    return;
  }
  isDashboardView = false;
  std::string url = contexts[tabSlot].url;
  if (tab == Tab::New && url.empty()) url = newRoute;
  if (tab == Tab::Books && url.empty()) url = "@offline-search:1:";
  fetchListing(url, false);
  contexts[tabSlot].url = url;

}

void BookOrbitLibraryActivity::onBackFromTabs() {
  const size_t slot = static_cast<size_t>(selectedTab());
  if (slot < contexts.size() && contexts[slot].depth > 0) {
    navigateBack();
  } else {
    stopDashboardCoverWork(false);
    onGoHome();
  }
}
void BookOrbitLibraryActivity::backFromPosition(const int position) {
  const size_t slot = static_cast<size_t>(selectedTab());
  if (state == BrowserState::BROWSING && slot < contexts.size() && contexts[slot].depth > 0) {
    navigateBack();
    return;
  }
  TabbedUiListActivity::backFromPosition(position);
}

void BookOrbitLibraryActivity::buildScreen(UiScreen& screen) {
  layoutListArea(screen, 0, 0, /*spacer=*/false);
  buildTabBar(screen);
  screen.spacer(static_cast<int16_t>(UITheme::getInstance().getMetrics().verticalSpacing));
  const int count = listCount();
  if (count <= 0) {
    screen.centeredText(tr(STR_NO_ENTRIES), screen.theme().bodyText);
    return;
  }
  fui::ListProps props = listProps(screen);
  props.count = static_cast<uint16_t>(count);
  props.rowProvider = &BookOrbitLibraryActivity::provideRow;
  props.rowProviderCtx = this;
  props.labelText.maxLines = 2;
  addList(screen, props, /*hasSubtitle=*/true);
}

void BookOrbitLibraryActivity::provideRow(void* context, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<BookOrbitLibraryActivity*>(context);
  const int offset = static_cast<int>(index) - self->rowWindowFirst;
  if (offset < 0 || offset >= static_cast<int>(self->rowWindow.size())) {
    item.label = "…";
    return;
  }
  const BookOrbitEntry& entry = self->rowWindow[static_cast<size_t>(offset)];
  item.actionValue = static_cast<int16_t>(index);
  item.label = entry.title.c_str();
  if (!entry.subtitle.empty()) item.subtitle = entry.subtitle.c_str();
  if (self->isDashboardView) {
    const Tab tab = self->tabs[static_cast<size_t>(self->selectedTab())];
    const size_t section = tab == Tab::Continue ? 0 : 1;
    const size_t row = self->dashboardOrder[self->dashboardSectionStart[section] + index];
    if (row < self->shelfDownloadedFlags.size() && self->shelfDownloadedFlags[row]) {
      item.value = tr(STR_BO_DOWNLOADED);
      return;
    }
  } else if (index < self->downloadedFlags.size() && self->downloadedFlags[index]) {
    item.value = tr(STR_BO_DOWNLOADED);
    return;
  }
  if (entry.progressPercent > 0) {
    snprintf(self->rowValueBuffer, sizeof(self->rowValueBuffer), "%d%%", entry.progressPercent);
    item.value = self->rowValueBuffer;
  }
}

void BookOrbitLibraryActivity::ensureRowsFor(const int selected, const int windowTop) {
  const int count = listCount();
  if (count <= 0) {
    RenderLock lock(*this);
    rowWindow.clear();
    rowWindowFirst = 0;
    return;
  }
  const int drawn = publishedWindow().drawn > 1 ? publishedWindow().drawn : ROW_CACHE_OPENING_ROWS;
  const ListRowCache::Range held{rowWindowFirst, rowWindowFirst + static_cast<int>(rowWindow.size())};
  if (held.holds(ListRowCache::needed(count, selected, windowTop, drawn))) return;

  const ListRowCache::Range range = ListRowCache::toDecode(count, selected, windowTop, drawn);
  std::vector<BookOrbitEntry> rows;
  rows.reserve(static_cast<size_t>(range.last - range.first));
  for (int i = range.first; i < range.last; ++i) rows.push_back(getVisibleEntry(static_cast<size_t>(i)));
  {
    RenderLock lock(*this);
    rowWindow = std::move(rows);
    rowWindowFirst = range.first;
  }
  requestUpdate();
}

void BookOrbitLibraryActivity::onSelectionChanged(const int index) {
  ensureRowsFor(index, pageTop >= 0 ? pageTop : publishedWindow().top);
}

void BookOrbitLibraryActivity::showPositionPage(const int position, const int topPosition) {
  pageTop = topPosition > 0 ? topPosition - 1 : 0;
  TabbedUiListActivity::showPositionPage(position, topPosition);
  pageTop = -1;
  ensureRowsFor(activeNav().selected, topPosition > 0 ? topPosition - 1 : 0);
}

void BookOrbitLibraryActivity::activateIndex(const int index) {
  if (index < 0 || index >= listCount()) return;
  const BookOrbitEntry entry = getVisibleEntry(static_cast<size_t>(index));
  size_t row = static_cast<size_t>(index);
  if (isDashboardView) {
    const Tab tab = tabs[static_cast<size_t>(selectedTab())];
    const size_t section = tab == Tab::Continue ? 0 : 1;
    row = dashboardOrder[dashboardSectionStart[section] + static_cast<size_t>(index)];
  }
  const bool downloaded = isDashboardView ? row < shelfDownloadedFlags.size() && shelfDownloadedFlags[row]
                                          : row < downloadedFlags.size() && downloadedFlags[row];
  const std::string localPath = downloaded ? downloadedPathFor(entry) : std::string{};
  refreshPending = false;
  if (!localPath.empty()) {
    openDownloadedBook(localPath);
  } else if (entry.type == BookOrbitEntryType::BOOK) {
    detailReturnState = BrowserState::BROWSING;
    openBookDetail(entry);
  } else {
    navigateToEntry(entry);
  }
}



bool BookOrbitLibraryActivity::loadDashboard() {
  shelfEntryOffsets.clear();
  shelfDownloadedFlags.clear();
  newRoute.clear();
  isNewTabAvailable = false;
  dashboardOrder.clear();
  dashboardSectionStart.fill(0);
  dashboardSectionCount.fill(0);
  shelfCachePath = SHELF_CACHE_PATH;
  shelfEntryOffsets.reserve(MAX_DASHBOARD_NAV_ITEMS + MAX_DASHBOARD_SHELF_ITEMS * 2);
  shelfDownloadedFlags.reserve(MAX_DASHBOARD_NAV_ITEMS + MAX_DASHBOARD_SHELF_ITEMS * 2);
  dashboardOrder.reserve(MAX_DASHBOARD_NAV_ITEMS + MAX_DASHBOARD_SHELF_ITEMS * 2);
  std::array<std::array<uint16_t, MAX_DASHBOARD_NAV_ITEMS>, DASHBOARD_SECTION_COUNT> grouped{};
  std::array<uint16_t, DASHBOARD_SECTION_COUNT> groupedCount{};
  constexpr size_t MAX_DASHBOARD_ROWS = MAX_DASHBOARD_NAV_ITEMS + MAX_DASHBOARD_SHELF_ITEMS * 2;
  // One format batch at a time bounds temporary paths to 20 * 169 bytes off-stack.
  std::vector<std::string> candidatePaths;
  candidatePaths.reserve(MAX_DASHBOARD_ROWS);
  std::vector<uint8_t> candidateFlags;
  candidateFlags.reserve(MAX_DASHBOARD_ROWS);
  HalFile file;
  if (!Storage.openFileForWrite("BOCat", SHELF_CACHE_PATH, file)) {
    LOG_ERR("BOCat", "Local shelf stage failed: could not create session cache");
    shelfCachePath.clear();
    return false;
  }
  const bool loaded = BookOrbitCatalogStore::dashboard([&](const BookOrbitEntry& entry) {
    const size_t group = entry.group == BookOrbitEntryGroup::CONTINUE_READING
                             ? 0
                             : (entry.group == BookOrbitEntryGroup::DISCOVER ? 1 : 2);
    if (group == 2 && entry.type == BookOrbitEntryType::NAVIGATION && !isNewTabAvailable) {
      std::string route = entry.href;
      std::transform(route.begin(), route.end(), route.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      bool recentlyAdded = false;
      const size_t queryStart = route.find('?');
      if (queryStart != std::string::npos) {
        size_t parameterStart = queryStart + 1;
        while (parameterStart < route.size()) {
          const size_t parameterEnd = route.find('&', parameterStart);
          const size_t end = parameterEnd == std::string::npos ? route.size() : parameterEnd;
          const size_t equals = route.find('=', parameterStart);
          if (equals < end && route.compare(parameterStart, equals - parameterStart, "sort") == 0 &&
              route.compare(equals + 1, end - equals - 1, "recently_added") == 0) {
            recentlyAdded = true;
            break;
          }
          if (parameterEnd == std::string::npos) break;
          parameterStart = parameterEnd + 1;
        }
      }
      const size_t pathEnd = route.find('?');
      const size_t end = pathEnd == std::string::npos ? route.size() : pathEnd;
      size_t segmentStart = route.find('/', 0);
      bool explicitRecentRoute = false;
      while (segmentStart != std::string::npos && segmentStart < end) {
        const size_t segmentEnd = route.find('/', segmentStart + 1);
        const size_t stop = segmentEnd == std::string::npos || segmentEnd > end ? end : segmentEnd;
        const size_t length = stop - segmentStart - 1;
        if ((length == 6 && route.compare(segmentStart + 1, length, "recent") == 0) ||
            (length == 3 && route.compare(segmentStart + 1, length, "new") == 0) ||
            (length == 14 && route.compare(segmentStart + 1, length, "recently-added") == 0)) {
          explicitRecentRoute = true;
          break;
        }
        segmentStart = segmentEnd;
      }
      if (recentlyAdded || explicitRecentRoute) {
        newRoute = entry.href;
        isNewTabAvailable = true;
      }
    }
    const size_t cap = group == 2 ? MAX_DASHBOARD_NAV_ITEMS : MAX_DASHBOARD_SHELF_ITEMS;
    if (groupedCount[group] >= cap) return;
    const uint16_t row = static_cast<uint16_t>(shelfEntryOffsets.size());
    shelfEntryOffsets.push_back(file.position());
    shelfDownloadedFlags.push_back(0);
    grouped[group][groupedCount[group]++] = row;
    writeEntry(file, entry);
  });
  file.flush();
  file.close();
  if (!loaded) {
    LOG_ERR("BOCat", "Local shelf stage failed: catalog read");
    shelfCachePath.clear();
    shelfEntryOffsets.clear();
    shelfDownloadedFlags.clear();
    return false;
  }
  candidatePaths.resize(shelfEntryOffsets.size());
  HalFile candidateFile;
  if (!Storage.openFileForRead("BOCat", SHELF_CACHE_PATH, candidateFile)) {
    LOG_ERR("BOCat", "Local shelf stage failed: could not reopen cache for offline flags");
  } else {
    for (const char* format : LOCAL_DOWNLOAD_FORMATS) {
      for (size_t row = 0; row < shelfEntryOffsets.size(); ++row) {
        candidatePaths[row].clear();
        candidateFile.seek(shelfEntryOffsets[row]);
        const BookOrbitEntry entry = readEntry(candidateFile);
        if (hasLocalFormat(entry, format)) candidatePaths[row] = localPathFor(entry.subtitle, entry.title, format);
      }
      BookOrbitLocalLibrary::downloadedFlags(candidatePaths, candidateFlags);
      for (size_t row = 0; row < shelfDownloadedFlags.size(); ++row) {
        if (candidateFlags[row]) shelfDownloadedFlags[row] = 1;
      }
    }
  }
  for (size_t group = 0; group < DASHBOARD_SECTION_COUNT; ++group) {
    dashboardSectionStart[group] = static_cast<uint16_t>(dashboardOrder.size());
    dashboardSectionCount[group] = groupedCount[group];
    for (uint16_t i = 0; i < groupedCount[group]; ++i) dashboardOrder.push_back(grouped[group][i]);
  }
  if (isNewTabAvailable) {
    tabs = {Tab::Continue, Tab::Discover, Tab::New, Tab::Books, Tab::Browse};
    tabCountValue = 5;
  } else {
    tabs = {Tab::Continue, Tab::Discover, Tab::Books, Tab::Browse, Tab::Browse};
    tabCountValue = 4;
  }
  rowWindow.clear();
  rowWindowFirst = 0;
  return true;
}

void BookOrbitLibraryActivity::onEnter() {
  tabCountValue = TAB_CAPACITY;
  tabs = {Tab::Continue, Tab::Discover, Tab::New, Tab::Books, Tab::Browse};
  TabbedUiListActivity::onEnter();
  entryOffsets.clear();
  downloadedFlags.clear();
  shelfEntryOffsets.clear();
  shelfDownloadedFlags.clear();
  dashboardOrder.clear();
  rowWindow.clear();
  contexts = {};
  dashboardCoverCount = 0;
  dashboardCoverPosition = 0;
  dashboardCoverHeaders.clear();
  dashboardCoverSession.reset();
  refreshInProgress = false;
  dashboardCoversPending = false;
  dashboardCoverBackRequested = false;
  currentUrl.clear();
  activeCachePath.clear();
  shelfCachePath.clear();
  consumeConfirm = false;
  memoryTrimmed = false;
  coverAvailable = false;
  refreshPending = false;
  restartToDownloadedBook = false;
  offlineDetail = false;
  downloadAfterDetail = false;
  detailConnectPending = false;
  errorMessage.clear();
  statusMessage.clear();
  state = BrowserState::LOADING;
  initialLoadPending = true;
  BookOrbitCatalogClient::setSkipTlsValidation(SETTINGS.skipHttpsValidation != 0);
  refreshStartMs = 0;
  refreshProgressAtMs = 0;
  requestUpdate();
}

void BookOrbitLibraryActivity::onExit() {
  if (refreshInProgress) {
    BookOrbitCatalogStore::cancelRebuild();
    refreshInProgress = false;
  }
  stopDashboardCoverWork(false);
  TabbedUiListActivity::onExit();
  BookOrbitCatalogClient::endPersistentSession();
  entryOffsets.clear();
  downloadedFlags.clear();
  shelfEntryOffsets.clear();
  shelfDownloadedFlags.clear();
  dashboardOrder.clear();
  rowWindow.clear();
  contexts = {};
  detail = {};
  Storage.remove(SESSION_CACHE_PATH);
  Storage.remove(SHELF_CACHE_PATH);
  Storage.remove(COVER_JPG_PATH);
  Storage.remove(COVER_STAGING_PATH);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    if (restartToDownloadedBook) {
      silentRestartToReader();
    } else {
      silentRestart();
    }
  }
}


void BookOrbitLibraryActivity::loop() {
  if (initialLoadPending) {
    initialLoadPending = false;
    Storage.remove(LEGACY_DASHBOARD_CACHE_PATH);
    Storage.remove(LEGACY_DASHBOARD_STAGING_PATH);
    Storage.remove(LEGACY_DASHBOARD_BACKUP_PATH);
    const bool hasLocalCatalog = BookOrbitCatalogStore::available();
    LOG_INF("BOCat", "Library entry mode=%s", hasLocalCatalog ? "local-snapshot" : "network-rebuild");
    if (hasLocalCatalog && fetchDashboard()) {
      selectTab(0);
      return;
    }
    refreshPending = true;
    state = BrowserState::CHECK_WIFI;
    statusMessage = tr(STR_CHECKING_WIFI);
    requestUpdate();
    checkAndConnectWifi();
    return;
  }
  if (state == BrowserState::WIFI_SELECTION || state == BrowserState::SEARCH_INPUT) return;
  if (consumeConfirm && mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    consumeConfirm = false;
    return;
  }
  if (refreshInProgress) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) cancelCatalogRefresh();
    else advanceCatalogRefresh();
    return;
  }
  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (refreshPending || detailConnectPending) checkAndConnectWifi();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      pendingDetailEntry = {};
      detailConnectPending = false;
      downloadAfterDetail = false;
      refreshPending = false;
      restoreCachedCatalogView();
    }
    return;
  }
  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back))
      state == BrowserState::CHECK_WIFI ? onGoHome() : navigateBack();
    return;
  }
  if (state == BrowserState::DOWNLOADING) return;
  if (state == BrowserState::BOOK_DETAIL) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = detailReturnState;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!downloadedBookPath.empty()) openDownloadedBook(downloadedBookPath);
      else if (offlineDetail) {
        downloadAfterDetail = true;
        const BookOrbitEntry entry = pendingDetailEntry;
        openBookDetail(entry);
      } else chooseBookFormat();
    } else if (!offlineDetail && mappedInput.wasLogicalReleased(MappedInputManager::Direction::Left)) {
      statusSelectorIndex = 0;
      for (int i = 0; i < STATUS_OPTION_COUNT; ++i)
        if (STATUS_OPTIONS[i].status == BookOrbitCatalogClient::readStatusFromKey(detail.readStatus)) {
          statusSelectorIndex = i;
          break;
        }
      state = BrowserState::STATUS_SELECTION;
      requestUpdate();
    } else if (!offlineDetail && mappedInput.wasLogicalReleased(MappedInputManager::Direction::Right)) {
      ratingSelectorIndex = detail.ratingTenths > 0 ? std::min(5, (detail.ratingTenths + 5) / 10) : 0;
      state = BrowserState::RATING_SELECTION;
      requestUpdate();
    }
    return;
  }
  if (state == BrowserState::FORMAT_SELECTION || state == BrowserState::STATUS_SELECTION ||
      state == BrowserState::RATING_SELECTION) {
    int& selected = state == BrowserState::FORMAT_SELECTION
                        ? formatSelectorIndex
                        : (state == BrowserState::STATUS_SELECTION ? statusSelectorIndex : ratingSelectorIndex);
    const int count = state == BrowserState::FORMAT_SELECTION
                          ? static_cast<int>(detail.files.size())
                          : (state == BrowserState::STATUS_SELECTION ? STATUS_OPTION_COUNT : RATING_OPTION_COUNT);
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = BrowserState::BOOK_DETAIL;
      requestUpdate();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (state == BrowserState::FORMAT_SELECTION && count > 0) downloadBook(detail.files[selected]);
      else if (state == BrowserState::STATUS_SELECTION) applyReadStatus(STATUS_OPTIONS[selected].status);
      else if (state == BrowserState::RATING_SELECTION) applyRating(static_cast<uint8_t>(selected));
    } else {
      buttonNavigator.onNextList(ButtonNavigator::getStepNextButtons(), selected, count,
                                 [this] { requestUpdate(); });
      buttonNavigator.onPreviousList(ButtonNavigator::getStepPreviousButtons(), selected, count,
                                     [this] { requestUpdate(); });
    }
    return;
  }
  TabbedUiListActivity::loop();
  processDashboardCover();
}

void BookOrbitLibraryActivity::onListAction(const ListGrammar::Side side, const int /*position*/) {
  if (state != BrowserState::BROWSING) return;
  switch (side) {
    case ListGrammar::Side::Left:
      launchSearch();
      return;
    case ListGrammar::Side::Right:
      stopDashboardCoverWork(true);
      refreshPending = true;
      statusMessage = tr(STR_CHECKING_WIFI);
      state = BrowserState::CHECK_WIFI;
      requestUpdate();
      checkAndConnectWifi();
      return;
  }
}

void BookOrbitLibraryActivity::openDownloadedBook(const std::string& path) {
  if (!readableNonemptyFile(path)) {
    downloadedBookPath.clear();
    requestUpdate();
    return;
  }
  stopDashboardCoverWork(false);
  APP_STATE.openEpubPath = path;
  APP_STATE.saveToFile();
  restartToDownloadedBook = WiFi.getMode() != WIFI_MODE_NULL;
  activityManager.goToReader(path);
}

void BookOrbitLibraryActivity::render(RenderLock&& lock) {
  if (state == BrowserState::BROWSING) {
    UiListActivity::render(std::move(lock));
    return;
  }
  renderer.clearScreen();

  const Rect contentRect = UITheme::getContentRect(renderer, true, false);
  const int midY = contentRect.y + contentRect.height / 2;
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{contentRect.x, metrics.topPadding, contentRect.width, metrics.headerHeight},
                 tr(STR_BOOKORBIT_LIBRARY));

  if (state == BrowserState::CHECK_WIFI || state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 20, tr(STR_ERROR_MSG));
    const auto lines = renderer.wrappedText(UI_10_FONT_ID, errorMessage.c_str(), contentRect.width - 40, 3);
    int y = midY + 10;
    for (const auto& line : lines) {
      renderer.drawCenteredText(UI_10_FONT_ID, y, line.c_str());
      y += renderer.getLineHeight(UI_10_FONT_ID);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 40, tr(STR_DOWNLOADING));
    const auto title = renderer.truncatedText(UI_10_FONT_ID, statusMessage.c_str(), contentRect.width - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, midY - 10, title.c_str());
    if (downloadTotal > 0) {
      GUI.drawProgressBar(renderer, Rect{contentRect.x + 50, midY + 20, contentRect.width - 100, 20}, downloadProgress,
                          downloadTotal);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::FORMAT_SELECTION || state == BrowserState::STATUS_SELECTION ||
      state == BrowserState::RATING_SELECTION) {
    const bool isFormat = state == BrowserState::FORMAT_SELECTION;
    const bool isStatus = state == BrowserState::STATUS_SELECTION;
    const int count =
        isFormat ? static_cast<int>(detail.files.size()) : (isStatus ? STATUS_OPTION_COUNT : RATING_OPTION_COUNT);
    const int selected = isFormat ? formatSelectorIndex : (isStatus ? statusSelectorIndex : ratingSelectorIndex);
    const char* header = isFormat ? tr(STR_DOWNLOAD) : (isStatus ? tr(STR_BO_SET_STATUS) : tr(STR_BO_SET_RATING));

    const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int bodyTop = std::max(contentRect.y, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing);
    const int contentBottom = contentRect.y + contentRect.height;
    const int listTop = bodyTop + 2 * lineHeight + metrics.verticalSpacing;
    const int rowHeight = std::max(metrics.listRowHeight, lineHeight);
    const int visibleRows = std::max(1, (contentBottom - listTop) / rowHeight);
    const int pageStart = selected / visibleRows * visibleRows;
    const int pageEnd = std::min(count, pageStart + visibleRows);
    renderer.drawCenteredText(UI_10_FONT_ID, bodyTop, header, true, EpdFontFamily::BOLD);
    const auto title = renderer.truncatedText(UI_10_FONT_ID, detail.title.c_str(), contentRect.width - 40);
    renderer.drawCenteredText(UI_10_FONT_ID, bodyTop + lineHeight, title.c_str());

    ListTouchBand::recordUniformRows(contentRect.x, contentRect.width - 1, listTop - 2, rowHeight, pageStart,
                                     pageEnd - pageStart);
    renderer.fillRect(contentRect.x, listTop + (selected - pageStart) * rowHeight - 2, contentRect.width - 1,
                      rowHeight);
    for (int i = pageStart; i < pageEnd; i++) {
      std::string label;
      if (isFormat) {
        label = detail.files[i].format;
        std::transform(label.begin(), label.end(), label.begin(),
                       [](unsigned char c) { return static_cast<char>(toupper(c)); });
        if (detail.files[i].sizeBytes > 0) {
          label += " (" + std::to_string((detail.files[i].sizeBytes + 512 * 1024) / (1024 * 1024)) + " MB)";
        }
      } else if (isStatus) {
        label = I18N.get(STATUS_OPTIONS[i].label);
      } else {
        label = ratingLabel(i);
      }
      const auto row = renderer.truncatedText(UI_10_FONT_ID, label.c_str(), contentRect.width - 40);
      renderer.drawText(UI_10_FONT_ID, contentRect.x + 20, listTop + (i - pageStart) * rowHeight, row.c_str(),
                        i != selected);
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), isFormat ? tr(STR_DOWNLOAD) : tr(STR_SELECT),
                                              tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::BOOK_DETAIL) {
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int contentTop = std::max(contentRect.y, metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing);
    const int footerY = contentRect.y + contentRect.height - lineH - metrics.verticalSpacing;
    const int padding = metrics.contentSidePadding;
    const bool wide = contentRect.width >= 600;
    int textX = contentRect.x + padding;
    int textW = std::max(1, contentRect.width - padding * 2);
    int y = contentTop;
    int coverX = 0;
    int coverY = contentTop;
    int coverW = 0;
    int coverH = 0;
    bool hasGrayCover = false;
    if (coverAvailable && !coverBmpPath.empty()) {
      HalFile bmpFile;
      if (Storage.openFileForRead("BOCat", coverBmpPath.c_str(), bmpFile)) {
        Bitmap bmp(bmpFile);
        if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getWidth() > 0 && bmp.getHeight() > 0) {
          const int availableH = std::max(1, footerY - contentTop - metrics.verticalSpacing);
          coverH = wide ? availableH : std::min(availableH / 3, MAX_BOOK_COVER_HEIGHT);
          coverW = bmp.getWidth() * coverH / bmp.getHeight();
          const int maxCoverW = wide ? contentRect.width / 3 : contentRect.width / 2;
          if (coverW > maxCoverW) {
            coverW = maxCoverW;
            coverH = bmp.getHeight() * coverW / bmp.getWidth();
          }
          coverX = wide ? contentRect.x + padding : contentRect.x + (contentRect.width - coverW) / 2;
          renderer.drawBitmap(bmp, coverX, coverY, coverW, coverH);
          hasGrayCover = true;
          if (wide) {
            textX = coverX + coverW + padding;
            textW = std::max(1, contentRect.x + contentRect.width - padding - textX);
          } else {
            y = contentTop + coverH + metrics.verticalSpacing;
          }
        }
      }
    }

    if (!detail.title.empty()) {
      const auto titleLines = renderer.wrappedText(UI_12_FONT_ID, detail.title.c_str(), textW, wide ? 3 : 2);
      for (const auto& line : titleLines) {
        if (y + renderer.getLineHeight(UI_12_FONT_ID) > footerY) break;
        renderer.drawText(UI_12_FONT_ID, textX, y, line.c_str(), true, EpdFontFamily::BOLD);
        y += renderer.getLineHeight(UI_12_FONT_ID);
      }
      y += 2;
    }
    const auto drawMeta = [&](const std::string& value) {
      if (value.empty() || y + lineH > footerY) return;
      const auto line = renderer.truncatedText(UI_10_FONT_ID, value.c_str(), textW);
      renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str());
      y += lineH + 2;
    };
    drawMeta(detail.authors);
    drawMeta(detail.series);

    if (!offlineDetail) {
      std::string stateLine = statusLabel(BookOrbitCatalogClient::readStatusFromKey(detail.readStatus));
      if (detail.progressPercent > 0) stateLine += " · " + std::to_string(detail.progressPercent) + "%";
      stateLine += " · ";
      stateLine += detail.ratingTenths > 0 ? ratingLabel(std::min(5, (detail.ratingTenths + 5) / 10))
                                           : std::string(tr(STR_BO_RATING_NONE));
      drawMeta(stateLine);
    }

    if (y + metrics.verticalSpacing < footerY) {
      renderer.drawLine(textX, y, textX + textW, y);
      y += metrics.verticalSpacing;
    }
    if (!detail.description.empty()) {
      const int maxLines = std::max(0, (footerY - y) / lineH);
      if (maxLines > 0) {
        const auto lines = renderer.wrappedText(UI_10_FONT_ID, detail.description.c_str(), textW, maxLines);
        for (const auto& line : lines) {
          renderer.drawText(UI_10_FONT_ID, textX, y, line.c_str());
          y += lineH;
        }
      }
    }

    std::string footer = statusMessage;
    if (footer.empty() && !downloadedBookPath.empty()) footer = tr(STR_BO_DOWNLOADED);
    if (footer.empty()) {
      for (const auto& file : detail.files) {
        if (!footer.empty()) footer += " · ";
        footer += file.format;
      }
    }
    if (!footer.empty()) {
      const auto footerText = renderer.truncatedText(UI_10_FONT_ID, footer.c_str(), contentRect.width - padding * 2);
      renderer.drawCenteredText(UI_10_FONT_ID, footerY, footerText.c_str());
    }

    const auto labels =
        mappedInput.mapLabels(tr(STR_BACK), downloadedBookPath.empty() ? tr(STR_DOWNLOAD) : tr(STR_OPEN),
                              offlineDetail ? "" : tr(STR_BO_SET_STATUS), offlineDetail ? "" : tr(STR_BO_SET_RATING));
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    if (hasGrayCover) {
      const auto renderCoverPlane = [&](const GfxRenderer::RenderMode) {
        HalFile bmpFile;
        if (!Storage.openFileForRead("BOCat", coverBmpPath.c_str(), bmpFile)) return;
        Bitmap bmp(bmpFile);
        if (bmp.parseHeaders() != BmpReaderError::Ok || bmp.getWidth() <= 0 || bmp.getHeight() <= 0) return;
        renderer.drawBitmap(bmp, coverX, coverY, coverW, coverH);
      };
      renderer.renderGrayscalePlanesSequential(renderCoverPlane, [] { return false; });
    }
    return;
  }

  renderer.displayBuffer();
}

bool BookOrbitLibraryActivity::fetchDashboard() {
  const uint32_t startMs = millis();
  const bool loaded = loadDashboard();
  LOG_INF("BOCat", "Local BookOrbit views load %s entries=%u ms=%lu", loaded ? "succeeded" : "failed",
          static_cast<unsigned>(shelfEntryOffsets.size()),
          static_cast<unsigned long>(static_cast<uint32_t>(millis()) - startMs));
  if (!loaded) {
    DiskLogger::flushNow();
    refreshPending = true;
    state = BrowserState::ERROR;
    errorMessage = tr(STR_ERROR_GENERAL_FAILURE);
    requestUpdate();
    return false;
  }
  refreshPending = false;
  statusMessage.clear();
  state = BrowserState::BROWSING;
  isDashboardView = true;
  currentUrl.clear();
  requestUpdate();
  return true;
}

void BookOrbitLibraryActivity::refreshCatalog() {
  stopDashboardCoverWork(false);
  BookOrbitCatalogClient::beginPersistentSession();
  refreshStartMs = millis();
  refreshProgressAtMs = refreshStartMs;
  state = BrowserState::LOADING;
  statusMessage.reserve(64);
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();
  if (!BookOrbitCatalogStore::beginRebuild()) {
    showError(BookOrbitCatalogStore::rebuildError());
    return;
  }
  refreshInProgress = true;
}

void BookOrbitLibraryActivity::advanceCatalogRefresh() {
  BookOrbitCatalogStore::RebuildProgress progress{};
  const auto status = BookOrbitCatalogStore::rebuildStep(&progress);
  switch (status) {
    case BookOrbitCatalogStore::RebuildStatus::InProgress: {
      const uint32_t now = millis();
      if (now - refreshProgressAtMs < 1000) return;
      refreshProgressAtMs = now;
      char progressMessage[64];
      snprintf(progressMessage, sizeof(progressMessage), "%s %lu/%lu", tr(STR_LOADING),
               static_cast<unsigned long>(progress.completedPages),
               static_cast<unsigned long>(progress.discoveredPages));
      statusMessage = progressMessage;
      requestUpdate();
      return;
    }
    case BookOrbitCatalogStore::RebuildStatus::Complete: {
      refreshInProgress = false;
      const int previousTab = selectedTab();
      contexts = {};
      if (fetchDashboard()) {
        selectTab(std::min(previousTab, tabCount() - 1));
        if (state == BrowserState::BROWSING) startDashboardCoverWork();
      }
      LOG_INF("BOCat", "Library refresh complete ms=%lu views=%s",
              static_cast<unsigned long>(static_cast<uint32_t>(millis()) - refreshStartMs),
              state == BrowserState::BROWSING ? "ready" : "failed");
      return;
    }
    case BookOrbitCatalogStore::RebuildStatus::Failed: {
      refreshInProgress = false;
      const auto error = BookOrbitCatalogStore::rebuildError();
      LOG_ERR("BOCat", "Library refresh failed ms=%lu error=%d HTTP=%d",
              static_cast<unsigned long>(static_cast<uint32_t>(millis()) - refreshStartMs),
              static_cast<int>(error), BookOrbitCatalogClient::lastHttpCode);
      showError(error);
      return;
    }
    case BookOrbitCatalogStore::RebuildStatus::Cancelled:
      refreshInProgress = false;
      refreshPending = false;
      restoreCachedCatalogView();
      return;
  }
}

void BookOrbitLibraryActivity::cancelCatalogRefresh() {
  if (!refreshInProgress) return;
  BookOrbitCatalogStore::cancelRebuild();
  refreshInProgress = false;
  refreshPending = false;
  restoreCachedCatalogView();
}

void BookOrbitLibraryActivity::restoreCachedCatalogView() {
  if (!BookOrbitCatalogStore::available()) {
    onGoHome();
    return;
  }
  state = BrowserState::BROWSING;
  selectTab(std::min(selectedTab(), tabCount() - 1));
}

void BookOrbitLibraryActivity::startDashboardCoverWork() {
  dashboardCoverStartMs = millis();
  dashboardCoverAttempted = 0;
  dashboardCoverPublished = 0;
  fetchDashboardCovers();
}

void BookOrbitLibraryActivity::fetchDashboardCovers() {
  dashboardCoverCount = 0;
  dashboardCoverPosition = 0;
  dashboardCoverHeaders.clear();
  dashboardCoversPending = false;
  for (size_t position = 0; position < dashboardOrder.size() &&
                             dashboardCoverCount < dashboardCoverCandidates.size();
       ++position) {
    const BookOrbitEntry entry = getDashboardEntry(position);
    if (entry.type != BookOrbitEntryType::BOOK || !entry.hasCover || entry.bookId < 0) continue;
    bool alreadySeen = false;
    for (size_t i = 0; i < dashboardCoverCount; ++i) {
      if (dashboardCoverCandidates[i].bookId == entry.bookId) {
        alreadySeen = true;
        break;
      }
    }
    if (alreadySeen) continue;

    auto& candidate = dashboardCoverCandidates[dashboardCoverCount++];
    candidate.bookId = entry.bookId;
    candidate.needsDownload = false;
    const std::string bmpPath = coverPathFor(entry.bookId);
    if (!recoverCoverCache(bmpPath)) {
      LOG_DBG("BOCat", "Could not recover dashboard cover cache for book %ld", static_cast<long>(entry.bookId));
      continue;
    }
    candidate.needsDownload = !validCoverBmp(bmpPath);
  }

  bool hasMissingCover = false;
  for (size_t i = 0; i < dashboardCoverCount; ++i) {
    if (dashboardCoverCandidates[i].needsDownload) {
      hasMissingCover = true;
      break;
    }
  }
  if (!hasMissingCover) return;

  dashboardCoverHeaders = BookOrbitCatalogClient::authHeaders();
  dashboardCoversPending = true;
  BookOrbitCatalogClient::endPersistentSession();
}

void BookOrbitLibraryActivity::stopDashboardCoverWork(const bool resumeCatalogSession) {
  if (!dashboardCoversPending) return;
  dashboardCoversPending = false;
  dashboardCoverSession.reset();
  dashboardCoverHeaders.clear();
  dashboardCoverCount = 0;
  dashboardCoverPosition = 0;
  dashboardCoverBackRequested = false;
  dashboardCoverCandidates.fill(DashboardCoverCandidate{});
  if (resumeCatalogSession) BookOrbitCatalogClient::beginPersistentSession();
}

void BookOrbitLibraryActivity::processDashboardCover() {
  if (!dashboardCoversPending || state != BrowserState::BROWSING) return;
  while (dashboardCoverPosition < dashboardCoverCount &&
         !dashboardCoverCandidates[dashboardCoverPosition].needsDownload) {
    ++dashboardCoverPosition;
  }
  if (dashboardCoverPosition >= dashboardCoverCount) {
    LOG_INF("BOCat", "Dashboard cover backfill total_ms=%lu attempted=%u published=%u failed=%u",
            static_cast<unsigned long>(static_cast<uint32_t>(millis()) - dashboardCoverStartMs),
            dashboardCoverAttempted, dashboardCoverPublished,
            dashboardCoverAttempted - dashboardCoverPublished);
    stopDashboardCoverWork(true);
    return;
  }

  const int32_t bookId = dashboardCoverCandidates[dashboardCoverPosition++].bookId;
  ++dashboardCoverAttempted;
  if (!dashboardCoverSession) dashboardCoverSession.emplace();
  if (!dashboardCoverSession->impl()) {
    stopDashboardCoverWork(true);
    return;
  }

  dashboardCoverBackRequested = false;
  const auto result = HttpDownloader::downloadToFile(
      *dashboardCoverSession, BookOrbitCatalogClient::thumbnailUrl(bookId), COVER_JPG_PATH, dashboardCoverHeaders,
      [this](const unsigned int, const unsigned int) {
        mappedInput.update();
        if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          dashboardCoverBackRequested = true;
          return false;
        }
        return true;
      });
  if (result != HttpDownloader::OK) {
    LOG_DBG("BOCat", "Dashboard cover download failed for book %ld", static_cast<long>(bookId));
  } else if (!publishCoverBmp(coverPathFor(bookId), MAX_BOOK_COVER_HEIGHT * 2 / 3, MAX_BOOK_COVER_HEIGHT)) {
    LOG_DBG("BOCat", "Dashboard cover conversion/publication failed for book %ld", static_cast<long>(bookId));
  } else {
    ++dashboardCoverPublished;
  }

  if (dashboardCoverBackRequested) {
    stopDashboardCoverWork(true);
    return;
  }
  if (dashboardCoverPosition >= dashboardCoverCount) {
    LOG_INF("BOCat", "Dashboard cover backfill total_ms=%lu attempted=%u published=%u failed=%u",
            static_cast<unsigned long>(static_cast<uint32_t>(millis()) - dashboardCoverStartMs),
            dashboardCoverAttempted, dashboardCoverPublished,
            dashboardCoverAttempted - dashboardCoverPublished);
    stopDashboardCoverWork(true);
  }
}

void BookOrbitLibraryActivity::openCatalogRoot() {
  for (int slot = 0; slot < tabCount(); ++slot) {
    if (tabs[static_cast<size_t>(slot)] == Tab::Browse) {
      contexts[static_cast<size_t>(slot)] = {};
      selectTab(slot);
      return;
    }
  }
}

void BookOrbitLibraryActivity::fetchListing(const std::string& url, const bool resetSelection) {
  stopDashboardCoverWork(true);
  isDashboardView = false;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();
  currentUrl = url;
  activeCachePath = SESSION_CACHE_PATH;
  {
    RenderLock lock(*this);
    rowWindow.clear();
    rowWindowFirst = 0;
  }
  entryOffsets.clear();
  downloadedFlags.clear();
  entryOffsets.reserve(MAX_LISTING_ROWS + 2);
  downloadedFlags.reserve(MAX_LISTING_ROWS + 2);
  pageInfo = {};
  HalFile file;
  if (!Storage.openFileForWrite("BOCat", SESSION_CACHE_PATH, file)) {
    LOG_ERR("BOCat", "Local listing stage failed: could not create session cache");
    DiskLogger::flushNow();
    state = BrowserState::ERROR;
    errorMessage = tr(STR_ERROR_GENERAL_FAILURE);
    requestUpdate();
    return;
  }
  bool capped = false;
  size_t candidateRowCount = 0;
  const auto onEntry = [&](const BookOrbitEntry& entry) {
    if (entryOffsets.size() >= MAX_LISTING_ROWS) {
      capped = true;
      return;
    }
    ++candidateRowCount;
    entryOffsets.push_back(file.position());
    downloadedFlags.push_back(0);
    writeEntry(file, entry);
  };
  const bool loaded = BookOrbitCatalogStore::listing(url, onEntry, pageInfo);
  if (loaded && !capped && !pageInfo.prevUrl.empty()) {
    BookOrbitEntry prev;
    prev.type = BookOrbitEntryType::NAVIGATION;
    prev.title = tr(STR_PREV_PAGE);
    prev.href = pageInfo.prevUrl;
    entryOffsets.insert(entryOffsets.begin(), file.position());
    downloadedFlags.insert(downloadedFlags.begin(), 0);
    writeEntry(file, prev);
  }
  if (loaded && !capped && !pageInfo.nextUrl.empty()) {
    BookOrbitEntry next;
    next.type = BookOrbitEntryType::NAVIGATION;
    next.title = tr(STR_NEXT_PAGE);
    next.href = pageInfo.nextUrl;
    entryOffsets.push_back(file.position());
    downloadedFlags.push_back(0);
    writeEntry(file, next);
  }
  file.flush();
  file.close();
  if (!loaded || capped) {
    LOG_ERR("BOCat", "Local listing failed: loaded=%u capped=%u rows=%u HTTP=%d",
            loaded ? 1U : 0U, capped ? 1U : 0U, static_cast<unsigned>(entryOffsets.size()),
            BookOrbitCatalogClient::lastHttpCode);
    DiskLogger::flushNow();
    refreshPending = true;
    state = BrowserState::ERROR;
    errorMessage = tr(STR_ERROR_GENERAL_FAILURE);
    requestUpdate();
    return;
  }
  const size_t candidateRowOffset = pageInfo.prevUrl.empty() ? 0 : 1;
  // One format batch at a time bounds scratch to 100 * 169 bytes off-stack.
  std::vector<std::string> candidatePaths;
  candidatePaths.reserve(MAX_LISTING_ROWS);
  candidatePaths.resize(candidateRowCount);
  std::vector<uint8_t> candidateFlags;
  candidateFlags.reserve(MAX_LISTING_ROWS);
  HalFile candidateFile;
  if (candidateRowCount > 0 && !Storage.openFileForRead("BOCat", SESSION_CACHE_PATH, candidateFile)) {
    LOG_ERR("BOCat", "Local listing stage failed: could not reopen cache for offline flags");
  } else {
    for (const char* format : LOCAL_DOWNLOAD_FORMATS) {
      for (size_t row = 0; row < candidateRowCount; ++row) {
        candidatePaths[row].clear();
        candidateFile.seek(entryOffsets[row + candidateRowOffset]);
        const BookOrbitEntry entry = readEntry(candidateFile);
        if (hasLocalFormat(entry, format)) candidatePaths[row] = localPathFor(entry.subtitle, entry.title, format);
      }
      BookOrbitLocalLibrary::downloadedFlags(candidatePaths, candidateFlags);
      for (size_t row = 0; row < candidateRowCount; ++row) {
        if (candidateFlags[row]) downloadedFlags[row + candidateRowOffset] = 1;
      }
    }
  }
  const size_t slot = static_cast<size_t>(selectedTab());
  if (slot < contexts.size()) contexts[slot].url = url;
  if (resetSelection) activeNav().requestSelection(0);
  statusMessage.clear();
  state = BrowserState::BROWSING;
  ensureRowsFor(activeNav().selected.load(), 0);
  requestUpdate();
}

void BookOrbitLibraryActivity::fetchSearch(const std::string& query) {
  fetchListing(std::string("@offline-search:1:") + query);
}

void BookOrbitLibraryActivity::navigateToEntry(const BookOrbitEntry& entry) {
  if (entry.href.empty()) return;
  const size_t slot = static_cast<size_t>(selectedTab());
  if (slot >= contexts.size()) return;
  TabContext& context = contexts[slot];
  if (context.depth >= MAX_DRILLDOWN_DEPTH) return;
  context.frames[context.depth++] = NavigationFrame{currentUrl, activeNav().selected.load(), isDashboardView};
  context.url = entry.href;
  activeNav().requestSelection(0);
  fetchListing(context.url);
}

void BookOrbitLibraryActivity::navigateBack() {
  const size_t slot = static_cast<size_t>(selectedTab());
  if (slot >= contexts.size() || contexts[slot].depth == 0) {
    if (tabs[slot] == Tab::Continue || tabs[slot] == Tab::Discover) onGoHome();
    else focusTabs();
    return;
  }
  TabContext& context = contexts[slot];
  const NavigationFrame previous = context.frames[--context.depth];
  context.url = previous.url;
  activeNav().requestSelection(previous.selected);
  if (previous.dashboard) {
    isDashboardView = true;
    state = BrowserState::BROWSING;
    {
      RenderLock lock(*this);
      rowWindow.clear();
      rowWindowFirst = 0;
    }
    currentUrl.clear();
    ensureRowsFor(previous.selected, 0);
    requestUpdate();
  } else {
    fetchListing(previous.url, false);
  }
}


void BookOrbitLibraryActivity::openBookDetail(const BookOrbitEntry& entry) {
  stopDashboardCoverWork(true);
  if (!downloadAfterDetail && WiFi.status() != WL_CONNECTED && entry.bookId > 0 &&
      BookOrbitCatalogStore::available()) {
    detail = {};
    detail.bookId = entry.bookId;
    detail.title = entry.title;
    detail.authors = entry.subtitle;
    detail.series = entry.series;
    detail.progressPercent = entry.progressPercent;
    detail.hasCover = entry.hasCover;
    pendingDetailEntry = entry;
    offlineDetail = true;
    downloadedBookPath = downloadedPathFor(entry);
    coverBmpPath = coverPathFor(entry.bookId);
    coverAvailable = validCoverBmp(coverBmpPath);
    statusMessage = tr(STR_BO_REMOTE_ONLY);
    state = BrowserState::BOOK_DETAIL;
    requestUpdate();
    return;
  }
  offlineDetail = false;
  if (WiFi.status() != WL_CONNECTED || WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    pendingDetailEntry = entry;
    detailConnectPending = true;
    state = BrowserState::CHECK_WIFI;
    statusMessage = tr(STR_CHECKING_WIFI);
    requestUpdate();
    checkAndConnectWifi();
    return;
  }
  pendingDetailEntry = entry;
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();
  detail = {};
  const auto error = BookOrbitCatalogClient::fetchDetail(entry.href, detail);
  if (error != BookOrbitCatalogClient::OK) {
    detailConnectPending = true;
    showError(error);
    return;
  }
  // The listing already carries what the detail response omits for a device
  // (the numeric id lives in both, but a search result's href is all we kept).
  if (detail.bookId < 0) detail.bookId = entry.bookId;
  applyCachedStateToDetail(detail, false);
  downloadedBookPath = downloadedPathFor(detail);
  pendingDetailEntry = {};
  detailConnectPending = false;

  fetchCover(entry);

  statusMessage.clear();
  state = BrowserState::BOOK_DETAIL;
  requestUpdate();
  if (downloadAfterDetail) {
    downloadAfterDetail = false;
    chooseBookFormat();
  }
}

void BookOrbitLibraryActivity::fetchCover(const BookOrbitEntry& entry) {
  coverAvailable = false;
  coverBmpPath.clear();
  if (!detail.hasCover && !entry.hasCover) return;
  if (detail.bookId < 0) return;

  coverBmpPath = coverPathFor(detail.bookId);
  if (!recoverCoverCache(coverBmpPath)) {
    coverBmpPath.clear();
    return;
  }
  const bool cachedCover = validCoverBmp(coverBmpPath);
  if (cachedCover) {
    coverAvailable = true;
    HalFile file;
    if (Storage.openFileForRead("BOCat", coverBmpPath.c_str(), file)) {
      Bitmap bmp(file);
      // Older detail thumbnails used a tall crop box that discarded most of
      // the cover. Replace them transactionally, retaining them while offline.
      if (bmp.parseHeaders() == BmpReaderError::Ok && bmp.getHeight() <= bmp.getWidth() * 2) return;
    }
  }

  // HttpDownloader::downloadToFile() opens its own SecureHttpClient rather than
  // reusing BookOrbitCatalogClient's kept-alive session, so this would otherwise
  // hold TWO live wolfSSL connections at once -- the catalog session and this
  // download's fresh handshake. A handshake needs ~26 KB contiguous heap
  // (KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS); with the session's buffers
  // already resident it routinely fails to find that block on this heap, so the
  // download silently fails and the detail screen shows no cover. Close the
  // persistent session for the download's duration; ResumeSession reopens it on
  // every return path so later catalog requests (status/rating, next listing)
  // keep their keep-alive behavior.
  BookOrbitCatalogClient::endPersistentSession();
  struct ResumeSession {
    ~ResumeSession() { BookOrbitCatalogClient::beginPersistentSession(); }
  } resumeSession;
  if (HttpDownloader::downloadToFile(BookOrbitCatalogClient::thumbnailUrl(detail.bookId), COVER_JPG_PATH,
                                     BookOrbitCatalogClient::authHeaders()) != HttpDownloader::OK) {
    LOG_DBG("BOCat", "Cover download failed for book %ld", static_cast<long>(detail.bookId));
    if (!cachedCover) coverBmpPath.clear();
    return;
  }

  const int coverMaxW = std::max(1, renderer.getScreenWidth() / 3);
  const int coverMaxH = std::max(1, std::min(renderer.getScreenHeight() * 2 / 3, coverMaxW * 3 / 2));
  coverAvailable = publishCoverBmp(coverBmpPath, coverMaxW, coverMaxH);
  if (!coverAvailable) {
    coverAvailable = cachedCover;
    if (!cachedCover) coverBmpPath.clear();
    LOG_INF("BOCat", "Cover JPEG conversion/publication failed (see JPG log for details)");
  }
}


void BookOrbitLibraryActivity::chooseBookFormat() {
  if (detail.files.empty()) {
    state = BrowserState::ERROR;
    errorMessage = tr(STR_DOWNLOAD_FAILED);
    requestUpdate();
    return;
  }
  if (detail.files.size() == 1) {
    downloadBook(detail.files[0]);
    return;
  }
  formatSelectorIndex = 0;
  state = BrowserState::FORMAT_SELECTION;
  requestUpdate();
}

void BookOrbitLibraryActivity::downloadBook(const BookOrbitFile& file) {
  state = BrowserState::DOWNLOADING;
  statusMessage = detail.title;
  downloadProgress = 0;
  downloadTotal = 0;
  requestUpdate(true);

  // Reuse the OPDS download folder and filename-format settings: both name
  // "where books fetched over the network land", and a second pair of settings
  // for the same decision would only be a way to get them out of step.
  const char* folder = SETTINGS.opdsDownloadFolder;  // "" => SD root
  bool haveFolder = folder[0] != '\0';
  if (haveFolder && !Storage.exists(folder) && !Storage.mkdir(folder)) {
    LOG_ERR("BOCat", "mkdir failed for download folder; using SD root");
    haveFolder = false;
  }

  std::string filename;
  filename.reserve(96);
  if (haveFolder) filename += folder;
  filename += '/';
  filename += opdsBookFilename(detail.authors, detail.title,
                               static_cast<OpdsFilenameFormat>(SETTINGS.opdsFilenameFormat), extensionFor(file.format));

  const std::string url = BookOrbitCatalogClient::resolveUrl(file.downloadUrl);
  LOG_DBG("BOCat", "Downloading catalog file");

  uint32_t lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      url, filename, BookOrbitCatalogClient::authHeaders(),
      [this, &lastProgressUpdateMs](const unsigned int downloaded, const unsigned int total) {
        mappedInput.update();
        downloadProgress = downloaded;
        downloadTotal = total;
        const uint32_t now = millis();
        if (now - lastProgressUpdateMs >= 3000) {
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
        return !mappedInput.wasPressed(MappedInputManager::Button::Back);
      });

  if (result == HttpDownloader::OK) {
    bool downloadedFileValid = false;
    {
      HalFile downloaded;
      downloadedFileValid = Storage.openFileForRead("BOCat", filename, downloaded) && downloaded.size() > 0;
    }
    if (!downloadedFileValid) {
      LOG_ERR("BOCat", "Downloaded file is empty or unreadable");
      Storage.remove(filename.c_str());
      state = BrowserState::ERROR;
      errorMessage = tr(STR_DOWNLOAD_FAILED);
      requestUpdate();
      return;
    }

    // Drop any cache left by an earlier copy of the same path, so a redownload
    // is not read through a stale index.
    if (file.format == "epub") {
      Epub(filename, "/.crosspoint").clearCache();
    } else if (file.format == "xtc" || file.format == "xtch") {
      Xtc(filename, "/.crosspoint").clearCache();
    }
    // Upstream owns local metadata and indexing. A same-size replacement must not retain
    // details.bin (including TXT/MD), and the HAL's durable content marker triggers the
    // normal New/Authors rebuild without erasing firstSeen or publishing remote-only rows.
    const std::string detailsPath = ReaderActivity::bookCacheDir(filename) + "/details.bin";
    if (Storage.exists(detailsPath.c_str())) Storage.remove(detailsPath.c_str());
    BookOrbitCatalogClient::endPersistentSession();
    BookDetails localDetails;
    uint32_t localSize = 0;
    {
      HalFile downloaded;
      if (Storage.openFileForRead("BOCat", filename.c_str(), downloaded)) {
        localSize = static_cast<uint32_t>(downloaded.size());
      }
    }
    if (!BookDetailsLookup::parse(filename, localSize, localDetails)) {
      LOG_INF("BOCat", "Local metadata deferred to Library");
    }
    BookOrbitCatalogClient::beginPersistentSession();

    downloadedBookPath = filename;
    statusMessage = tr(STR_BO_DOWNLOADED);
    state = BrowserState::BOOK_DETAIL;
    requestUpdate();
    return;
  }

  if (result == HttpDownloader::ABORTED) {
    statusMessage.clear();
    state = BrowserState::BOOK_DETAIL;
    requestUpdate();
    return;
  }

  state = BrowserState::ERROR;
  errorMessage = tr(STR_DOWNLOAD_FAILED);
  requestUpdate();
}

void BookOrbitLibraryActivity::applyReadStatus(const BookOrbitCatalogClient::ReadStatus status) {
  stopDashboardCoverWork(true);
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const std::string* hash = statusContentHash(detail);
  std::string modifiedDay;
  BookOrbitCatalogClient::Error error = BookOrbitCatalogClient::OK;
  if (stateStatusSupported(status) && hash && BookOrbitBookStateSync::currentModifiedDay(modifiedDay)) {
    const auto queued = BookOrbitBookStateStore::queueStatus(*hash, status, modifiedDay);
    BookOrbitBookStateStore::State sent;
    if (queued != BookOrbitBookStateStore::QueueResult::Queued) {
      error = BookOrbitCatalogClient::NETWORK_ERROR;
    } else if (!BookOrbitBookStateStore::read(*hash, sent) || !sent.pendingStatus || sent.localStatus != status ||
               sent.statusModified != modifiedDay) {
      statusMessage = tr(STR_BO_STATE_SYNC_PENDING);
    } else {
      error = BookOrbitCatalogClient::setReadStatus(detail.bookId, status);
      if (error == BookOrbitCatalogClient::OK) {
        BookOrbitBookStateResult canonical;
        const auto refreshError = BookOrbitBookStateSync::refreshCanonical(*hash, detail.bookId, canonical);
        if (refreshError == BookOrbitCatalogClient::OK && BookOrbitBookStateStore::acknowledge(sent, true, false)) {
          detail.ratingTenths = canonical.ratingSet && canonical.rating > 0 ? canonical.rating * 10 : -1;
          statusMessage = tr(STR_BO_STATUS_UPDATED);
        } else {
          statusMessage = tr(STR_BO_STATE_SYNC_PENDING);
        }
      }
    }
  } else {
    error = BookOrbitCatalogClient::setReadStatus(detail.bookId, status);
    if (error == BookOrbitCatalogClient::OK) {
      detail.readStatus = BookOrbitCatalogClient::readStatusKey(status);
      if (hash) {
        BookOrbitBookStateResult canonical;
        if (BookOrbitBookStateSync::refreshCanonical(*hash, detail.bookId, canonical) == BookOrbitCatalogClient::OK) {
          detail.ratingTenths = canonical.ratingSet && canonical.rating > 0 ? canonical.rating * 10 : -1;
        } else {
          BookOrbitBookStateStore::rememberCanonicalStatus(*hash, status);
        }
      }
      statusMessage = tr(STR_BO_STATUS_UPDATED);
    }
  }
  if (error != BookOrbitCatalogClient::OK && error != BookOrbitCatalogClient::NOT_FOUND) {
    statusMessage = std::string(tr(STR_BO_UPDATE_FAILED)) + ": " + BookOrbitCatalogClient::errorString(error);
  }
  applyCachedStateToDetail(detail, true);
  state = BrowserState::BOOK_DETAIL;
  requestUpdate();
}

void BookOrbitLibraryActivity::applyRating(const uint8_t stars) {
  stopDashboardCoverWork(true);
  state = BrowserState::LOADING;
  statusMessage = tr(STR_LOADING);
  requestUpdateAndWait();

  const std::string* hash = statusContentHash(detail);
  std::string modifiedDay;
  BookOrbitCatalogClient::Error error = BookOrbitCatalogClient::OK;
  if (hash && BookOrbitBookStateSync::currentModifiedDay(modifiedDay)) {
    const auto queued = BookOrbitBookStateStore::queueRating(*hash, stars, modifiedDay);
    BookOrbitBookStateStore::State sent;
    if (queued != BookOrbitBookStateStore::QueueResult::Queued) {
      error = BookOrbitCatalogClient::NETWORK_ERROR;
    } else if (!BookOrbitBookStateStore::read(*hash, sent) || !sent.pendingRating || sent.rating != stars ||
               sent.ratingCleared != (stars == 0) || sent.ratingModified != modifiedDay) {
      statusMessage = tr(STR_BO_STATE_SYNC_PENDING);
    } else {
      error = BookOrbitCatalogClient::setRating(detail.bookId, stars);
      if (error == BookOrbitCatalogClient::OK) {
        BookOrbitBookStateResult canonical;
        const auto refreshError = BookOrbitBookStateSync::refreshCanonical(*hash, detail.bookId, canonical);
        if (refreshError == BookOrbitCatalogClient::OK && BookOrbitBookStateStore::acknowledge(sent, false, true)) {
          detail.ratingTenths = canonical.ratingSet && canonical.rating > 0 ? canonical.rating * 10 : -1;
          statusMessage = tr(STR_BO_RATING_UPDATED);
        } else {
          detail.ratingTenths = stars == 0 ? -1 : static_cast<int16_t>(stars * 10);
          statusMessage = tr(STR_BO_STATE_SYNC_PENDING);
        }
      }
    }
  } else {
    error = BookOrbitCatalogClient::setRating(detail.bookId, stars);
    if (error == BookOrbitCatalogClient::OK) {
      detail.ratingTenths = stars == 0 ? -1 : static_cast<int16_t>(stars * 10);
      if (hash) {
        BookOrbitBookStateResult canonical;
        if (BookOrbitBookStateSync::refreshCanonical(*hash, detail.bookId, canonical) == BookOrbitCatalogClient::OK) {
          detail.ratingTenths = canonical.ratingSet && canonical.rating > 0 ? canonical.rating * 10 : -1;
        }
      }
      statusMessage = tr(STR_BO_RATING_UPDATED);
    }
  }
  if (error != BookOrbitCatalogClient::OK && error != BookOrbitCatalogClient::NOT_FOUND) {
    statusMessage = std::string(tr(STR_BO_UPDATE_FAILED)) + ": " + BookOrbitCatalogClient::errorString(error);
  }
  applyCachedStateToDetail(detail, true);
  state = BrowserState::BOOK_DETAIL;
  requestUpdate();
}

void BookOrbitLibraryActivity::launchSearch() {
  stopDashboardCoverWork(true);
  consumeConfirm = true;
  state = BrowserState::SEARCH_INPUT;
  requestUpdate();

  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_SEARCH));
  if (!keyboard) {
    LOG_ERR("BOCat", "OOM: search keyboard activity");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    state = BrowserState::BROWSING;
    // The keyboard consumes its submit release; only suppress a still-held button.
    consumeConfirm = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    if (!result.isCancelled) {
      performSearch(std::get<KeyboardResult>(result.data).text);
    } else {
      requestUpdate();
    }
  });
}

void BookOrbitLibraryActivity::performSearch(const std::string& query) {

  const size_t slot = static_cast<size_t>(selectedTab());
  if (slot >= contexts.size() || contexts[slot].depth >= MAX_DRILLDOWN_DEPTH) return;
  TabContext& context = contexts[slot];
  context.frames[context.depth++] = NavigationFrame{currentUrl, activeNav().selected.load(), isDashboardView};
  context.url = std::string("@offline-search:1:") + query;
  fetchSearch(query);
}

void BookOrbitLibraryActivity::showError(const BookOrbitCatalogClient::Error error) {
  errorMessage = BookOrbitCatalogClient::errorString(error);
  if (BookOrbitCatalogClient::lastHttpCode > 0) {
    errorMessage += " (HTTP " + std::to_string(BookOrbitCatalogClient::lastHttpCode) + ")";
  }
  LOG_ERR("BOCat", "Request failed: %s | heap=%u contig=%u", errorMessage.c_str(),
          BookOrbitCatalogClient::lastHeapAtFailure, BookOrbitCatalogClient::lastContigHeapAtFailure);
  DiskLogger::flushNow();
  state = BrowserState::ERROR;
  requestUpdate();
}

void BookOrbitLibraryActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    BookOrbitCatalogClient::beginPersistentSession();
    if (detailConnectPending) {
      BookOrbitEntry entry = std::move(pendingDetailEntry);
      pendingDetailEntry = {};
      detailConnectPending = false;
      openBookDetail(entry);
    } else {
      if (refreshPending) refreshCatalog();
    }
    return;
  }
  launchWifiSelection();
}

void BookOrbitLibraryActivity::launchWifiSelection() {
  state = BrowserState::WIFI_SELECTION;
  requestUpdate();

  auto wifiSelection = makeUniqueNoThrow<WifiSelectionActivity>(renderer, mappedInput);
  if (!wifiSelection) {
    LOG_ERR("BOCat", "OOM: WiFi selection activity");
    state = BrowserState::ERROR;
    errorMessage = tr(STR_MEMORY_ERROR);
    requestUpdate();
    return;
  }
  startActivityForResult(std::move(wifiSelection),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void BookOrbitLibraryActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    // Leave WiFi up; onExit's silent reboot handles teardown without fragmenting further.
    state = BrowserState::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
    return;
  }
  BookOrbitCatalogClient::beginPersistentSession();
  if (detailConnectPending) {
    BookOrbitEntry entry = std::move(pendingDetailEntry);
    pendingDetailEntry = {};
    detailConnectPending = false;
    openBookDetail(entry);
  } else {
    if (refreshPending) refreshCatalog();
  }
}

ListRowTap::Result BookOrbitLibraryActivity::selectListRow(const int index) {
  // One recorder, several lists: `state` is read on the loop task and decides
  // which selection a tap moves (see OpdsBookBrowserActivity).
  switch (state) {
    case BrowserState::FORMAT_SELECTION:
      return ListRowTap::apply(index, static_cast<int>(detail.files.size()), formatSelectorIndex);
    case BrowserState::STATUS_SELECTION:
      return ListRowTap::apply(index, STATUS_OPTION_COUNT, statusSelectorIndex);
    case BrowserState::RATING_SELECTION:
      return ListRowTap::apply(index, RATING_OPTION_COUNT, ratingSelectorIndex);
    case BrowserState::BROWSING:
      return TabbedUiListActivity::selectListRow(index);
    default:
      return ListRowTap::Result::Rejected;
  }
}
