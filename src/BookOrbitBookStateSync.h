#pragma once

#include <BookOrbitCatalogClient.h>

#include <string>

namespace BookOrbitBookStateSync {

// UTC date sent as the server's date-granularity conflict key.
bool currentModifiedDay(std::string& day);

// Resolve one hash, upload any durable edits, and refresh canonical state even when
// there is no pending edit. Does not open or close a catalog session.
BookOrbitCatalogClient::Error syncHash(const std::string& hash, BookOrbitBookStateResult& result);
// Fetch and cache canonical state without sending any queued field.
BookOrbitCatalogClient::Error refreshCanonical(const std::string& hash, int32_t bookId,
                                               BookOrbitBookStateResult& result);
// Refresh active-reader state first, then mark untouched Unread/WantToRead books as Reading.
BookOrbitCatalogClient::Error syncActiveHash(const std::string& hash, BookOrbitBookStateResult& result);

// Drain durable book-state changes. Unmatched hashes remain queued for a later
// library change; transport/server failures stop this pass without acknowledging.
void drain();

}  // namespace BookOrbitBookStateSync
