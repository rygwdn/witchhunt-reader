#pragma once

#include <string>
#include <vector>

#include "../../../lib/BookOrbitCatalog/BookOrbitCatalogSettings.h"
#include "activities/UiListActivity.h"

class BookOrbitCatalogSettingsActivity final : public UiListActivity {
 public:
  BookOrbitCatalogSettingsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;

 private:
  int listCount() const override { return static_cast<int>(rows.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  const char* headerTitle() const override;
  void rebuildRows();

  BookOrbitCatalogSettings settings;
  std::vector<std::string> labels;
  std::vector<freeink::ui::ListItem> rows;
  std::string status;
  bool loaded = false;
};
