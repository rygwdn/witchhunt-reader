#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

class BookOrbitCatalogSettings {
 public:
  enum class Scope : uint8_t { All, Selected };
  enum class SourceKind : uint8_t { Library, Collection, SmartScope };
  struct Source {
    SourceKind kind;
    int32_t id;
    std::string title;
  };

  static constexpr size_t MAX_SOURCES = 32;
  bool supportedOnly = true;
  Scope scope = Scope::All;
  std::vector<Source> sources;

  bool load();
  bool save() const;
  static bool supportsFormats(const std::string& formats) {
    size_t start = 0;
    while (start <= formats.size()) {
      size_t end = formats.find("·", start);
      if (end == std::string::npos) end = formats.size();
      size_t left = start;
      size_t right = end;
      while (left < right && std::isspace(static_cast<unsigned char>(formats[left]))) ++left;
      while (right > left && std::isspace(static_cast<unsigned char>(formats[right - 1]))) --right;
      constexpr const char* supported[] = {"epub", "xtc", "xtch", "txt", "md"};
      for (const char* token : supported) {
        const size_t length = std::strlen(token);
        if (right - left != length) continue;
        size_t i = 0;
        for (; i < length; ++i) {
          if (std::tolower(static_cast<unsigned char>(formats[left + i])) != token[i]) break;
        }
        if (i == length) return true;
      }
      if (end == formats.size()) break;
      start = end + std::strlen("·");
    }
    return false;
  }
  static const char* sourceQueryKey(SourceKind kind) {
    switch (kind) {
      case SourceKind::Library: return "libraryId";
      case SourceKind::Collection: return "collectionId";
      case SourceKind::SmartScope: return "smartScopeId";
    }
    return "";
  }
};
