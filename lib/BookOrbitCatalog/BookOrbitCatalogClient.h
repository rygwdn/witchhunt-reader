#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "BookOrbitBookStateTypes.h"
#include "BookOrbitBookmarkTypes.h"
#include "BookOrbitCatalogListParser.h"
#include "BookOrbitCatalogSnapshotParser.h"
#include "BookOrbitCatalogTypes.h"
struct BookOrbitPageStatEvent {
  uint32_t page = 0;
  uint32_t startTime = 0;
  uint32_t durationSeconds = 0;
  uint32_t totalPages = 0;
};

struct BookOrbitPageStatsBook {
  std::string hash;
  int32_t bookFileId = -1;
  std::vector<BookOrbitPageStatEvent> events;
};

/**
 * HTTP client for BookOrbit's KOReader catalog API.
 *
 * Authentication is the KOReader sync account already stored in
 * KOReaderCredentialStore: the server guards `koreader/plugin/...` with the
 * same `x-auth-user` / `x-auth-key` headers as `syncs/progress`, so a device
 * that can sync progress can already browse the library — there is no second
 * account, and no second credential screen.
 *
 * Endpoints used (all relative to KOReaderCredentialStore::getBaseUrl()):
 *   GET  plugin/version                     capability probe
 *   GET  plugin/catalog/root                top-level navigation
 *   GET  plugin/catalog/sections/:name      libraries/collections/authors/series
 *   GET  plugin/catalog/books?...           paginated book listings and search
 *   GET  plugin/catalog/books/:id           book detail (+ downloadable files)
 *   PUT  plugin/catalog/books/:id/read-status
 *   PUT  plugin/catalog/books/:id/rating
 *   GET  plugin/catalog/books/:id/thumbnail JPEG cover
 *   GET  plugin/catalog/files/:id/download  book file
 *
 * Listings stream through BookOrbitCatalogListParser so a page is never
 * resident; only the detail response (one book, long description) is buffered
 * and DOM-parsed. Every request is refused up front when the contiguous heap is
 * too small for a TLS handshake, the same gate KOReaderSyncClient applies.
 */
class BookOrbitCatalogClient {
 public:
  enum Error {
    OK = 0,
    NO_CREDENTIALS,
    NETWORK_ERROR,
    AUTH_FAILED,
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    NOT_SUPPORTED,  // reachable KOReader server, but no BookOrbit catalog on it
  };

  /** Read statuses the catalog accepts from a device (server-side whitelist). */
  enum class ReadStatus : uint8_t {
    Unread = 0,
    WantToRead,
    Reading,
    OnHold,
    Read,
    Abandoned,
  };

  /**
   * Accept any TLS certificate. Mirrors the app's "skip HTTPS validation"
   * setting, which this library cannot read itself; the app pushes the current
   * value before use. Off by default — requests carry the account credentials.
   */
  static void setSkipTlsValidation(bool skip);

  /**
   * Ask the configured KOReader server whether it serves the BookOrbit catalog.
   * OK means it does; NOT_SUPPORTED means it answered but is a plain kosync
   * server (or predates the catalog API). The verdict is what gates the library
   * entry on the home screen, cached in KOReaderCredentialStore.
   */
  static Error probeCapabilities();

  /**
   * Fetch one listing (root, a section, or a page of books) and hand each row to
   * `onEntry` as it arrives. `pathOrUrl` may be empty (the catalog root), a
   * server-absolute href taken from a previous response, or a full URL.
   * Pagination links of the response land in outPage.
   */
  static Error fetchListing(const std::string& pathOrUrl, const BookOrbitCatalogListParser::EntryCallback& onEntry,
                            BookOrbitPageInfo& outPage);
  /** Stream one immutable BookOrbit snapshot page; query excludes the endpoint path. */
  static Error fetchSnapshot(const std::string& query, const BookOrbitSnapshotCallbacks& callbacks,
                             BookOrbitSnapshotPage& outPage);

  /** Wire URL of a server-side search page. The library fetches it through
   *  fetchListing() so search results page exactly like any other listing. */
  static std::string searchUrl(const std::string& query);

  /** Fetch one book's detail, including the list of downloadable files. */
  static Error fetchDetail(const std::string& pathOrUrl, BookOrbitBookDetail& outDetail);

  /** Set a book's reading status. */
  static Error setReadStatus(int32_t bookId, ReadStatus status);

  /**
   * Set or clear a book's rating. `stars` is 1..5; pass 0 to clear it (the
   * server models "unrated" as a null rating).
   */
  static Error setRating(int32_t bookId, uint8_t stars);
  /** Resolve one content MD5 to the exact catalog book/file ids. */
  static Error matchCheck(const std::string& hash, BookOrbitMatchCheckResponse& response);
  /** Upload one status/rating record and stream the canonical review note to `noteSink`. */
  static Error uploadBookState(const BookOrbitBookStateUpload& state, BookOrbitReviewNoteSink noteSink,
                               void* noteContext, BookOrbitBookStateResult& result);
  /** Upload an acknowledged batch of actual page-reading events. */
  static Error uploadPageStats(const std::vector<BookOrbitPageStatsBook>& books, std::string& response);

  /** Exchange bounded device bookmark changes and fetch one push-down page. */
  static Error exchangeBookmarks(const BookOrbitBookmarkExchangeRequest& request,
                                 BookOrbitBookmarkExchangeResponse& response);
  /** Acknowledge only durable local bookmark applications/deletions. */
  static Error acknowledgeBookmarks(const BookOrbitBookmarkAckRequest& request, std::string& response);

  /**
   * Absolute URL for a server-supplied href. Responses carry server-absolute
   * paths ("/api/v1/koreader/plugin/catalog/..."), which resolve against the
   * base URL's ORIGIN rather than its path; a full URL is returned unchanged.
   */
  static std::string resolveUrl(const std::string& pathOrUrl);

  /** URL of a book's JPEG cover thumbnail. */
  static std::string thumbnailUrl(int32_t bookId);

  /**
   * Auth headers for a transfer the app performs itself. File and cover
   * downloads go through HttpDownloader (WiFi power-save handling, SD sink,
   * progress plumbing), which lives in src/ and cannot be called from a
   * library — so the library hands out the headers instead of the transfer.
   */
  static std::vector<std::pair<std::string, std::string>> authHeaders();

  /** Wire value the server expects for a status, e.g. "want_to_read". */
  static const char* readStatusKey(ReadStatus status);
  /** Status parsed back from a server string; defaults to Unread. */
  static ReadStatus readStatusFromKey(const std::string& key);

  /**
   * Keep the HTTP/TLS session open across requests. Browsing is a burst of
   * small GETs against one host, so amortizing the handshake is the difference
   * between one 12-16 KB contiguous allocation and one per screen.
   */
  static void beginPersistentSession();
  static void endPersistentSession();

  static const char* errorString(Error error);

  /** HTTP status of the last request, for diagnostics. */
  static int lastHttpCode;
  /** Free heap captured when the last request failed. */
  static unsigned lastHeapAtFailure;
  /** Largest contiguous free block captured when the last request failed. */
  static unsigned lastContigHeapAtFailure;
};
