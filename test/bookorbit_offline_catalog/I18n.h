#pragma once

#include <cstring>

inline const char* testTranslation(const char* id) {
  if (std::strcmp(id, "STR_BOOKORBIT_LIBRARIES") == 0) return "Libraries";
  if (std::strcmp(id, "STR_BOOKORBIT_COLLECTIONS") == 0) return "Collections";
  if (std::strcmp(id, "STR_BOOKORBIT_SMART_SCOPES") == 0) return "Smart scopes";
  if (std::strcmp(id, "STR_TAB_AUTHORS") == 0) return "Authors";
  if (std::strcmp(id, "STR_BOOKORBIT_SERIES") == 0) return "Series";
  return id;
}

#define tr(id) testTranslation(#id)
