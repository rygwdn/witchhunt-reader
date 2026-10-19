#include "BookOrbitCatalogSettingsActivity.h"

#include <I18n.h>

#include <memory>
#include "BookOrbitSourcePickerActivity.h"
#include "MappedInputManager.h"

namespace fui = freeink::ui;

BookOrbitCatalogSettingsActivity::BookOrbitCatalogSettingsActivity(GfxRenderer& renderer,
                                                                   MappedInputManager& mappedInput)
    : UiListActivity("BookOrbitSettings", renderer, mappedInput) {}

void BookOrbitCatalogSettingsActivity::onEnter() {
  UiListActivity::onEnter();
  loaded = settings.load();
  status = loaded ? "" : tr(STR_BOOKORBIT_LOAD_ERROR);
  rebuildRows();
}

const char* BookOrbitCatalogSettingsActivity::headerTitle() const { return tr(STR_BOOKORBIT_CATALOG_SETTINGS); }

void BookOrbitCatalogSettingsActivity::rebuildRows() {
  labels.clear();
  rows.clear();
  labels.reserve(7);
  rows.reserve(7);
  auto add = [&](const char* label, const char* value, int action) {
    labels.emplace_back(label);
    fui::ListItem item;
    item.label = labels.back().c_str();
    item.value = value;
    item.actionValue = static_cast<int16_t>(action);
    rows.push_back(item);
  };
  if (loaded) {
    add(tr(STR_BOOKORBIT_SUPPORTED_ONLY), settings.supportedOnly ? tr(STR_YES) : tr(STR_NO), 0);
    add(tr(STR_BOOKORBIT_SCOPE), settings.scope == BookOrbitCatalogSettings::Scope::All
                                     ? tr(STR_BOOKORBIT_SCOPE_ALL)
                                     : tr(STR_BOOKORBIT_SCOPE_SELECTED), 1);
    add(tr(STR_BOOKORBIT_LIBRARIES), nullptr, 2);
    add(tr(STR_BOOKORBIT_COLLECTIONS), nullptr, 3);
    add(tr(STR_BOOKORBIT_SMART_SCOPES), nullptr, 4);
  }
  add(tr(STR_BOOKORBIT_APPLIES_ON_REFRESH), status.empty() ? "" : status.c_str(), 5);
}

void BookOrbitCatalogSettingsActivity::buildScreen(UiScreen& screen) {
  rebuildRows();
  layoutListArea(screen);
  auto props = listProps(screen);
  props.items = rows.data();
  props.count = static_cast<uint16_t>(rows.size());
  addList(screen, props);
}

void BookOrbitCatalogSettingsActivity::activateIndex(int index) {
  if (!loaded || index < 0 || static_cast<size_t>(index) >= rows.size()) return;
  const int action = rows[static_cast<size_t>(index)].actionValue;
  if (action >= 2 && action <= 4) {
    const auto kind = action == 2 ? BookOrbitCatalogSettings::SourceKind::Library
                      : action == 3 ? BookOrbitCatalogSettings::SourceKind::Collection
                                    : BookOrbitCatalogSettings::SourceKind::SmartScope;
    startActivityForResult(
        std::make_unique<BookOrbitSourcePickerActivity>(renderer, mappedInput, kind), [this](const ActivityResult&) {
          loaded = settings.load();
          status = loaded ? "" : tr(STR_BOOKORBIT_LOAD_ERROR);
          requestUpdate();
        });
    return;
  }
  const bool previousSupportedOnly = settings.supportedOnly;
  const auto previousScope = settings.scope;
  if (action == 0) settings.supportedOnly = !settings.supportedOnly;
  else if (action == 1) settings.scope = settings.scope == BookOrbitCatalogSettings::Scope::All
                                             ? BookOrbitCatalogSettings::Scope::Selected
                                             : BookOrbitCatalogSettings::Scope::All;
  else return;
  if (settings.save()) {
    status.clear();
  } else {
    settings.supportedOnly = previousSupportedOnly;
    settings.scope = previousScope;
    status = tr(STR_BOOKORBIT_SAVE_ERROR);
  }
  requestUpdate();
}
