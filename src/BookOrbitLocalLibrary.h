#pragma once

#include <LibraryFormat.h>

#include <cstdint>
#include <string>
#include <vector>

namespace BookOrbitLocalLibrary {

// Local files remain owned by the upstream library. The catalog's expected filename and
// optional file size are a lookup key, not another persistent downloaded-book database.
std::string downloadedPath(const std::string& expectedPath, uint32_t expectedSize = 0,
                           const char* indexPath = library::INDEX_PATH);
void downloadedFlags(const std::vector<std::string>& expectedPaths, std::vector<uint8_t>& flags,
                     const char* indexPath = library::INDEX_PATH);

}  // namespace BookOrbitLocalLibrary
