
#include "BookOrbitCatalogClient.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <CrossPointRoots.h>
#include <HalClock.h>
#include <HeapPools.h>
#include <KOReaderCredentialStore.h>
#include <KOReaderSyncClient.h>
#include <Logging.h>
#include <Memory.h>
#include <SecureHttpClient.h>
#include <StreamingJsonParser.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>

#include "../../src/network/FirmwareBoardTag.h"

namespace {
// Mirrors the app's "skip HTTPS validation" setting; see setSkipTlsValidation().
bool g_skipTlsValidation = false;

bool g_keepSessionOpen = false;
// Persistent client for a browsing session. Its keep-alive reuses the TLS
// connection across the listing -> detail -> status-write burst a single screen
// produces, so the handshake's contiguous allocation is paid once.
std::unique_ptr<crosspoint::SecureHttpClient> g_sessionHttp;

// Listings are answered in one shot and parsed as they stream, so the timeout
// only has to cover a server round trip, not a transfer.
constexpr uint32_t REQUEST_TIMEOUT_MS = 8000;

// Page size requested for book listings. The server allows up to 100; 12 is
// what one screen shows, and a page is the unit that has to survive being
// parsed on a fragmented heap. Larger pages buy nothing: rows are spooled to SD
// and the next page is one keep-alive GET away.
constexpr int LISTING_PAGE_SIZE = 12;
constexpr size_t MAX_PAGE_STATS_EVENTS_PER_REQUEST = 32;

// Capability the server advertises when it serves the catalog API.
constexpr char CATALOG_CAPABILITY[] = "catalogDashboardSections";

void logHeapSnapshot() {
  BookOrbitCatalogClient::lastHeapAtFailure = ESP.getFreeHeap();
  BookOrbitCatalogClient::lastContigHeapAtFailure = internalContigBytes();
}

// Refuse before a doomed handshake. Same threshold and reasoning as
// KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS — one number, one derivation.
bool heapAllowsTls() {
  logHeapSnapshot();
  const bool reusableSession = g_keepSessionOpen && g_sessionHttp != nullptr;
  if (reusableSession) {
    return true;  // no handshake to pay for on a live keep-alive connection
  }
  if (BookOrbitCatalogClient::lastContigHeapAtFailure < KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS) {
    LOG_ERR("BOCat", "Insufficient contiguous heap for TLS: %u available, %u required",
            BookOrbitCatalogClient::lastContigHeapAtFailure, KOReaderSyncClient::MIN_CONTIG_HEAP_FOR_TLS);
    return false;
  }
  return true;
}

// Scheme+host+port of the configured sync server. Catalog responses carry
// server-absolute paths, which resolve against this and not against the base
// URL's path (which already ends in /api/v1/koreader).
std::string baseOrigin() {
  const std::string base = KOREADER_STORE.getBaseUrl();
  const size_t schemeEnd = base.find("://");
  if (schemeEnd == std::string::npos) return base;
  const size_t pathStart = base.find('/', schemeEnd + 3);
  return pathStart == std::string::npos ? base : base.substr(0, pathStart);
}

std::string catalogUrl(const std::string& suffix) { return KOREADER_STORE.getBaseUrl() + "/plugin/catalog" + suffix; }

void configureClient(crosspoint::SecureHttpClient& http) {
  // Fail closed unless the user disabled validation: these requests carry the
  // sync account's credentials in headers, so an unverifiable peer would be
  // handed them. No roots => SecureClient skips verification, which is what a
  // self-hosted server with a private CA needs.
  http.setCACert(g_skipTlsValidation ? nullptr : CROSSPOINT_ROOTS_PEM);
  http.setAllowInsecureFallback(false);
  // Same last-resort clock guard as the sync client: with the clock at 1970 the
  // trust store refuses to load, long before a certificate is seen. Only the
  // validity window is waived; chain, signature and hostname stay enforced.
  http.setAllowCertificateDateErrors(!HalClock::isPlausibleForTls());
  http.setTimeout(REQUEST_TIMEOUT_MS);
  http.setUserAgent("WitchReader-ESP32-" CROSSPOINT_VERSION);
  http.clearHeaders();
  http.addHeader("Accept", "application/json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password());
}

crosspoint::SecureHttpClient& clientFor(crosspoint::SecureHttpClient& local) {
  if (!g_keepSessionOpen) return local;
  if (!g_sessionHttp) {
    g_sessionHttp = makeUniqueNoThrow<crosspoint::SecureHttpClient>();
    if (!g_sessionHttp) {
      // Keep browsing functional without session reuse; the local client stays
      // on this request's stack and pays one extra handshake.
      LOG_ERR("BOCat", "OOM: persistent HTTP client; using one-shot request");
      return local;
    }
  }
  return *g_sessionHttp;
}

BookOrbitCatalogClient::Error mapHttpStatus(int status) {
  if (status >= 200 && status < 300) return BookOrbitCatalogClient::OK;
  if (status == 401 || status == 403) return BookOrbitCatalogClient::AUTH_FAILED;
  if (status == 404) return BookOrbitCatalogClient::NOT_FOUND;
  if (status >= 300 && status < 400) return BookOrbitCatalogClient::SERVER_ERROR;
  return BookOrbitCatalogClient::SERVER_ERROR;
}

bool hasCredentials() {
  if (KOREADER_STORE.hasCredentials()) return true;
  LOG_INF("BOCat", "No KOReader credentials configured");
  return false;
}

std::string urlEncode(const std::string& in) {
  std::string out;
  out.reserve(in.size() * 3);
  for (const unsigned char c : in) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", c);
      out += buf;
    }
  }
  return out;
}

// Buffered request for the small JSON bodies (version probe, detail, writes).
// Returns the HTTP status, or a negative SecureHttpClient error.
int performRequest(const char* method, const std::string& url, const std::string& body, std::string& outBody) {
  crosspoint::SecureHttpClient local;
  crosspoint::SecureHttpClient& http = clientFor(local);
  configureClient(http);
  if (!body.empty()) http.addHeader("Content-Type", "application/json");

  const uint32_t startMs = millis();
  const int rc = http.request(method, url, body);
  outBody = rc >= 100 ? http.getBody() : std::string{};
  LOG_DBG("BOCat", "%s -> %d in %lu ms (len=%u)", method, rc, static_cast<unsigned long>(millis() - startMs),
          static_cast<unsigned>(outBody.size()));
  return rc;
}
int performStreamingRequest(const char* method, const std::string& url, const std::string& body,
                            const crosspoint::SecureHttpClient::BodySink& sink) {
  crosspoint::SecureHttpClient local;
  crosspoint::SecureHttpClient& http = clientFor(local);
  configureClient(http);
  if (!body.empty()) http.addHeader("Content-Type", "application/json");
  const uint32_t startMs = millis();
  const int rc = http.request(method, url, body, sink);
  LOG_DBG("BOCat", "%s -> %d in %lu ms (streamed)", method, rc, static_cast<unsigned long>(millis() - startMs));
  return rc;
}
constexpr size_t MAX_BOOKMARK_RESPONSE_ITEMS = 8;
constexpr size_t MAX_BOOKMARK_POSITION_BYTES = 4000;
// The server's MaxLength(500) is character-based; a Unicode title can occupy up to 4 UTF-8 bytes per character.
constexpr size_t MAX_BOOKMARK_TITLE_BYTES = 500 * 4;
constexpr size_t MAX_BOOKMARK_SERVER_IDS_PER_BOOK = 600;
bool withinBookmarkTitleLimit(const std::string_view title) {
  if (title.size() > MAX_BOOKMARK_TITLE_BYTES) return false;
  size_t characters = 0;
  for (const unsigned char byte : title) {
    if ((byte & 0xC0) != 0x80 && ++characters > 500) return false;
  }
  return true;
}

bool validBookmarkHash(const std::string_view hash) {
  if (hash.size() != 32) return false;
  for (const unsigned char c : hash) {
    if (!std::isxdigit(c)) return false;
  }
  return true;
}
bool validDeviceDatetime(const std::string_view value) {
  if (value.size() != 19) return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 4 || i == 7) {
      if (value[i] != '-') return false;
    } else if (i == 10) {
      if (value[i] != ' ') return false;
    } else if (i == 13 || i == 16) {
      if (value[i] != ':') return false;
    } else if (value[i] < '0' || value[i] > '9') {
      return false;
    }
  }
  return true;
}
bool sameBookmarkHash(const std::string_view left, const std::string_view right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

bool validMd5(const std::string& hash) {
  if (hash.size() != 32) return false;
  for (const unsigned char digit : hash) {
    if (!std::isxdigit(digit)) return false;
  }
  return true;
}
bool validDateOnly(const std::string& day) {
  if (day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
  for (size_t i = 0; i < day.size(); ++i) {
    if (i != 4 && i != 7 && (day[i] < '0' || day[i] > '9')) return false;
  }
  return true;
}

std::string lowerMd5(const std::string& hash) {
  std::string normalized = hash;
  for (char& digit : normalized) digit = static_cast<char>(std::tolower(static_cast<unsigned char>(digit)));
  return normalized;
}

bool addDeviceMetadata(JsonDocument& doc) {
  const std::string deviceId = KOReaderSyncClient::deviceId();
  if (deviceId.empty() || strlen(CROSSPOINT_VERSION) > 20) return false;
  char deviceModel[40] = "CrossPoint ";
  const size_t prefixLen = sizeof("CrossPoint ") - 1;
  const size_t boardNameLen = board_tag::boardNameLen();
  if (boardNameLen > sizeof(deviceModel) - prefixLen - 1) return false;
  memcpy(deviceModel + prefixLen, board_tag::boardName(), boardNameLen);
  deviceModel[prefixLen + boardNameLen] = '\0';
  doc["deviceId"] = deviceId;
  doc["deviceModel"] = deviceModel;
  doc["pluginVersion"] = CROSSPOINT_VERSION;
  if (HalClock::isSynced()) {
    const time_t now = HalClock::now();
    struct tm local{};
    if (localtime_r(&now, &local)) {
      char deviceTime[20];
      if (strftime(deviceTime, sizeof(deviceTime), "%Y-%m-%d %H:%M:%S", &local) > 0) {
        doc["deviceTime"] = deviceTime;
      }
    }
  }
  return true;
}

class BookmarkExchangeParser {
 public:
  BookmarkExchangeParser(const BookOrbitBookmarkExchangeRequest& request, BookOrbitBookmarkExchangeResponse& response)
      : request_(request),
        response_(response),
        parser_(JsonCallbacks{.ctx = this,
                              .onKey = onKey,
                              .onString = onString,
                              .onNumber = onNumber,
                              .onBool = onBool,
                              .onNull = onNull,
                              .onObjectStart = onObjectStart,
                              .onObjectEnd = onObjectEnd,
                              .onArrayStart = onArrayStart,
                              .onArrayEnd = onArrayEnd,
                              .onStringPart = onStringPart}) {
    response_ = {};
    response_.results.reserve(request.books.size());
    response_.unmatched.reserve(request.books.size());
  }

  void feed(const char* data, const size_t length) { parser_.feed(data, length); }
  bool hasError() const { return error_ || parser_.hasError(); }

  bool complete() const {
    if (hasError() || !rootSeen_ || !rootClosed_ || !resultsSeen_ || !resultsClosed_ || !unmatchedSeen_ ||
        !unmatchedClosed_ || response_.results.size() + response_.unmatched.size() != request_.books.size()) {
      return false;
    }
    for (size_t i = 0; i < request_.books.size(); ++i) {
      if (resultSeen_[i] == unmatchedSeenByRequest_[i]) return false;
    }
    return true;
  }

 private:
  enum class Context : uint8_t {
    Root,
    Results,
    Result,
    ToApply,
    Adds,
    Add,
    Deletes,
    Delete,
    Unmatched,
    Ignore,
  };
  enum class Key : uint8_t {
    None,
    Results,
    Unmatched,
    Hash,
    More,
    ToApply,
    Add,
    Delete,
    ServerId,
    Position,
    Page,
    Title,
    Key,
    Datetime,
    Other,
  };

  static void onKey(void* ctx, const char* value, size_t length) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    self.key_ = Key::Other;
    if (length == 7 && memcmp(value, "results", 7) == 0 && self.top() == Context::Root) {
      self.key_ = Key::Results;
    } else if (length == 9 && memcmp(value, "unmatched", 9) == 0 && self.top() == Context::Root) {
      self.key_ = Key::Unmatched;
    } else if (length == 4 && memcmp(value, "hash", 4) == 0 && self.top() == Context::Result) {
      self.key_ = Key::Hash;
    } else if (length == 4 && memcmp(value, "more", 4) == 0 && self.top() == Context::Result) {
      self.key_ = Key::More;
    } else if (length == 7 && memcmp(value, "toApply", 7) == 0 && self.top() == Context::Result) {
      self.key_ = Key::ToApply;
    } else if (length == 3 && memcmp(value, "add", 3) == 0 && self.top() == Context::ToApply) {
      self.key_ = Key::Add;
    } else if (length == 6 && memcmp(value, "delete", 6) == 0 && self.top() == Context::ToApply) {
      self.key_ = Key::Delete;
    } else if (length == 8 && memcmp(value, "serverId", 8) == 0 &&
               (self.top() == Context::Add || self.top() == Context::Delete)) {
      self.key_ = Key::ServerId;
    } else if (length == 3 && memcmp(value, "pos", 3) == 0 && self.top() == Context::Add) {
      self.key_ = Key::Position;
    } else if (length == 6 && memcmp(value, "pageno", 6) == 0 && self.top() == Context::Add) {
      self.key_ = Key::Page;
    } else if (length == 5 && memcmp(value, "title", 5) == 0 && self.top() == Context::Add) {
      self.key_ = Key::Title;
    } else if (length == 3 && memcmp(value, "key", 3) == 0 && self.top() == Context::Delete) {
      self.key_ = Key::Key;
    } else if (length == 8 && memcmp(value, "datetime", 8) == 0 && self.top() == Context::Delete) {
      self.key_ = Key::Datetime;
    }
  }

  static void onString(void* ctx, const char* value, size_t length) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    const Key key = self.key_;
    self.key_ = Key::None;
    if (self.top() == Context::Unmatched) {
      self.addUnmatched(value, length);
      return;
    }
    if (self.top() == Context::Results || self.top() == Context::Adds || self.top() == Context::Deletes) {
      self.error_ = true;
      return;
    }
    if (self.top() == Context::Result && key == Key::Hash) {
      if (self.resultHashSeen_ || length != 32) {
        self.error_ = true;
        return;
      }
      self.currentResult_.hash.assign(value, length);
      self.resultHashSeen_ = true;
    } else if (self.top() == Context::Add && key == Key::Position) {
      if (self.addPositionSeen_) {
        self.error_ = true;
        return;
      }
      self.appendPosition(value, length);
    } else if (self.top() == Context::Add && key == Key::Title) {
      if (self.addTitleSeen_ || length > MAX_BOOKMARK_TITLE_BYTES) {
        self.error_ = true;
        return;
      }
      self.addTitleLength_ = length;
      if (self.storeCurrentRecord()) self.currentAdd_.title.assign(value, length);
      self.addTitleSeen_ = true;
    } else if (self.top() == Context::Delete && key == Key::Key) {
      if (self.deleteKeySeen_ || !validBookmarkHash(std::string_view(value, length))) {
        self.error_ = true;
        return;
      }
      self.currentDelete_.key.assign(value, length);
      self.deleteKeySeen_ = true;
    } else if (self.top() == Context::Delete && key == Key::Datetime) {
      if (self.deleteDatetimeSeen_ || !validDeviceDatetime(std::string_view(value, length))) {
        self.error_ = true;
        return;
      }
      self.currentDelete_.datetime.assign(value, length);
      self.deleteDatetimeSeen_ = true;
    } else if ((self.top() == Context::Result && (key == Key::More || key == Key::ToApply)) ||
               (self.top() == Context::Add && (key == Key::Page || key == Key::ServerId)) ||
               (self.top() == Context::Delete && key == Key::ServerId)) {
      self.error_ = true;
    }
  }

  static void onStringPart(void* ctx, const char* value, size_t length, const bool first, const bool last) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    if (self.top() == Context::Add && self.key_ == Key::Position) {
      if (first) {
        self.addPositionLength_ = 0;
        if (self.addPositionSeen_) {
          self.error_ = true;
          return;
        }
        if (self.storeCurrentRecord()) self.currentAdd_.position.clear();
      }
      self.appendPosition(value, length);
      return;
    }
    if (self.top() == Context::Add && self.key_ == Key::Title) {
      if (first) {
        if (self.addTitleSeen_) {
          self.error_ = true;
          return;
        }
        self.addTitleLength_ = 0;
        if (self.storeCurrentRecord()) {
          self.currentAdd_.title.clear();
          self.currentAdd_.title.reserve(MAX_BOOKMARK_TITLE_BYTES);
        }
      }
      if (length > MAX_BOOKMARK_TITLE_BYTES - self.addTitleLength_) {
        self.error_ = true;
        return;
      }
      self.addTitleLength_ += length;
      if (self.storeCurrentRecord()) self.currentAdd_.title.append(value, length);
      if (last) {
        self.addTitleSeen_ = true;
        self.key_ = Key::None;
      }
      return;
    }
    self.error_ = true;
  }

  static void onNumber(void* ctx, const char* value, size_t length) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    const bool integer = end == value + length && end != value && parsed >= 0 && parsed <= INT32_MAX;
    const Key key = self.key_;
    self.key_ = Key::None;
    if (self.top() == Context::Add && key == Key::ServerId) {
      if (!integer || parsed == 0 || self.addIdSeen_) {
        self.error_ = true;
        return;
      }
      self.currentAdd_.serverId = static_cast<int32_t>(parsed);
      self.addIdSeen_ = true;
    } else if (self.top() == Context::Add && key == Key::Page) {
      if (!integer || self.addPageSeen_) {
        self.error_ = true;
        return;
      }
      self.currentAdd_.page = static_cast<int32_t>(parsed);
      self.addPageSeen_ = true;
    } else if (self.top() == Context::Delete && key == Key::ServerId) {
      if (!integer || parsed == 0 || self.deleteIdSeen_) {
        self.error_ = true;
        return;
      }
      self.currentDelete_.serverId = static_cast<int32_t>(parsed);
      self.deleteIdSeen_ = true;
    } else if ((self.top() == Context::Result && key != Key::Other && key != Key::None) ||
               (self.top() == Context::Add && (key == Key::Position || key == Key::Title)) ||
               (self.top() == Context::Delete && (key == Key::Key || key == Key::Datetime)) ||
               self.top() == Context::Results || self.top() == Context::Adds || self.top() == Context::Deletes ||
               self.top() == Context::Unmatched) {
      self.error_ = true;
    }
  }

  static void onBool(void* ctx, const bool value) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    if (self.top() == Context::Result && self.key_ == Key::More) {
      if (self.moreSeen_) self.error_ = true;
      self.currentResult_.more = value;
      self.moreSeen_ = true;
      self.key_ = Key::None;
      return;
    }
    if ((self.top() == Context::Result && (self.key_ == Key::Hash || self.key_ == Key::ToApply)) ||
        (self.top() == Context::Add && self.key_ != Key::Other && self.key_ != Key::None) ||
        (self.top() == Context::Delete && self.key_ != Key::Other && self.key_ != Key::None) ||
        self.top() == Context::Results || self.top() == Context::Adds || self.top() == Context::Deletes ||
        self.top() == Context::Unmatched) {
      self.error_ = true;
    }
    self.key_ = Key::None;
  }

  static void onNull(void* ctx) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    if (self.top() == Context::Add && self.key_ == Key::Page && !self.addPageSeen_) {
      self.currentAdd_.page = -1;
      self.addPageSeen_ = true;
    } else if (self.top() == Context::Delete && self.key_ == Key::Datetime && !self.deleteDatetimeSeen_) {
      self.currentDelete_.datetime.clear();
      self.deleteDatetimeSeen_ = true;
    } else if ((self.top() == Context::Result && self.key_ != Key::Other) ||
               (self.top() == Context::Add && self.key_ != Key::Other) ||
               (self.top() == Context::Delete && self.key_ != Key::Other) || self.top() == Context::Results ||
               self.top() == Context::Adds || self.top() == Context::Deletes || self.top() == Context::Unmatched) {
      self.error_ = true;
    }
    self.key_ = Key::None;
  }

  static void onObjectStart(void* ctx) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    const Context parent = self.top();
    if (self.depth_ == 0) {
      if (self.rootSeen_) {
        self.error_ = true;
        return;
      }
      self.rootSeen_ = true;
      self.push(Context::Root);
    } else if (parent == Context::Results) {
      self.beginResult();
      self.push(Context::Result);
    } else if (parent == Context::Result && self.key_ == Key::ToApply) {
      if (self.toApplySeen_) self.error_ = true;
      self.toApplySeen_ = true;
      self.key_ = Key::None;
      self.push(Context::ToApply);
    } else if (parent == Context::Adds) {
      self.beginAdd();
      self.push(Context::Add);
    } else if (parent == Context::Deletes) {
      self.beginDelete();
      self.push(Context::Delete);
    } else {
      if (parent == Context::Unmatched || (parent == Context::ToApply && self.key_ != Key::Other) ||
          (parent == Context::Root && (self.key_ == Key::Results || self.key_ == Key::Unmatched))) {
        self.error_ = true;
      }
      self.key_ = Key::None;
      self.push(Context::Ignore);
    }
  }

  static void onObjectEnd(void* ctx) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    if (self.depth_ == 0) {
      self.error_ = true;
      return;
    }
    const Context context = self.top();
    if (context == Context::Add) {
      self.finishAdd();
    } else if (context == Context::Delete) {
      self.finishDelete();
    } else if (context == Context::Result) {
      self.finishResult();
    } else if (context == Context::Root) {
      self.rootClosed_ = true;
    }
    self.pop();
  }

  static void onArrayStart(void* ctx) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    const Context parent = self.top();
    if (parent == Context::Root && self.key_ == Key::Results) {
      if (self.resultsSeen_) self.error_ = true;
      self.resultsSeen_ = true;
      self.key_ = Key::None;
      self.push(Context::Results);
    } else if (parent == Context::Root && self.key_ == Key::Unmatched) {
      if (self.unmatchedSeen_) self.error_ = true;
      self.unmatchedSeen_ = true;
      self.key_ = Key::None;
      self.push(Context::Unmatched);
    } else if (parent == Context::ToApply && self.key_ == Key::Add) {
      if (self.addsSeen_) self.error_ = true;
      self.addsSeen_ = true;
      self.key_ = Key::None;
      self.push(Context::Adds);
    } else if (parent == Context::ToApply && self.key_ == Key::Delete) {
      if (self.deletesSeen_) self.error_ = true;
      self.deletesSeen_ = true;
      self.key_ = Key::None;
      self.push(Context::Deletes);
    } else {
      if (parent == Context::Results || parent == Context::Unmatched || parent == Context::Adds ||
          parent == Context::Deletes ||
          (parent == Context::Root && (self.key_ == Key::Results || self.key_ == Key::Unmatched)) ||
          (parent == Context::Result && (self.key_ == Key::More || self.key_ == Key::ToApply)) ||
          (parent == Context::ToApply && (self.key_ == Key::Add || self.key_ == Key::Delete))) {
        self.error_ = true;
      }
      self.key_ = Key::None;
      self.push(Context::Ignore);
    }
  }

  static void onArrayEnd(void* ctx) {
    auto& self = *static_cast<BookmarkExchangeParser*>(ctx);
    if (self.depth_ == 0) {
      self.error_ = true;
      return;
    }
    switch (self.top()) {
      case Context::Results:
        self.resultsClosed_ = true;
        break;
      case Context::Unmatched:
        self.unmatchedClosed_ = true;
        break;
      case Context::Adds:
        self.addsClosed_ = true;
        break;
      case Context::Deletes:
        self.deletesClosed_ = true;
        break;
      default:
        break;
    }
    self.pop();
  }

  Context top() const { return depth_ == 0 ? Context::Ignore : stack_[depth_ - 1]; }
  void push(const Context context) {
    if (depth_ == stack_.size()) {
      error_ = true;
      return;
    }
    stack_[depth_++] = context;
  }
  void pop() {
    if (depth_ > 0) --depth_;
  }

  int requestIndex(const std::string_view hash) const {
    for (size_t i = 0; i < request_.books.size(); ++i) {
      if (sameBookmarkHash(request_.books[i].hash, hash)) return static_cast<int>(i);
    }
    return -1;
  }

  void beginResult() {
    currentResult_ = {};
    resultHashSeen_ = false;
    moreSeen_ = false;
    toApplySeen_ = false;
    addsSeen_ = deletesSeen_ = addsClosed_ = deletesClosed_ = false;
    currentResultTruncated_ = false;
    serverIdCount_ = 0;
  }
  void beginAdd() {
    currentAdd_ = {};
    addIdSeen_ = addPositionSeen_ = addTitleSeen_ = addPageSeen_ = false;
    addPositionLength_ = 0;
    addTitleLength_ = 0;
  }
  void beginDelete() {
    currentDelete_ = {};
    deleteIdSeen_ = deleteKeySeen_ = deleteDatetimeSeen_ = false;
  }

  void appendPosition(const char* data, const size_t length) {
    if (!addPositionSeen_) {
      addPositionSeen_ = true;
      currentAdd_.position.clear();
    }
    if (length > MAX_BOOKMARK_POSITION_BYTES - addPositionLength_) {
      error_ = true;
      return;
    }
    addPositionLength_ += length;
    if (storeCurrentRecord()) currentAdd_.position.append(data, length);
  }

  bool storeCurrentRecord() const { return storedRecords_ < MAX_BOOKMARK_RESPONSE_ITEMS; }

  bool uniqueServerId(const int32_t serverId) {
    if (serverIdCount_ >= serverIds_.size()) return false;
    if (std::find(serverIds_.begin(), serverIds_.begin() + serverIdCount_, serverId) !=
        serverIds_.begin() + serverIdCount_) {
      return false;
    }
    serverIds_[serverIdCount_++] = serverId;
    return true;
  }

  void finishAdd() {
    if (!addIdSeen_ || !addPositionSeen_ || addPositionLength_ == 0 || !addTitleSeen_ || !addPageSeen_ ||
        !uniqueServerId(currentAdd_.serverId)) {
      error_ = true;
      return;
    }
    if (storeCurrentRecord()) {
      currentResult_.add.push_back(std::move(currentAdd_));
      ++storedRecords_;
    } else {
      currentResultTruncated_ = true;
    }
  }

  void finishDelete() {
    if (!deleteIdSeen_ || !deleteKeySeen_ || !deleteDatetimeSeen_ || !uniqueServerId(currentDelete_.serverId)) {
      error_ = true;
      return;
    }
    if (storeCurrentRecord()) {
      currentResult_.remove.push_back(std::move(currentDelete_));
      ++storedRecords_;
    } else {
      currentResultTruncated_ = true;
    }
  }

  void finishResult() {
    if (!resultHashSeen_ || !moreSeen_ || !toApplySeen_ || !addsSeen_ || !deletesSeen_ || !addsClosed_ ||
        !deletesClosed_ || !validBookmarkHash(currentResult_.hash)) {
      error_ = true;
      return;
    }
    const int index = requestIndex(currentResult_.hash);
    if (index < 0 || resultSeen_[index]) {
      error_ = true;
      return;
    }
    resultSeen_[index] = true;
    currentResult_.hash = request_.books[static_cast<size_t>(index)].hash;
    currentResult_.more = currentResult_.more || currentResultTruncated_;
    response_.results.push_back(std::move(currentResult_));
  }

  void addUnmatched(const char* data, const size_t length) {
    if (length != 32 || !validBookmarkHash(std::string_view(data, length))) {
      error_ = true;
      return;
    }
    const int index = requestIndex(std::string_view(data, length));
    if (index < 0 || unmatchedSeenByRequest_[index]) {
      error_ = true;
      return;
    }
    unmatchedSeenByRequest_[index] = true;
    response_.unmatched.push_back(request_.books[static_cast<size_t>(index)].hash);
  }

  const BookOrbitBookmarkExchangeRequest& request_;
  BookOrbitBookmarkExchangeResponse& response_;
  StreamingJsonParser parser_;
  std::array<Context, 8> stack_{};
  std::array<bool, 20> resultSeen_{};
  std::array<bool, 20> unmatchedSeenByRequest_{};
  std::array<int32_t, MAX_BOOKMARK_SERVER_IDS_PER_BOOK> serverIds_{};
  BookOrbitBookmarkExchangeResult currentResult_;
  BookOrbitBookmarkAdd currentAdd_;
  BookOrbitBookmarkDelete currentDelete_;
  size_t depth_ = 0;
  size_t storedRecords_ = 0;
  size_t addPositionLength_ = 0;
  size_t addTitleLength_ = 0;
  size_t serverIdCount_ = 0;
  Key key_ = Key::None;
  bool error_ = false;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  bool resultsSeen_ = false;
  bool resultsClosed_ = false;
  bool unmatchedSeen_ = false;
  bool unmatchedClosed_ = false;
  bool resultHashSeen_ = false;
  bool moreSeen_ = false;
  bool toApplySeen_ = false;
  bool addsSeen_ = false;
  bool deletesSeen_ = false;
  bool addsClosed_ = false;
  bool deletesClosed_ = false;
  bool addIdSeen_ = false;
  bool addPositionSeen_ = false;
  bool addTitleSeen_ = false;
  bool addPageSeen_ = false;
  bool deleteIdSeen_ = false;
  bool deleteKeySeen_ = false;
  bool deleteDatetimeSeen_ = false;
  bool currentResultTruncated_ = false;
};
class BookmarkAckResponseParser {
 public:
  explicit BookmarkAckResponseParser(const BookOrbitBookmarkAckRequest& request)
      : request_(request),
        parser_(JsonCallbacks{.ctx = this,
                              .onKey = onKey,
                              .onString = onString,
                              .onNumber = onNumber,
                              .onBool = onBool,
                              .onNull = onNull,
                              .onObjectStart = onObjectStart,
                              .onObjectEnd = onObjectEnd,
                              .onArrayStart = onArrayStart,
                              .onArrayEnd = onArrayEnd}) {}

  void feed(const char* data, const size_t length) { parser_.feed(data, length); }
  bool hasError() const { return error_ || parser_.hasError(); }
  bool complete() const {
    if (hasError() || unmatchedCount_ != 0 || !rootSeen_ || !rootClosed_ || !resultsSeen_ || !resultsClosed_ ||
        !unmatchedSeen_ || !unmatchedClosed_ || resultCount_ + unmatchedCount_ != request_.books.size()) {
      return false;
    }
    for (size_t i = 0; i < request_.books.size(); ++i) {
      if (resultSeen_[i] == unmatchedSeenByRequest_[i]) return false;
    }
    return true;
  }

 private:
  enum class Context : uint8_t { Root, Results, Result, Unmatched, Ignore };
  enum class Key : uint8_t { None, Results, Unmatched, Hash, Acked, Other };

  static void onKey(void* ctx, const char* value, const size_t length) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    self.key_ = Key::Other;
    if (self.top() == Context::Root && length == 7 && memcmp(value, "results", 7) == 0) {
      self.key_ = Key::Results;
    } else if (self.top() == Context::Root && length == 9 && memcmp(value, "unmatched", 9) == 0) {
      self.key_ = Key::Unmatched;
    } else if (self.top() == Context::Result && length == 4 && memcmp(value, "hash", 4) == 0) {
      self.key_ = Key::Hash;
    } else if (self.top() == Context::Result && length == 5 && memcmp(value, "acked", 5) == 0) {
      self.key_ = Key::Acked;
    }
  }

  static void onString(void* ctx, const char* value, const size_t length) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    const Key key = self.key_;
    self.key_ = Key::None;
    if (self.top() == Context::Unmatched) {
      self.addUnmatched(value, length);
    } else if (self.top() == Context::Result && key == Key::Hash) {
      if (self.hashSeen_ || length != 32) {
        self.error_ = true;
        return;
      }
      self.hash_.assign(value, length);
      self.hashSeen_ = true;
    } else if (self.top() == Context::Result && key == Key::Acked) {
      self.error_ = true;
    } else if (self.top() == Context::Results || self.top() == Context::Unmatched) {
      self.error_ = true;
    }
  }

  static void onNumber(void* ctx, const char* value, const size_t length) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    const Key key = self.key_;
    self.key_ = Key::None;
    if (self.top() != Context::Result || key != Key::Acked) {
      if (self.top() == Context::Results || self.top() == Context::Unmatched ||
          (self.top() == Context::Result && key == Key::Hash)) {
        self.error_ = true;
      }
      return;
    }
    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    if (self.ackedSeen_ || end != value + length || end == value || parsed < 0 || parsed > 200) {
      self.error_ = true;
      return;
    }
    self.acked_ = static_cast<size_t>(parsed);
    self.ackedSeen_ = true;
  }

  static void onBool(void* ctx, const bool) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    if (self.top() == Context::Result && (self.key_ == Key::Hash || self.key_ == Key::Acked)) {
      self.error_ = true;
    } else if (self.top() == Context::Results || self.top() == Context::Unmatched) {
      self.error_ = true;
    }
    self.key_ = Key::None;
  }

  static void onNull(void* ctx) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    if (self.top() == Context::Result && (self.key_ == Key::Hash || self.key_ == Key::Acked)) {
      self.error_ = true;
    } else if (self.top() == Context::Results || self.top() == Context::Unmatched) {
      self.error_ = true;
    }
    self.key_ = Key::None;
  }

  static void onObjectStart(void* ctx) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    const Context parent = self.top();
    if (self.depth_ == 0) {
      if (self.rootSeen_) self.error_ = true;
      self.rootSeen_ = true;
      self.push(Context::Root);
    } else if (parent == Context::Results) {
      self.hash_.clear();
      self.hashSeen_ = self.ackedSeen_ = false;
      self.acked_ = 0;
      self.push(Context::Result);
    } else {
      if (parent == Context::Unmatched ||
          (parent == Context::Root && (self.key_ == Key::Results || self.key_ == Key::Unmatched))) {
        self.error_ = true;
      }
      self.key_ = Key::None;
      self.push(Context::Ignore);
    }
  }

  static void onObjectEnd(void* ctx) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    if (self.depth_ == 0) {
      self.error_ = true;
      return;
    }
    const Context context = self.top();
    if (context == Context::Result) self.finishResult();
    if (context == Context::Root) self.rootClosed_ = true;
    self.pop();
  }

  static void onArrayStart(void* ctx) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    const Context parent = self.top();
    if (parent == Context::Root && self.key_ == Key::Results) {
      if (self.resultsSeen_) self.error_ = true;
      self.resultsSeen_ = true;
      self.key_ = Key::None;
      self.push(Context::Results);
    } else if (parent == Context::Root && self.key_ == Key::Unmatched) {
      if (self.unmatchedSeen_) self.error_ = true;
      self.unmatchedSeen_ = true;
      self.key_ = Key::None;
      self.push(Context::Unmatched);
    } else {
      if (parent == Context::Results || parent == Context::Unmatched ||
          (parent == Context::Root && (self.key_ == Key::Results || self.key_ == Key::Unmatched)) ||
          (parent == Context::Result && (self.key_ == Key::Hash || self.key_ == Key::Acked))) {
        self.error_ = true;
      }
      self.key_ = Key::None;
      self.push(Context::Ignore);
    }
  }

  static void onArrayEnd(void* ctx) {
    auto& self = *static_cast<BookmarkAckResponseParser*>(ctx);
    if (self.depth_ == 0) {
      self.error_ = true;
      return;
    }
    if (self.top() == Context::Results) self.resultsClosed_ = true;
    if (self.top() == Context::Unmatched) self.unmatchedClosed_ = true;
    self.pop();
  }

  Context top() const { return depth_ == 0 ? Context::Ignore : stack_[depth_ - 1]; }
  void push(const Context context) {
    if (depth_ == stack_.size()) {
      error_ = true;
      return;
    }
    stack_[depth_++] = context;
  }
  void pop() {
    if (depth_ > 0) --depth_;
  }

  int requestIndex(const std::string_view hash) const {
    for (size_t i = 0; i < request_.books.size(); ++i) {
      if (sameBookmarkHash(request_.books[i].hash, hash)) return static_cast<int>(i);
    }
    return -1;
  }

  size_t minimumAckCount(const BookOrbitBookmarkAckBook& book) const {
    size_t count = 0;
    for (const BookOrbitBookmarkAckApplied& item : book.applied) count += item.applied;
    return count;
  }

  size_t maximumAckCount(const BookOrbitBookmarkAckBook& book) const {
    size_t count = minimumAckCount(book);
    for (const BookOrbitBookmarkAckDeleted& item : book.deleted) count += item.applied;
    return count;
  }

  void finishResult() {
    if (!hashSeen_ || !ackedSeen_ || !validBookmarkHash(hash_)) {
      error_ = true;
      return;
    }
    const int index = requestIndex(hash_);
    if (index < 0 || resultSeen_[index] || acked_ < minimumAckCount(request_.books[static_cast<size_t>(index)]) ||
        acked_ > maximumAckCount(request_.books[static_cast<size_t>(index)])) {
      error_ = true;
      return;
    }
    resultSeen_[index] = true;
    ++resultCount_;
  }

  void addUnmatched(const char* data, const size_t length) {
    if (length != 32 || !validBookmarkHash(std::string_view(data, length))) {
      error_ = true;
      return;
    }
    const int index = requestIndex(std::string_view(data, length));
    if (index < 0 || unmatchedSeenByRequest_[index]) {
      error_ = true;
      return;
    }
    unmatchedSeenByRequest_[index] = true;
    ++unmatchedCount_;
  }

  const BookOrbitBookmarkAckRequest& request_;
  StreamingJsonParser parser_;
  std::array<Context, 8> stack_{};
  std::array<bool, 20> resultSeen_{};
  std::array<bool, 20> unmatchedSeenByRequest_{};
  std::string hash_;
  size_t depth_ = 0;
  size_t resultCount_ = 0;
  size_t unmatchedCount_ = 0;
  size_t acked_ = 0;
  Key key_ = Key::None;
  bool error_ = false;
  bool rootSeen_ = false;
  bool rootClosed_ = false;
  bool resultsSeen_ = false;
  bool resultsClosed_ = false;
  bool unmatchedSeen_ = false;
  bool unmatchedClosed_ = false;
  bool hashSeen_ = false;
  bool ackedSeen_ = false;
};
const char* bookStateStatusKey(const BookOrbitBookStateStatus status) {
  switch (status) {
    case BookOrbitBookStateStatus::Reading:
      return "reading";
    case BookOrbitBookStateStatus::Complete:
      return "complete";
    case BookOrbitBookStateStatus::Abandoned:
      return "abandoned";
  }
  return nullptr;
}

struct BookStateResponseContext {
  std::string expectedHash;
  BookOrbitBookStateResult* result = nullptr;
  BookOrbitReviewNoteSink noteSink = nullptr;
  void* noteContext = nullptr;
  char key[32]{};
  bool noteActive = false;
  uint8_t depth = 0;
  uint8_t resultsArrayDepth = 0;
  uint8_t unmatchedArrayDepth = 0;
  uint8_t resultObjectDepth = 0;
  bool resultsArrayOpen = false;
  bool unmatchedArrayOpen = false;
  bool rootSeen = false;
  bool rootClosed = false;
  bool resultsSeen = false;
  bool resultsClosed = false;
  bool unmatchedSeen = false;
  bool unmatchedClosed = false;
  bool resultObjectSeen = false;
  bool resultObjectClosed = false;
  bool unmatchedHashSeen = false;
  bool sawStatusApplied = false;
  bool sawRatingApplied = false;
  bool sawReviewApplied = false;
  bool sawRatingSet = false;
  bool sawRatingValue = false;
  bool sawReviewNoteSet = false;
  bool sawReviewNote = false;
  bool noteWasString = false;
  bool matchedResultSeen = false;
  bool sawRatingUpdatedAt = false;
  bool sawReviewUpdatedAt = false;
  bool invalid = false;

  bool complete() const {
    return !invalid && depth == 0 && rootSeen && rootClosed && resultsSeen && resultsClosed && unmatchedSeen &&
           unmatchedClosed;
  }
};

bool responseKeyIs(const BookStateResponseContext& context, const char* key) { return strcmp(context.key, key) == 0; }

bool inBookStateResult(const BookStateResponseContext& context) {
  return context.resultObjectDepth != 0 && context.depth == context.resultObjectDepth;
}

void onBookStateObjectStart(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.depth == 0) {
    if (context.rootSeen) context.invalid = true;
    context.rootSeen = true;
  } else if (context.resultsArrayOpen && context.depth == context.resultsArrayDepth) {
    if (context.resultObjectSeen) {
      context.invalid = true;
    } else {
      context.resultObjectSeen = true;
      context.resultObjectDepth = context.depth + 1;
    }
  } else if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) {
    context.invalid = true;
  }
  ++context.depth;
}

void onBookStateObjectEnd(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.depth == 0) {
    context.invalid = true;
    return;
  }
  if (context.depth == context.resultObjectDepth) context.resultObjectClosed = true;
  if (context.depth == 1) context.rootClosed = true;
  --context.depth;
}

void onBookStateArrayStart(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) context.invalid = true;
  if (context.depth == 1 && responseKeyIs(context, "results")) {
    if (context.resultsSeen) {
      context.invalid = true;
    } else {
      context.resultsSeen = true;
      context.resultsArrayOpen = true;
      context.resultsArrayDepth = context.depth + 1;
    }
  } else if (context.depth == 1 && responseKeyIs(context, "unmatched")) {
    if (context.unmatchedSeen) {
      context.invalid = true;
    } else {
      context.unmatchedSeen = true;
      context.unmatchedArrayOpen = true;
      context.unmatchedArrayDepth = context.depth + 1;
    }
  }
  ++context.depth;
}

void onBookStateArrayEnd(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.depth == 0) {
    context.invalid = true;
    return;
  }
  if (context.resultsArrayOpen && context.depth == context.resultsArrayDepth) {
    context.resultsClosed = true;
    context.resultsArrayOpen = false;
  }
  if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) {
    context.unmatchedClosed = true;
    context.unmatchedArrayOpen = false;
  }
  --context.depth;
}

void onBookStateKey(void* raw, const char* key, const size_t length) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (length >= sizeof(context.key)) {
    context.key[0] = '\0';
    return;
  }
  memcpy(context.key, key, length);
  context.key[length] = '\0';
}

void onBookStateString(void* raw, const char* value, const size_t length) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) {
    if (context.unmatchedHashSeen || length != context.expectedHash.size() ||
        memcmp(value, context.expectedHash.data(), length) != 0) {
      context.invalid = true;
    } else {
      context.unmatchedHashSeen = true;
      context.result->unmatched = true;
    }
    return;
  }
  if (!inBookStateResult(context)) return;
  BookOrbitBookStateResult& result = *context.result;
  if (responseKeyIs(context, "hash")) {
    result.hash.assign(value, length);
    context.matchedResultSeen = true;
  } else if (responseKeyIs(context, "ratingUpdatedAt")) {
    result.ratingUpdatedAt.assign(value, length);
    context.sawRatingUpdatedAt = true;
  } else if (responseKeyIs(context, "reviewUpdatedAt")) {
    result.reviewUpdatedAt.assign(value, length);
    context.sawReviewUpdatedAt = true;
  }
}

void onBookStateNumber(void* raw, const char* value, const size_t length) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) {
    context.invalid = true;
    return;
  }
  if (!inBookStateResult(context) || !responseKeyIs(context, "rating")) return;
  int rating = 0;
  if (length == 0) {
    context.invalid = true;
    return;
  }
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') {
      context.invalid = true;
      return;
    }
    rating = rating * 10 + value[i] - '0';
    if (rating > 5) {
      context.invalid = true;
      return;
    }
  }
  if (rating < 1) {
    context.invalid = true;
    return;
  }
  context.result->rating = static_cast<int8_t>(rating);
  context.sawRatingValue = true;
}

void onBookStateBool(void* raw, const bool value) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) {
    context.invalid = true;
    return;
  }
  if (!inBookStateResult(context)) return;
  BookOrbitBookStateResult& result = *context.result;
  if (responseKeyIs(context, "statusApplied")) {
    result.statusApplied = value;
    context.sawStatusApplied = true;
  } else if (responseKeyIs(context, "ratingApplied")) {
    result.ratingApplied = value;
    context.sawRatingApplied = true;
  } else if (responseKeyIs(context, "reviewApplied")) {
    result.reviewApplied = value;
    context.sawReviewApplied = true;
  } else if (responseKeyIs(context, "ratingSet")) {
    result.ratingSet = value;
    context.sawRatingSet = true;
  } else if (responseKeyIs(context, "reviewNoteSet")) {
    result.reviewNoteSet = value;
    context.sawReviewNoteSet = true;
  }
}

void onBookStateNull(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (context.unmatchedArrayOpen && context.depth == context.unmatchedArrayDepth) {
    context.invalid = true;
    return;
  }
  if (!inBookStateResult(context)) return;
  BookOrbitBookStateResult& result = *context.result;
  if (responseKeyIs(context, "rating")) {
    result.rating = -1;
    result.ratingSet = false;
    context.sawRatingValue = true;
  } else if (responseKeyIs(context, "ratingUpdatedAt")) {
    result.ratingUpdatedAt.clear();
    context.sawRatingUpdatedAt = true;
  } else if (responseKeyIs(context, "reviewUpdatedAt")) {
    result.reviewUpdatedAt.clear();
    context.sawReviewUpdatedAt = true;
  } else if (responseKeyIs(context, "reviewNote")) {
    result.reviewNotePresent = true;
    result.reviewNoteSet = false;
    context.sawReviewNote = true;
    if (context.noteSink && !context.noteSink(context.noteContext, nullptr, 0, true)) context.invalid = true;
  }
}

bool onBookStateStringStart(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (!inBookStateResult(context) || !responseKeyIs(context, "reviewNote")) return true;
  context.noteActive = true;
  context.result->reviewNotePresent = true;
  context.sawReviewNote = true;
  context.noteWasString = true;
  return true;
}

bool onBookStateStringChunk(void* raw, const char* bytes, const size_t length) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (!context.noteActive) return true;
  return context.noteSink &&
         context.noteSink(context.noteContext, reinterpret_cast<const uint8_t*>(bytes), length, false);
}

bool onBookStateStringEnd(void* raw) {
  auto& context = *static_cast<BookStateResponseContext*>(raw);
  if (!context.noteActive) return true;
  context.noteActive = false;
  return context.noteSink && context.noteSink(context.noteContext, nullptr, 0, true);
}
}  // namespace

int BookOrbitCatalogClient::lastHttpCode = 0;
unsigned BookOrbitCatalogClient::lastHeapAtFailure = 0;
unsigned BookOrbitCatalogClient::lastContigHeapAtFailure = 0;

void BookOrbitCatalogClient::setSkipTlsValidation(bool skip) { g_skipTlsValidation = skip; }

void BookOrbitCatalogClient::beginPersistentSession() {
  if (g_keepSessionOpen) return;
  g_keepSessionOpen = true;
  g_sessionHttp.reset();  // created lazily on the first request
}

void BookOrbitCatalogClient::endPersistentSession() {
  g_keepSessionOpen = false;
  g_sessionHttp.reset();  // closes the kept-open TLS connection
}

std::string BookOrbitCatalogClient::resolveUrl(const std::string& pathOrUrl) {
  if (pathOrUrl.empty()) return catalogUrl("/root");
  if (pathOrUrl.rfind("http", 0) == 0) return pathOrUrl;
  if (pathOrUrl.front() == '/') return baseOrigin() + pathOrUrl;
  return catalogUrl("/" + pathOrUrl);
}

std::string BookOrbitCatalogClient::thumbnailUrl(const int32_t bookId) {
  return catalogUrl("/books/" + std::to_string(bookId) + "/thumbnail");
}

std::vector<std::pair<std::string, std::string>> BookOrbitCatalogClient::authHeaders() {
  return {{"x-auth-user", KOREADER_STORE.getUsername()}, {"x-auth-key", KOREADER_STORE.getMd5Password()}};
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::probeCapabilities() {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/version";
  std::string body;
  lastHttpCode = performRequest("GET", url, "", body);

  if (lastHttpCode < 100) return NETWORK_ERROR;
  // A plain kosync server has no plugin endpoints at all, so its 404 is the
  // normal "not BookOrbit" answer rather than a failure to report.
  if (lastHttpCode == 404) return NOT_SUPPORTED;
  if (const Error status = mapHttpStatus(lastHttpCode); status != OK) return status;

  JsonDocument doc;
  if (const DeserializationError err = deserializeJson(doc, body)) {
    LOG_ERR("BOCat", "Version probe JSON parse failed: %s", err.c_str());
    return JSON_ERROR;
  }

  const JsonArrayConst capabilities = doc["capabilities"].as<JsonArrayConst>();
  if (capabilities.isNull()) return NOT_SUPPORTED;
  for (const JsonVariantConst capability : capabilities) {
    if (capability.is<const char*>() && strcmp(capability.as<const char*>(), CATALOG_CAPABILITY) == 0) {
      LOG_INF("BOCat", "BookOrbit catalog available (server %s, plugin %s)",
              doc["serverVersion"].as<const char*>() ? doc["serverVersion"].as<const char*>() : "?",
              doc["pluginVersion"].as<const char*>() ? doc["pluginVersion"].as<const char*>() : "?");
      return OK;
    }
  }
  LOG_INF("BOCat", "Server answered plugin/version without the %s capability", CATALOG_CAPABILITY);
  return NOT_SUPPORTED;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::fetchListing(
    const std::string& pathOrUrl, const BookOrbitCatalogListParser::EntryCallback& onEntry,
    BookOrbitPageInfo& outPage) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  const std::string url = resolveUrl(pathOrUrl);
  BookOrbitCatalogListParser listParser(onEntry);

  crosspoint::SecureHttpClient local;
  crosspoint::SecureHttpClient& http = clientFor(local);
  configureClient(http);

  const uint32_t startMs = millis();
  const int rc = http.get(url, [&listParser](const uint8_t* data, size_t len) {
    listParser.feed(reinterpret_cast<const char*>(data), len);
    return !listParser.hasError();
  });
  if (listParser.hasError()) {
    // SecureHttpClient aborts the body read once the streaming parser rejects
    // a chunk, so rc is its body error rather than the received HTTP status.
    lastHttpCode = http.lastStatus();
    const BookOrbitPageInfo& parsedPage = listParser.pageInfo();
    LOG_ERR("BOCat", "Listing JSON parse failure: HTTP=%d body_rc=%d rows=%u total=%ld page=%ld", lastHttpCode, rc,
            static_cast<unsigned>(listParser.entryCount()), static_cast<long>(parsedPage.total),
            static_cast<long>(parsedPage.page));
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  lastHttpCode = rc;
  LOG_DBG("BOCat", "GET -> %d in %lu ms (%u rows, total=%ld page=%ld)", rc,
          static_cast<unsigned long>(millis() - startMs), static_cast<unsigned>(listParser.entryCount()),
          static_cast<long>(listParser.pageInfo().total), static_cast<long>(listParser.pageInfo().page));

  if (rc < 100) {
    LOG_ERR("BOCat", "Listing request failed before HTTP response: rc=%d", rc);
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  if (const Error status = mapHttpStatus(rc); status != OK) {
    LOG_ERR("BOCat", "Listing HTTP failure: status=%d mapped_error=%d", rc, static_cast<int>(status));
    return status;
  }

  outPage = listParser.pageInfo();
  return OK;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::fetchSnapshot(const std::string& query,
                                                                    const BookOrbitSnapshotCallbacks& callbacks,
                                                                    BookOrbitSnapshotPage& outPage) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  BookOrbitCatalogSnapshotParser parser(callbacks);
  if (parser.hasError()) return JSON_ERROR;
  if (!heapAllowsTls()) return NETWORK_ERROR;
  const std::string url = catalogUrl("/snapshot?" + query);
  crosspoint::SecureHttpClient local;
  crosspoint::SecureHttpClient& http = clientFor(local);
  configureClient(http);
  const uint32_t startMs = millis();
  const int rc = http.get(url, [&parser](const uint8_t* bytes, size_t length) {
    parser.feed(reinterpret_cast<const char*>(bytes), length);
    return !parser.hasError();
  });
  lastHttpCode = parser.hasError() ? http.lastStatus() : rc;
  LOG_DBG("BOCat", "Snapshot GET -> %d in %lu ms", lastHttpCode, static_cast<unsigned long>(millis() - startMs));
  if (rc >= 100) {
    if (const Error status = mapHttpStatus(rc); status != OK) return status;
  }
  if (parser.hasError()) {
    logHeapSnapshot();
    return JSON_ERROR;
  }
  if (rc < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  if (!parser.finish(outPage)) return JSON_ERROR;
  return OK;
}

std::string BookOrbitCatalogClient::searchUrl(const std::string& query) {
  return catalogUrl("/books?sort=title&size=" + std::to_string(LISTING_PAGE_SIZE) + "&q=" + urlEncode(query));
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::fetchDetail(const std::string& pathOrUrl,
                                                                  BookOrbitBookDetail& outDetail) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  std::string body;
  lastHttpCode = performRequest("GET", resolveUrl(pathOrUrl), "", body);
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  if (const Error status = mapHttpStatus(lastHttpCode); status != OK) return status;

  // Filtered DOM parse. The detail response carries related-book sections,
  // collections, genres and tags the reader never shows; the filter drops them
  // in the parser rather than materializing them first, which is what keeps a
  // multi-kilobyte response down to a handful of short strings.
  //
  // The streaming parser used for listings is not usable here: it discards any
  // token longer than its 512-byte buffer, and a book description routinely is.
  JsonDocument filter;
  for (const char* key : {"id", "title", "seriesName", "seriesIndex", "description", "publisher", "publishedYear",
                          "readStatus", "progressPercentage", "rating", "pageCount", "hasCover"}) {
    filter[key] = true;
  }
  filter["authors"][0] = true;
  // [0] is ArduinoJson's idiom for "every element of this array": the filter
  // object repeats for each file, so only these five keys survive per file.
  filter["files"][0]["id"] = true;
  filter["files"][0]["format"] = true;
  filter["files"][0]["fileHash"] = true;
  filter["files"][0]["sizeBytes"] = true;
  filter["files"][0]["downloadUrl"] = true;

  JsonDocument doc;
  if (const DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter))) {
    LOG_ERR("BOCat", "Detail JSON parse failed: %s", err.c_str());
    return JSON_ERROR;
  }
  body.clear();

  outDetail = {};
  outDetail.bookId = doc["id"] | -1;
  outDetail.title = doc["title"] | std::string{};
  for (const JsonVariantConst author : doc["authors"].as<JsonArrayConst>()) {
    const char* name = author.as<const char*>();
    if (!name || !*name) continue;
    if (!outDetail.authors.empty()) outDetail.authors += " \xC2\xB7 ";
    outDetail.authors += name;
  }
  outDetail.series = doc["seriesName"] | std::string{};
  if (!outDetail.series.empty()) {
    const std::string index = doc["seriesIndex"] | std::string{};
    if (!index.empty()) {
      outDetail.series += " #";
      outDetail.series += index;
    }
  }
  outDetail.description = doc["description"] | std::string{};
  outDetail.publisher = doc["publisher"] | std::string{};
  outDetail.readStatus = doc["readStatus"] | std::string{};
  outDetail.publishedYear = doc["publishedYear"] | static_cast<int16_t>(-1);
  outDetail.pageCount = doc["pageCount"] | -1;
  outDetail.hasCover = doc["hasCover"] | false;
  if (!doc["progressPercentage"].isNull()) {
    const float percent = doc["progressPercentage"].as<float>();
    outDetail.progressPercent = static_cast<int16_t>(lroundf(std::fmin(std::fmax(percent, 0.0f), 100.0f)));
  }
  if (!doc["rating"].isNull()) {
    outDetail.ratingTenths = static_cast<int16_t>(lroundf(doc["rating"].as<float>() * 10.0f));
  }

  const JsonArrayConst files = doc["files"].as<JsonArrayConst>();
  outDetail.files.reserve(files.size());
  for (const JsonObjectConst entry : files) {
    BookOrbitFile candidate;
    candidate.id = entry["id"] | -1;
    candidate.format = entry["format"] | std::string{};
    candidate.downloadUrl = entry["downloadUrl"] | std::string{};
    candidate.fileHash = entry["fileHash"] | std::string{};
    candidate.sizeBytes = entry["sizeBytes"] | 0u;
    if (candidate.downloadUrl.empty()) continue;  // nothing the reader could fetch
    outDetail.files.push_back(std::move(candidate));
  }

  LOG_DBG("BOCat", "Detail: id=%ld files=%u status=%s", static_cast<long>(outDetail.bookId),
          static_cast<unsigned>(outDetail.files.size()), outDetail.readStatus.c_str());
  return OK;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::setReadStatus(const int32_t bookId, const ReadStatus status) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  JsonDocument doc;
  doc["status"] = readStatusKey(status);
  std::string body;
  serializeJson(doc, body);

  std::string response;
  lastHttpCode = performRequest("PUT", catalogUrl("/books/" + std::to_string(bookId) + "/read-status"), body, response);
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  return mapHttpStatus(lastHttpCode);
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::setRating(const int32_t bookId, const uint8_t stars) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  JsonDocument doc;
  // "Unrated" is a null rating server-side, not a zero.
  if (stars == 0) {
    doc["rating"] = nullptr;
  } else {
    doc["rating"] = stars > 5 ? 5 : stars;
  }
  std::string body;
  serializeJson(doc, body);

  std::string response;
  lastHttpCode = performRequest("PUT", catalogUrl("/books/" + std::to_string(bookId) + "/rating"), body, response);
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  return mapHttpStatus(lastHttpCode);
}
BookOrbitCatalogClient::Error BookOrbitCatalogClient::matchCheck(const std::string& hash,
                                                                 BookOrbitMatchCheckResponse& response) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!validMd5(hash)) return JSON_ERROR;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  JsonDocument request;
  if (!addDeviceMetadata(request)) return JSON_ERROR;
  JsonArray hashes = request["hashes"].to<JsonArray>();
  hashes.add(lowerMd5(hash));
  std::string requestBody;
  serializeJson(request, requestBody);

  std::string responseBody;
  lastHttpCode = performRequest("POST", KOREADER_STORE.getBaseUrl() + "/plugin/match-check", requestBody, responseBody);
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  if (const Error status = mapHttpStatus(lastHttpCode); status != OK) return status;

  JsonDocument doc;
  if (deserializeJson(doc, responseBody) || !doc["matches"].is<JsonArrayConst>()) return JSON_ERROR;
  const char* version = doc["libraryVersion"].as<const char*>();
  const JsonArrayConst matches = doc["matches"].as<JsonArrayConst>();
  if (!version || matches.size() > 1) return JSON_ERROR;
  response = {};
  response.libraryVersion = version;
  response.matches.reserve(matches.size());
  for (const JsonObjectConst entry : matches) {
    const char* matchedHash = entry["hash"].as<const char*>();
    const int32_t bookId = entry["bookId"] | -1;
    const int32_t bookFileId = entry["bookFileId"] | -1;
    if (!matchedHash || !validMd5(matchedHash) || lowerMd5(matchedHash) != lowerMd5(hash) || bookId <= 0 ||
        bookFileId <= 0) {
      response = {};
      return JSON_ERROR;
    }
    response.matches.push_back({lowerMd5(matchedHash), bookId, bookFileId});
  }
  return OK;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::uploadBookState(const BookOrbitBookStateUpload& state,
                                                                      const BookOrbitReviewNoteSink noteSink,
                                                                      void* const noteContext,
                                                                      BookOrbitBookStateResult& result) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (!validMd5(state.hash) || !noteSink) return JSON_ERROR;
  const char* statusKey = state.hasStatus ? bookStateStatusKey(state.status) : nullptr;
  if ((state.hasStatus || state.hasRating) && !validDateOnly(state.statusModified)) return JSON_ERROR;
  if (state.hasRating && !state.ratingCleared && (state.rating < 1 || state.rating > 5)) return JSON_ERROR;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  JsonDocument request;
  if (!addDeviceMetadata(request)) return JSON_ERROR;
  JsonArray books = request["books"].to<JsonArray>();
  JsonObject book = books.add<JsonObject>();
  const std::string normalizedHash = lowerMd5(state.hash);
  book["hash"] = normalizedHash;
  if (state.hasStatus) {
    book["status"] = statusKey;
  }
  if (state.hasStatus || state.hasRating) book["statusModified"] = state.statusModified;
  if (state.hasRating) {
    if (state.ratingCleared) {
      book["ratingCleared"] = true;
    } else {
      book["rating"] = state.rating;
    }
  }
  std::string requestBody;
  serializeJson(request, requestBody);

  result = {};
  result.hash = normalizedHash;
  BookStateResponseContext context;
  context.expectedHash = normalizedHash;
  context.result = &result;
  context.noteSink = noteSink;
  context.noteContext = noteContext;
  JsonCallbacks callbacks{};
  callbacks.ctx = &context;
  callbacks.onKey = onBookStateKey;
  callbacks.onString = onBookStateString;
  callbacks.onNumber = onBookStateNumber;
  callbacks.onBool = onBookStateBool;
  callbacks.onNull = onBookStateNull;
  callbacks.onObjectStart = onBookStateObjectStart;
  callbacks.onObjectEnd = onBookStateObjectEnd;
  callbacks.onArrayStart = onBookStateArrayStart;
  callbacks.onArrayEnd = onBookStateArrayEnd;
  callbacks.onStringStart = onBookStateStringStart;
  callbacks.onStringChunk = onBookStateStringChunk;
  callbacks.onStringEnd = onBookStateStringEnd;
  StreamingJsonParser parser(callbacks);
  const crosspoint::SecureHttpClient::BodySink responseSink = [&parser](const uint8_t* bytes, const size_t length) {
    parser.feed(reinterpret_cast<const char*>(bytes), length);
    return !parser.hasError();
  };
  lastHttpCode =
      performStreamingRequest("POST", KOREADER_STORE.getBaseUrl() + "/plugin/book-states", requestBody, responseSink);
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  if (const Error status = mapHttpStatus(lastHttpCode); status != OK) return status;
  if (parser.hasError() || !context.complete()) return JSON_ERROR;
  if (result.unmatched) {
    if (context.matchedResultSeen || context.resultObjectSeen || !context.unmatchedHashSeen) return JSON_ERROR;
    return OK;
  }
  if (!context.resultObjectSeen || !context.resultObjectClosed || !context.matchedResultSeen ||
      result.hash != normalizedHash || !context.sawStatusApplied || !context.sawRatingApplied ||
      !context.sawReviewApplied || !context.sawRatingSet || !context.sawRatingValue || !context.sawReviewNoteSet ||
      !context.sawReviewNote || !context.sawRatingUpdatedAt || !context.sawReviewUpdatedAt ||
      result.ratingSet != (result.rating > 0) || result.reviewNoteSet != context.noteWasString) {
    return JSON_ERROR;
  }
  return OK;
}
BookOrbitCatalogClient::Error BookOrbitCatalogClient::uploadPageStats(const std::vector<BookOrbitPageStatsBook>& books,
                                                                      std::string& response) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (books.empty() || books.size() > 50) return JSON_ERROR;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  size_t eventCount = 0;
  for (const BookOrbitPageStatsBook& book : books) {
    if (book.hash.size() != 32 || book.events.empty()) return JSON_ERROR;
    for (const char digit : book.hash) {
      if (!std::isxdigit(static_cast<unsigned char>(digit))) return JSON_ERROR;
    }
    eventCount += book.events.size();
    if (eventCount > MAX_PAGE_STATS_EVENTS_PER_REQUEST) return JSON_ERROR;
  }

  JsonDocument doc;
  JsonArray deviceBooks = doc["books"].to<JsonArray>();
  for (const BookOrbitPageStatsBook& book : books) {
    JsonObject deviceBook = deviceBooks.add<JsonObject>();
    deviceBook["hash"] = book.hash;
    if (book.bookFileId > 0) deviceBook["bookFileId"] = book.bookFileId;
    JsonArray events = deviceBook["events"].to<JsonArray>();
    for (const BookOrbitPageStatEvent& event : book.events) {
      if (event.startTime == 0 || event.durationSeconds > 86400 || event.totalPages == 0) return JSON_ERROR;
      JsonObject item = events.add<JsonObject>();
      item["page"] = event.page;
      item["startTime"] = event.startTime;
      item["durationSeconds"] = event.durationSeconds;
      item["totalPages"] = event.totalPages;
    }
  }

  doc["deviceId"] = KOReaderSyncClient::deviceId();
  char deviceModel[40] = "CrossPoint ";
  const size_t boardNameLen = board_tag::boardNameLen();
  if (boardNameLen > sizeof(deviceModel) - sizeof("CrossPoint ")) return JSON_ERROR;
  memcpy(deviceModel + sizeof("CrossPoint ") - 1, board_tag::boardName(), boardNameLen);
  deviceModel[sizeof("CrossPoint ") - 1 + boardNameLen] = '\0';
  doc["deviceModel"] = deviceModel;
  doc["pluginVersion"] = CROSSPOINT_VERSION;
  if (HalClock::isSynced()) {
    const time_t now = HalClock::now();
    struct tm local{};
    if (localtime_r(&now, &local)) {
      char deviceTime[20];
      if (strftime(deviceTime, sizeof(deviceTime), "%Y-%m-%d %H:%M:%S", &local) > 0) {
        doc["deviceTime"] = deviceTime;
      }
    }
  }

  std::string body;
  serializeJson(doc, body);
  const int rc = performRequest("POST", KOREADER_STORE.getBaseUrl() + "/plugin/page-stats", body, response);
  lastHttpCode = rc;
  if (rc < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  if (const Error status = mapHttpStatus(rc); status != OK) return status;
  JsonDocument ack;
  if (deserializeJson(ack, response)) return JSON_ERROR;
  return OK;
}

BookOrbitCatalogClient::Error BookOrbitCatalogClient::exchangeBookmarks(const BookOrbitBookmarkExchangeRequest& request,
                                                                        BookOrbitBookmarkExchangeResponse& response) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (request.books.empty() || request.books.size() > 20) return JSON_ERROR;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  JsonArray books = root["books"].to<JsonArray>();
  for (size_t bookIndex = 0; bookIndex < request.books.size(); ++bookIndex) {
    const BookOrbitBookmarkExchangeBook& book = request.books[bookIndex];
    if (!validBookmarkHash(book.hash) || book.keys.size() > 500 || book.changes.size() > 8) return JSON_ERROR;
    for (size_t previous = 0; previous < bookIndex; ++previous) {
      if (sameBookmarkHash(request.books[previous].hash, book.hash)) return JSON_ERROR;
    }
    JsonObject item = books.add<JsonObject>();
    item["hash"] = book.hash;
    item["keysComplete"] = book.keysComplete;
    JsonArray keys = item["keys"].to<JsonArray>();
    for (const BookOrbitBookmarkKey& key : book.keys) {
      if (key.key[32] != '\0' || key.datetime[19] != '\0') return JSON_ERROR;
      const std::string_view datetime(key.datetime.data(), 19);
      const std::string_view keyValue(key.key.data(), 32);
      if (!validBookmarkHash(keyValue) || !validDeviceDatetime(datetime)) return JSON_ERROR;
      JsonObject encoded = keys.add<JsonObject>();
      encoded["k"] = key.key.data();
      encoded["dt"] = key.datetime.data();
    }
    JsonArray changes = item["changes"].to<JsonArray>();
    for (const BookOrbitBookmarkChange& change : book.changes) {
      if (!validDeviceDatetime(change.datetime) ||
          (!change.datetimeUpdated.empty() && !validDeviceDatetime(change.datetimeUpdated)) ||
          change.position.empty() || change.position.size() > MAX_BOOKMARK_POSITION_BYTES ||
          !withinBookmarkTitleLimit(change.chapter) || !withinBookmarkTitleLimit(change.note)) {
        return JSON_ERROR;
      }
      JsonObject encoded = changes.add<JsonObject>();
      encoded["datetime"] = change.datetime;
      if (!change.datetimeUpdated.empty()) encoded["datetimeUpdated"] = change.datetimeUpdated;
      encoded["pos"] = change.position;
      encoded["pageno"] = change.page;
      if (!change.chapter.empty()) encoded["chapter"] = change.chapter;
      if (!change.note.empty()) encoded["note"] = change.note;
    }
  }
  root["deviceId"] = KOReaderSyncClient::deviceId();
  char deviceModel[40] = "CrossPoint ";
  const size_t boardNameLen = board_tag::boardNameLen();
  if (boardNameLen > sizeof(deviceModel) - sizeof("CrossPoint ")) return JSON_ERROR;
  memcpy(deviceModel + sizeof("CrossPoint ") - 1, board_tag::boardName(), boardNameLen);
  deviceModel[sizeof("CrossPoint ") - 1 + boardNameLen] = '\0';
  root["deviceModel"] = deviceModel;
  root["pluginVersion"] = CROSSPOINT_VERSION;
  if (HalClock::isSynced()) {
    const time_t now = HalClock::now();
    struct tm local{};
    if (localtime_r(&now, &local)) {
      char deviceTime[20];
      if (strftime(deviceTime, sizeof(deviceTime), "%Y-%m-%d %H:%M:%S", &local) > 0) {
        root["deviceTime"] = deviceTime;
      }
    }
  }

  std::string body;
  serializeJson(doc, body);
  BookmarkExchangeParser parsed(request, response);
  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/bookmarks/exchange";
  lastHttpCode = performStreamingRequest("POST", url, body, [&parsed](const uint8_t* data, const size_t length) {
    parsed.feed(reinterpret_cast<const char*>(data), length);
    return !parsed.hasError();
  });
  if (lastHttpCode >= 100) {
    if (const Error status = mapHttpStatus(lastHttpCode); status != OK) return status;
  }
  if (parsed.hasError()) return JSON_ERROR;
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  return parsed.complete() ? OK : JSON_ERROR;
}
BookOrbitCatalogClient::Error BookOrbitCatalogClient::acknowledgeBookmarks(const BookOrbitBookmarkAckRequest& request,
                                                                           std::string& response) {
  if (!hasCredentials()) return NO_CREDENTIALS;
  if (request.books.empty() || request.books.size() > 20) return JSON_ERROR;
  if (!heapAllowsTls()) return NETWORK_ERROR;

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  JsonArray books = root["books"].to<JsonArray>();
  for (size_t bookIndex = 0; bookIndex < request.books.size(); ++bookIndex) {
    const BookOrbitBookmarkAckBook& book = request.books[bookIndex];
    if (!validBookmarkHash(book.hash) || book.applied.size() + book.deleted.size() > MAX_BOOKMARK_RESPONSE_ITEMS) {
      return JSON_ERROR;
    }
    for (size_t previous = 0; previous < bookIndex; ++previous) {
      if (sameBookmarkHash(request.books[previous].hash, book.hash)) return JSON_ERROR;
    }
    std::array<int32_t, MAX_BOOKMARK_RESPONSE_ITEMS> seenIds{};
    size_t seenCount = 0;
    auto addUniqueId = [&seenIds, &seenCount](const int32_t id) {
      if (id <= 0 || std::find(seenIds.begin(), seenIds.begin() + seenCount, id) != seenIds.begin() + seenCount) {
        return false;
      }
      seenIds[seenCount++] = id;
      return true;
    };

    JsonObject item = books.add<JsonObject>();
    item["hash"] = book.hash;
    JsonArray applied = item["applied"].to<JsonArray>();
    for (const BookOrbitBookmarkAckApplied& entry : book.applied) {
      if (!addUniqueId(entry.serverId) ||
          (entry.applied && (!validBookmarkHash(entry.key) || !validDeviceDatetime(entry.datetime) ||
                             entry.position.empty() || entry.position.size() > MAX_BOOKMARK_POSITION_BYTES))) {
        return JSON_ERROR;
      }
      JsonObject encoded = applied.add<JsonObject>();
      encoded["serverId"] = entry.serverId;
      encoded["status"] = entry.applied ? "applied" : "failed";
      if (entry.applied) {
        encoded["key"] = entry.key;
        encoded["datetime"] = entry.datetime;
        encoded["pos"] = entry.position;
      }
    }
    JsonArray deleted = item["deleted"].to<JsonArray>();
    for (const BookOrbitBookmarkAckDeleted& entry : book.deleted) {
      if (!addUniqueId(entry.serverId)) return JSON_ERROR;
      JsonObject encoded = deleted.add<JsonObject>();
      encoded["serverId"] = entry.serverId;
      encoded["status"] = entry.applied ? "applied" : "failed";
    }
  }
  root["deviceId"] = KOReaderSyncClient::deviceId();
  char deviceModel[40] = "CrossPoint ";
  const size_t boardNameLen = board_tag::boardNameLen();
  if (boardNameLen > sizeof(deviceModel) - sizeof("CrossPoint ")) return JSON_ERROR;
  memcpy(deviceModel + sizeof("CrossPoint ") - 1, board_tag::boardName(), boardNameLen);
  deviceModel[sizeof("CrossPoint ") - 1 + boardNameLen] = '\0';
  root["deviceModel"] = deviceModel;
  root["pluginVersion"] = CROSSPOINT_VERSION;
  if (HalClock::isSynced()) {
    const time_t now = HalClock::now();
    struct tm local{};
    if (localtime_r(&now, &local)) {
      char deviceTime[20];
      if (strftime(deviceTime, sizeof(deviceTime), "%Y-%m-%d %H:%M:%S", &local) > 0) {
        root["deviceTime"] = deviceTime;
      }
    }
  }

  std::string body;
  serializeJson(doc, body);
  response.clear();
  BookmarkAckResponseParser parsed(request);
  bool responseTooLarge = false;
  const std::string url = KOREADER_STORE.getBaseUrl() + "/plugin/bookmarks/exchange-ack";
  lastHttpCode = performStreamingRequest(
      "POST", url, body, [&parsed, &response, &responseTooLarge](const uint8_t* data, const size_t length) {
        constexpr size_t MAX_ACK_RESPONSE_BYTES = 4096;
        if (length > MAX_ACK_RESPONSE_BYTES - response.size()) {
          responseTooLarge = true;
          return false;
        }
        response.append(reinterpret_cast<const char*>(data), length);
        parsed.feed(reinterpret_cast<const char*>(data), length);
        return !parsed.hasError();
      });
  if (lastHttpCode >= 100) {
    if (const Error status = mapHttpStatus(lastHttpCode); status != OK) return status;
  }
  if (responseTooLarge || parsed.hasError()) return JSON_ERROR;
  if (lastHttpCode < 100) {
    logHeapSnapshot();
    return NETWORK_ERROR;
  }
  return parsed.complete() ? OK : JSON_ERROR;
}

const char* BookOrbitCatalogClient::readStatusKey(const ReadStatus status) {
  switch (status) {
    case ReadStatus::Unread:
      return "unread";
    case ReadStatus::WantToRead:
      return "want_to_read";
    case ReadStatus::Reading:
      return "reading";
    case ReadStatus::OnHold:
      return "on_hold";
    case ReadStatus::Read:
      return "read";
    case ReadStatus::Abandoned:
      return "abandoned";
  }
  return "unread";
}

BookOrbitCatalogClient::ReadStatus BookOrbitCatalogClient::readStatusFromKey(const std::string& key) {
  if (key == "want_to_read") return ReadStatus::WantToRead;
  if (key == "reading") return ReadStatus::Reading;
  if (key == "on_hold") return ReadStatus::OnHold;
  if (key == "read") return ReadStatus::Read;
  if (key == "abandoned") return ReadStatus::Abandoned;
  return ReadStatus::Unread;
}

const char* BookOrbitCatalogClient::errorString(const Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No KOReader credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "Unexpected response from server";
    case NOT_FOUND:
      return "Not found on server";
    case NOT_SUPPORTED:
      return "No BookOrbit catalog on this server";
  }
  return "Unknown error";
}
