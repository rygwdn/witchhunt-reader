#pragma once

#include <string>

#include <BookOrbitBookmarkTypes.h>
#include <BookOrbitBookStateTypes.h>

class BookOrbitCatalogClient {
 public:
  enum Error { OK = 0, NETWORK_ERROR };
  static void setSkipTlsValidation(bool);
  static void beginPersistentSession();
  static void endPersistentSession();
  static Error acknowledgeBookmarks(const BookOrbitBookmarkAckRequest&, std::string& response);
  static Error exchangeBookmarks(const BookOrbitBookmarkExchangeRequest&, BookOrbitBookmarkExchangeResponse& response);
  static const char* errorString(Error);

  /** HTTP status of the last request, read by the sync worker for diagnostics. */
  static int lastHttpCode;
};
