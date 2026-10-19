#pragma once
#include <BookOrbitCatalogStore.h>
#include <BookOrbitCatalogTypes.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "activities/ListRowTap.h"
#include "activities/TabbedUiListActivity.h"
#include "network/HttpDownloader.h"
#include "util/ListRowCache.h"

namespace fui = freeink::ui;
/** Native, cache-first BookOrbit library browser. */
class BookOrbitLibraryActivity final : public TabbedUiListActivity {
 public:
  enum class BrowserState {
    CHECK_WIFI,
    WIFI_SELECTION,
    LOADING,
    BROWSING,
    BOOK_DETAIL,
    FORMAT_SELECTION,
    STATUS_SELECTION,
    RATING_SELECTION,
    DOWNLOADING,
    ERROR,
    SEARCH_INPUT,
  };

  explicit BookOrbitLibraryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : TabbedUiListActivity("BookOrbitLibrary", renderer, mappedInput, listDeclaration()) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  ListRowTap::Result selectListRow(int index) override;
 private:
  enum class Tab : uint8_t { Continue, Discover, New, Books, Browse };
  static constexpr size_t TAB_CAPACITY = 5;
  static constexpr size_t DASHBOARD_SECTION_COUNT = 3;
  static constexpr size_t MAX_DASHBOARD_NAV_ITEMS = 8;
  static constexpr size_t MAX_DASHBOARD_SHELF_ITEMS = 6;
  static constexpr size_t MAX_DASHBOARD_COVER_ITEMS =
      MAX_DASHBOARD_NAV_ITEMS + MAX_DASHBOARD_SHELF_ITEMS * 2;
  static constexpr size_t MAX_LISTING_ROWS = 100;
  static constexpr size_t MAX_DRILLDOWN_DEPTH = 8;
  static constexpr size_t ROW_CACHE_OPENING_ROWS = 16;

  struct DashboardCoverCandidate {
    int32_t bookId = -1;
    bool needsDownload = false;
  };

  struct NavigationFrame {
    std::string url;
    int selected = 0;
    bool dashboard = false;
  };
  struct TabContext {
    std::array<NavigationFrame, MAX_DRILLDOWN_DEPTH> frames{};
    uint8_t depth = 0;
    std::string url;
  };
  std::array<DashboardCoverCandidate, MAX_DASHBOARD_COVER_ITEMS> dashboardCoverCandidates{};
  // Reuse a single bounded header list; drop the TLS session before the catalog client resumes.
  HttpDownloader::HeaderList dashboardCoverHeaders;
  std::optional<HttpDownloader::Session> dashboardCoverSession;
  size_t dashboardCoverCount = 0;
  size_t dashboardCoverPosition = 0;
  uint32_t refreshStartMs = 0;
  uint32_t refreshProgressAtMs = 0;
  uint32_t dashboardCoverStartMs = 0;
  unsigned dashboardCoverAttempted = 0;
  unsigned dashboardCoverPublished = 0;

  BrowserState state = BrowserState::CHECK_WIFI;
  BrowserState detailReturnState = BrowserState::BROWSING;
  std::array<Tab, TAB_CAPACITY> tabs{};
  std::array<TabContext, TAB_CAPACITY> contexts{};
  size_t tabCountValue = 0;

  // Session caches keep strings off the ESP32 heap; one active list window is
  // decoded on the loop task for the render task's row provider.
  std::vector<uint32_t> entryOffsets;
  std::vector<uint8_t> downloadedFlags;
  std::vector<uint32_t> shelfEntryOffsets;
  std::vector<uint8_t> shelfDownloadedFlags;
  std::vector<uint16_t> dashboardOrder;
  std::array<uint16_t, DASHBOARD_SECTION_COUNT> dashboardSectionStart{};
  std::array<uint16_t, DASHBOARD_SECTION_COUNT> dashboardSectionCount{};
  std::vector<BookOrbitEntry> rowWindow;
  int rowWindowFirst = 0;
  int pageTop = -1;
  std::string activeCachePath;
  std::string shelfCachePath;
  std::string newRoute;
  char rowValueBuffer[48] = {};
  std::string currentUrl;
  std::string errorMessage;
  std::string statusMessage;
  std::string downloadedBookPath;
  std::string coverBmpPath;

  BookOrbitPageInfo pageInfo;
  BookOrbitEntry pendingDetailEntry;
  BookOrbitBookDetail detail;

  int formatSelectorIndex = 0;
  int statusSelectorIndex = 0;
  int ratingSelectorIndex = 0;
  size_t downloadProgress = 0;
  size_t downloadTotal = 0;
  bool consumeConfirm = false;
  bool memoryTrimmed = false;
  bool coverAvailable = false;
  bool detailConnectPending = false;
  bool refreshPending = false;
  bool offlineDetail = false;
  bool downloadAfterDetail = false;
  bool restartToDownloadedBook = false;
  bool initialLoadPending = false;
  bool isDashboardView = false;
  bool isNewTabAvailable = false;
  bool refreshInProgress = false;
  bool dashboardCoversPending = false;
  bool dashboardCoverBackRequested = false;

  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onListAction(ListGrammar::Side side, int position) override;
  void onSelectionChanged(int index) override;
  void showPositionPage(int position, int topPosition) override;
  void backFromPosition(int position) override;
  void onTabSelected(int slot) override;
  void onBackFromTabs() override;
  const char* headerTitle() const override;
  const char* footerConfirmLabel() const override;
  void customizeTabBar(UiScreen& screen, freeink::ui::TabBarProps& props) override;
  int tabCount() const override { return static_cast<int>(tabCountValue); }
  const char* tabLabel(int slot) const override;
  static ListDeclaration listDeclaration();

  static void provideRow(void* ctx, uint16_t index, fui::ListItem& item);
  BookOrbitEntry getEntry(size_t index) const;
  BookOrbitEntry getDashboardEntry(size_t position) const;
  BookOrbitEntry getVisibleEntry(size_t index) const;
  bool loadDashboard();
  bool fetchDashboard();
  void fetchDashboardCovers();
  void advanceCatalogRefresh();
  void cancelCatalogRefresh();
  void restoreCachedCatalogView();
  void processDashboardCover();
  void stopDashboardCoverWork(bool resumeCatalogSession);
  void ensureRowsFor(int selected, int windowTop);
  void openDownloadedBook(const std::string& path);

  void checkAndConnectWifi();
  void launchWifiSelection();
  void onWifiSelectionComplete(bool connected);
  void fetchListing(const std::string& url, bool resetSelection = true);
  void openCatalogRoot();
  void refreshCatalog();
  void startDashboardCoverWork();
  void fetchSearch(const std::string& query);
  void navigateToEntry(const BookOrbitEntry& entry);
  void navigateBack();
  void openBookDetail(const BookOrbitEntry& entry);
  void fetchCover(const BookOrbitEntry& entry);
  void chooseBookFormat();
  void downloadBook(const BookOrbitFile& file);
  void applyReadStatus(BookOrbitCatalogClient::ReadStatus status);
  void applyRating(uint8_t stars);
  void launchSearch();
  void performSearch(const std::string& query);
  void showError(BookOrbitCatalogClient::Error error);

  bool preventAutoSleep() override { return true; }
  bool shouldSkipPeriodicUpdate() const override { return true; }
};
