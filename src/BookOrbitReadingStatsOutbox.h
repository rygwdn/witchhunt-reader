#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <BookOrbitCatalogClient.h>

// Durable queue for real reader page-turn intervals. Records are appended before they
// are eligible for upload and removed only after the server confirms every event in a batch.
namespace BookOrbitReadingStatsOutbox {

bool append(const std::string& hash, const BookOrbitPageStatEvent& event);
// Selects the smallest pending hash at/after cursor; includeCursor=false skips that exact hash.
bool nextBatch(BookOrbitPageStatsBook& out, const std::string& cursor, bool includeCursor);
bool acknowledge(const std::string& hash, const std::vector<BookOrbitPageStatEvent>& events);

}  // namespace BookOrbitReadingStatsOutbox
