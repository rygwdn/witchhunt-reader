#pragma once

#include <string>
#include <vector>

#include "../../../lib/BookOrbitCatalog/BookOrbitCatalogSettings.h"
#include <HalStorage.h>
#include "activities/UiListActivity.h"

class BookOrbitSourcePickerActivity final : public UiListActivity {
 public:
  BookOrbitSourcePickerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                BookOrbitCatalogSettings::SourceKind kind);
  void onEnter() override;
  bool usesWifi() const override { return true; }
  void onExit() override;

 private:
  int listCount() const override;
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void fetch(const std::string& url);

  BookOrbitCatalogSettings::SourceKind kind;
  BookOrbitCatalogSettings settings;
  std::vector<BookOrbitCatalogSettings::Source> candidateWindow;
  HalFile candidateFile;
  uint64_t candidateCount = 0;
  uint64_t pageStart = 0;
  bool loadCandidateWindow();
  std::vector<freeink::ui::ListItem> rows;
  bool hasSavedSource() const;
  std::string error;
};
