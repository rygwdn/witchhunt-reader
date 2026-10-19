#include "BookOrbitCatalogSnapshotParser.h"

#include "../JsonParser/StreamingJsonParser.h"
#include "../Memory/Memory.h"

#if defined(ARDUINO)
#include <Logging.h>
#endif

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>
namespace {
constexpr size_t MAX_MEMBERSHIPS_PER_KIND = 500;
constexpr size_t MAX_UNIQUE_FORMATS = 20;
constexpr size_t MAX_FORMAT_TOKEN_BYTES = 32;
constexpr size_t MAX_JOINED_BYTES = 512;
constexpr char JOIN_SEPARATOR[] = " \xC2\xB7 ";
enum class Key : uint8_t {
  None,
  Version,
  Generation,
  Etag,
  Total,
  Books,
  Sources,
  Libraries,
  Collections,
  SmartScopes,
  Shelves,
  ContinueReading,
  Discover,
  NextCursor,
  Id,
  Title,
  Authors,
  SeriesName,
  SeriesIndex,
  ProgressPercentage,
  HasCover,
  Files,
  Format,
  Memberships,
  LibraryIds,
  CollectionIds,
  SmartScopeIds,
  Name,
  BookCount
};
enum class Array : uint8_t {
  None,
  Books,
  Libraries,
  Collections,
  SmartScopes,
  ContinueReading,
  Discover,
  Authors,
  Files,
  LibraryIds,
  CollectionIds,
  SmartScopeIds
};
bool isKey(const char* key, size_t length, const char* expected) {
  return length == strlen(expected) && memcmp(key, expected, length) == 0;
}
bool intValue(const char* text, size_t length, int32_t& result) {
  if (!length || length >= 16) return false;
  char buffer[16];
  memcpy(buffer, text, length);
  buffer[length] = '\0';
  char* end = nullptr;
  errno = 0;
  const long value = strtol(buffer, &end, 10);
  if (errno || end != buffer + length || value < 0 || value > INT32_MAX) return false;
  result = static_cast<int32_t>(value);
  return true;
}
BookOrbitCatalogSettings::SourceKind sourceKind(Array array) {
  if (array == Array::Libraries) return BookOrbitCatalogSettings::SourceKind::Library;
  if (array == Array::Collections) return BookOrbitCatalogSettings::SourceKind::Collection;
  return BookOrbitCatalogSettings::SourceKind::SmartScope;
}
}  // namespace

struct BookOrbitCatalogSnapshotParser::Impl {
  explicit Impl(const BookOrbitSnapshotCallbacks& cb)
      : callbacks(cb),
        parser(JsonCallbacks{.ctx = this,
                             .onKey = key,
                             .onString = string,
                             .onNumber = number,
                             .onBool = boolean,
                             .onNull = nullValue,
                             .onObjectStart = objectStart,
                             .onObjectEnd = objectEnd,
                             .onArrayStart = arrayStart,
                             .onArrayEnd = arrayEnd,
                             .onStringOverflow = stringOverflow}) {}
  static void key(void* p, const char* text, size_t n) {
    auto& s = *static_cast<Impl*>(p);
    if (s.error) return;
    if (s.depth > StreamingJsonParser::MAX_NESTING) {
      s.fail();
      return;
    }
    Key k = Key::None;
#define K(name, value) \
  if (isKey(text, n, value)) k = Key::name
    K(Version, "version");
    else K(Generation, "generation");
    else K(Etag, "etag");
    else K(Total, "total");
    else K(Books, "books");
    else K(Sources, "sources");
    else K(Libraries, "libraries");
    else K(Collections, "collections");
    else K(SmartScopes, "smartScopes");
    else K(Shelves, "shelves");
    else K(ContinueReading, "continueReading");
    else K(Discover, "discover");
    else K(NextCursor, "nextCursor");
    else K(Id, "id");
    else K(Title, "title");
    else K(Authors, "authors");
    else K(SeriesName, "seriesName");
    else K(SeriesIndex, "seriesIndex");
    else K(ProgressPercentage, "progressPercentage");
    else K(HasCover, "hasCover");
    else K(Files, "files");
    else K(Format, "format");
    else K(Memberships, "memberships");
    else K(LibraryIds, "libraryIds");
    else K(CollectionIds, "collectionIds");
    else K(SmartScopeIds, "smartScopeIds");
    else K(Name, "name");
    else K(BookCount, "bookCount");
#undef K
    s.keys[s.depth] = k;
  }
  static void stringOverflow(void* p, const char*, size_t) {
    auto& s = *static_cast<Impl*>(p);
    const Array a = s.activeArray();
    if ((s.depth == 2 && a == Array::Books) ||
        (s.depth == 3 && (a == Array::Libraries || a == Array::Collections || a == Array::SmartScopes ||
                          a == Array::ContinueReading || a == Array::Discover)) ||
        (s.depth == 4 && (a == Array::Authors || (s.bookOpen && a == Array::Files))) ||
        (s.depth == 5 && (a == Array::LibraryIds || a == Array::CollectionIds || a == Array::SmartScopeIds)))
      s.fail();
  }
  static void string(void* p, const char* value, size_t n) {
    auto& s = *static_cast<Impl*>(p);
    if (s.error) return;
    const Key k = s.currentKey();
    const Array a = s.activeArray();
    if ((s.depth == 2 && a == Array::Books) ||
        (s.bookOpen && s.depth == 5 &&
         (a == Array::LibraryIds || a == Array::CollectionIds || a == Array::SmartScopeIds)) ||
        (s.bookOpen && s.depth == 4 && a == Array::Files) ||
        (s.depth == 3 && (a == Array::Libraries || a == Array::Collections || a == Array::SmartScopes ||
                          a == Array::ContinueReading || a == Array::Discover))) {
      s.fail();
      return;
    }
    if (s.depth == 1 && k == Key::Generation) {
      s.page.generation.assign(value, n);
      s.generationSeen = true;
    } else if (s.depth == 1 && k == Key::Etag) {
      s.page.etag.assign(value, n);
      s.etagSeen = true;
    } else if (s.depth == 1 && k == Key::NextCursor) {
      s.page.nextCursor.assign(value, n);
      s.cursorSeen = true;
    } else if (s.bookOpen && s.depth == 3 && k == Key::Title) {
      s.book.title.assign(value, n);
      s.bookFields |= 1;
    } else if (s.bookOpen && s.depth == 3 && k == Key::SeriesName)
      s.book.series.assign(value, n);
    else if (s.bookOpen && s.depth == 3 && k == Key::SeriesIndex)
      s.seriesIndex.assign(value, n);
    else if (s.bookOpen && s.activeArray() == Array::Authors && s.depth == 4)
      s.join(s.book.subtitle, value, n);
    else if (s.bookOpen && s.activeArray() == Array::Files && s.depth == 5 && k == Key::Format) {
      s.appendUniqueFormat(value, n);
      s.fileFormatSeen = true;
    } else if (s.sourceOpen && s.depth == 4 && k == Key::Name) {
      s.sourceName.assign(value, n);
      s.sourceFields |= 2;
    }
  }
  static void number(void* p, const char* value, size_t n) {
    auto& s = *static_cast<Impl*>(p);
    if (s.error) return;
    const Key k = s.currentKey();
    int32_t v = 0;
    if (s.bookOpen && s.depth == 3 && k == Key::ProgressPercentage) {
      const double d = strtod(value, nullptr);
      s.book.progressPercent = d <= 0 ? 0 : d >= 100 ? 100 : static_cast<int16_t>(d + 0.5);
      s.bookFields |= 16;
      return;
    }
    const Array a = s.activeArray();
    if ((s.depth == 2 && a == Array::Books) ||
        (s.depth == 3 && (a == Array::Libraries || a == Array::Collections || a == Array::SmartScopes)) ||
        (s.bookOpen && s.depth == 4 && (a == Array::Authors || a == Array::Files))) {
      s.fail();
      return;
    }
    if (k != Key::Version && k != Key::Total && k != Key::Id && k != Key::BookCount && a != Array::LibraryIds &&
        a != Array::CollectionIds && a != Array::SmartScopeIds && a != Array::ContinueReading && a != Array::Discover)
      return;
    if (!intValue(value, n, v)) {
      s.fail();
      return;
    }
    if (s.depth == 1 && k == Key::Version) {
      s.page.version = static_cast<uint32_t>(v);
      s.versionSeen = true;
    } else if (s.depth == 1 && k == Key::Total) {
      s.page.total = v;
      s.totalSeen = true;
    } else if (s.bookOpen && s.depth == 5 && a == Array::Files && k == Key::Id && v <= 0)
      s.fail();
    else if (s.bookOpen && s.depth == 3 && k == Key::Id) {
      s.book.bookId = v;
      s.bookFields |= 2;
    } else if (s.sourceOpen && s.depth == 4 && k == Key::Id) {
      s.sourceId = v;
      s.sourceFields |= 1;
    } else if (s.sourceOpen && s.depth == 4 && k == Key::BookCount) {
      s.sourceCount = v;
      s.sourceFields |= 4;
    } else if (s.bookOpen && s.depth == 5 && a == Array::LibraryIds)
      s.addMembership(0, v);
    else if (s.bookOpen && s.depth == 5 && a == Array::CollectionIds)
      s.addMembership(1, v);
    else if (s.bookOpen && s.depth == 5 && a == Array::SmartScopeIds)
      s.addMembership(2, v);
    else if (s.depth == 3 && a == Array::ContinueReading)
      s.emitShelf(BookOrbitEntryGroup::CONTINUE_READING, v);
    else if (s.depth == 3 && a == Array::Discover)
      s.emitShelf(BookOrbitEntryGroup::DISCOVER, v);
  }
  static void boolean(void* p, bool value) {
    auto& s = *static_cast<Impl*>(p);
    if (s.error) return;
    const Array a = s.activeArray();
    if ((s.depth == 2 && a == Array::Books) ||
        (s.depth == 3 && (a == Array::Libraries || a == Array::Collections || a == Array::SmartScopes ||
                          a == Array::ContinueReading || a == Array::Discover)) ||
        (s.depth == 4 && (a == Array::Authors || (s.bookOpen && a == Array::Files))) ||
        (s.depth == 5 && (a == Array::LibraryIds || a == Array::CollectionIds || a == Array::SmartScopeIds))) {
      s.fail();
      return;
    }
    if (s.bookOpen && s.depth == 3 && s.currentKey() == Key::HasCover) {
      s.book.hasCover = value;
      s.bookFields |= 32;
    }
  }
  static void nullValue(void* p) {
    auto& s = *static_cast<Impl*>(p);
    if (s.error) return;
    const Array a = s.activeArray();
    if ((s.depth == 2 && a == Array::Books) ||
        (s.depth == 3 && (a == Array::Libraries || a == Array::Collections || a == Array::SmartScopes ||
                          a == Array::ContinueReading || a == Array::Discover)) ||
        (s.depth == 4 && (a == Array::Authors || (s.bookOpen && a == Array::Files))) ||
        (s.depth == 5 && (a == Array::LibraryIds || a == Array::CollectionIds || a == Array::SmartScopeIds))) {
      s.fail();
      return;
    }
    const Key k = s.currentKey();
    if (s.depth == 1 && k == Key::NextCursor) {
      s.page.nextCursor.clear();
      s.cursorSeen = true;
    } else if (s.bookOpen && s.depth == 3 && k == Key::ProgressPercentage) {
      s.book.progressPercent = -1;
      s.bookFields |= 16;
    }
  }
  static void objectStart(void* p) {
    auto& s = *static_cast<Impl*>(p);
    const uint8_t parent = s.depth;
    const Key k = parent ? s.keys[parent] : Key::None;
    const Array parentArray = s.activeArray();
    if (s.error) return;
    ++s.depth;
    if (parent == 0) {
      if (s.rootSeen) s.fail();
      s.rootSeen = true;
    } else if (parent == 2 && parentArray == Array::Books) {
      s.bookOpen = true;
      s.book.type = BookOrbitEntryType::BOOK;
      s.book.group = BookOrbitEntryGroup::STANDARD;
      s.book.title.clear();
      s.book.subtitle.clear();
      s.book.href.clear();
      s.book.series.clear();
      s.book.formats.clear();
      s.book.bookId = -1;
      s.book.count = -1;
      s.book.progressPercent = -1;
      s.book.hasCover = false;
      s.seriesIndex.clear();
      s.bookFields = 0;
      s.membershipCounts[0] = s.membershipCounts[1] = s.membershipCounts[2] = 0;
      s.formatCount = 0;
    } else if (parent == 1 && k == Key::Sources)
      s.sourcesOpen = true;
    else if (parent == 1 && k == Key::Shelves)
      s.shelvesOpen = true;
    else if ((parent == 2 && parentArray != Array::None) ||
             (parent == 3 && (parentArray == Array::ContinueReading || parentArray == Array::Discover)) ||
             (parent == 4 && parentArray == Array::Authors) ||
             (parent == 5 && (parentArray == Array::LibraryIds || parentArray == Array::CollectionIds ||
                              parentArray == Array::SmartScopeIds))) {
      s.fail();
    } else if (parent == 3 && parentArray != Array::None && s.sourcesOpen) {
      s.sourceOpen = true;
      s.sourceFields = 0;
      s.sourceId = s.sourceCount = -1;
      s.sourceName.clear();
    } else if (parent == 4 && s.bookOpen && parentArray == Array::Files) {
      s.fileOpen = true;
      s.fileFormatSeen = false;
    } else if (parent == 3 && s.bookOpen && k == Key::Memberships)
      s.membershipsSeen = true;
  }
  static void objectEnd(void* p) {
    auto& s = *static_cast<Impl*>(p);
    if (s.error) return;
    if (s.fileOpen && s.depth == 5) {
      if (!s.fileFormatSeen) s.fail();
      s.fileOpen = false;
    }
    if (s.bookOpen && s.depth == 3) s.commitBook();
    if (s.sourceOpen && s.depth == 4) s.commitSource();
    if (s.depth == 2 && s.sourcesOpen) s.sourcesOpen = false;
    if (s.depth == 2 && s.shelvesOpen) s.shelvesOpen = false;
    if (s.depth == 1) s.rootClosed = true;
    if (s.depth) --s.depth;
  }
  static void arrayStart(void* p) {
    auto& s = *static_cast<Impl*>(p);
    const Key k = s.currentKey();
    Array a = Array::None;
    if (s.error) return;
    const Array parentArray = s.activeArray();
    if ((s.depth == 2 && parentArray == Array::Books) ||
        (s.depth == 3 &&
         (parentArray == Array::Libraries || parentArray == Array::Collections || parentArray == Array::SmartScopes ||
          parentArray == Array::ContinueReading || parentArray == Array::Discover)) ||
        (s.depth == 4 && (parentArray == Array::Authors || parentArray == Array::Files)) ||
        (s.depth == 5 && (parentArray == Array::LibraryIds || parentArray == Array::CollectionIds ||
                          parentArray == Array::SmartScopeIds))) {
      s.fail();
      return;
    }
    if (s.depth == 1 && k == Key::Books)
      a = Array::Books;
    else if (s.depth == 2 && s.sourcesOpen && k == Key::Libraries)
      a = Array::Libraries;
    else if (s.depth == 2 && s.sourcesOpen && k == Key::Collections)
      a = Array::Collections;
    else if (s.depth == 2 && s.sourcesOpen && k == Key::SmartScopes)
      a = Array::SmartScopes;
    else if (s.depth == 2 && s.shelvesOpen && k == Key::ContinueReading)
      a = Array::ContinueReading;
    else if (s.depth == 2 && s.shelvesOpen && k == Key::Discover)
      a = Array::Discover;
    else if (s.bookOpen && s.depth == 3 && k == Key::Authors) {
      a = Array::Authors;
      s.authorsSeen = true;
    } else if (s.bookOpen && s.depth == 3 && k == Key::Files) {
      a = Array::Files;
      s.filesSeen = true;
    } else if (s.bookOpen && s.depth == 4 && k == Key::LibraryIds) {
      a = Array::LibraryIds;
      s.membershipKinds |= 1;
    } else if (s.bookOpen && s.depth == 4 && k == Key::CollectionIds) {
      a = Array::CollectionIds;
      s.membershipKinds |= 2;
    } else if (s.bookOpen && s.depth == 4 && k == Key::SmartScopeIds) {
      a = Array::SmartScopeIds;
      s.membershipKinds |= 4;
    }
    ++s.depth;
    s.arrays[s.depth] = a;
    if (a == Array::Libraries)
      s.sourceArray = a;
    else if (a == Array::Collections)
      s.sourceArray = a;
    else if (a == Array::SmartScopes)
      s.sourceArray = a;
  }
  static void arrayEnd(void* p) {
    auto& s = *static_cast<Impl*>(p);
    const Array a = s.activeArray();
    if (s.error) return;
    if (a == Array::Books)
      s.booksSeen = true;
    else if (a == Array::Libraries)
      s.librariesSeen = true;
    else if (a == Array::Collections)
      s.collectionsSeen = true;
    else if (a == Array::SmartScopes)
      s.smartScopesSeen = true;
    else if (a == Array::ContinueReading)
      s.continueSeen = true;
    else if (a == Array::Discover)
      s.discoverSeen = true;
    if (s.depth) {
      s.arrays[s.depth] = Array::None;
      --s.depth;
    }
  }
  Key currentKey() const { return depth ? keys[depth] : Key::None; }
  Array activeArray() const {
    for (int i = depth; i > 0; --i)
      if (arrays[i] != Array::None) return arrays[i];
    return Array::None;
  }
  void fail() { error = true; }
  bool join(std::string& target, const char* value, size_t n) {
    const size_t separator = target.empty() ? 0 : sizeof(JOIN_SEPARATOR) - 1;
    if (target.size() > MAX_JOINED_BYTES || n > MAX_JOINED_BYTES - target.size() ||
        separator > MAX_JOINED_BYTES - target.size() - n) {
      fail();
      return false;
    }
    if (separator) target += JOIN_SEPARATOR;
    target.append(value, n);
    return true;
  }
  bool appendUniqueFormat(const char* value, size_t n) {
    if (n == 0 || n > MAX_FORMAT_TOKEN_BYTES) {
      fail();
      return false;
    }
    char normalized[MAX_FORMAT_TOKEN_BYTES + 1];
    for (size_t i = 0; i < n; ++i)
      normalized[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(value[i])));
    normalized[n] = '\0';
    for (size_t i = 0; i < formatCount; ++i)
      if (formatLengths[i] == n && memcmp(formatTokens[i], normalized, n) == 0) return true;
    if (formatCount >= MAX_UNIQUE_FORMATS || !join(book.formats, normalized, n)) {
      fail();
      return false;
    }
    memcpy(formatTokens[formatCount], normalized, n + 1);
    formatLengths[formatCount++] = static_cast<uint8_t>(n);
    return true;
  }
  void addMembership(size_t kind, int32_t id) {
    if (id <= 0 || membershipCounts[kind] >= MAX_MEMBERSHIPS_PER_KIND) {
      fail();
      return;
    }
    membershipIds[kind][membershipCounts[kind]++] = id;
  }
  void emitShelf(BookOrbitEntryGroup group, int32_t id) {
    if (id <= 0 || !callbacks.onShelf || !callbacks.onShelf(callbacks.context, group, id)) fail();
  }
  void commitBook() {
    if ((bookFields & 51) != 51 || (bookFields & 48) != 48 || book.bookId <= 0 || book.title.empty() || !authorsSeen ||
        !filesSeen || !membershipsSeen || membershipKinds != 7) {
      fail();
      bookOpen = false;
      return;
    }
    book.href = "books/" + std::to_string(book.bookId);
    if (!book.series.empty() && !seriesIndex.empty()) {
      book.series += " #";
      book.series += seriesIndex;
    }
    if (!callbacks.onBook || !callbacks.onBook(callbacks.context, book)) {
      fail();
      bookOpen = false;
      return;
    }
    for (size_t k = 0; k < 3; ++k) {
      const auto kind = k == 0   ? BookOrbitCatalogSettings::SourceKind::Library
                        : k == 1 ? BookOrbitCatalogSettings::SourceKind::Collection
                                 : BookOrbitCatalogSettings::SourceKind::SmartScope;
      for (size_t j = 0; j < membershipCounts[k]; ++j)
        if (!callbacks.onMembership ||
            !callbacks.onMembership(callbacks.context, book.bookId, kind, membershipIds[k][j])) {
          fail();
          bookOpen = false;
          return;
        }
    }
    bookOpen = false;
  }
  void commitSource() {
    if ((sourceFields & 7) != 7 || sourceId <= 0 || sourceCount < 0 || sourceName.empty() || !callbacks.onSource ||
        !callbacks.onSource(callbacks.context, sourceKind(sourceArray), sourceId, sourceName, sourceCount))
      fail();
    sourceOpen = false;
    sourceFields = 0;
  }

  const BookOrbitSnapshotCallbacks callbacks;
  StreamingJsonParser parser;
  Key keys[StreamingJsonParser::MAX_NESTING + 1]{};
  Array arrays[StreamingJsonParser::MAX_NESTING + 1]{};
  uint8_t depth = 0;
  bool error = false, rootSeen = false, rootClosed = false;
  bool versionSeen = false, generationSeen = false, etagSeen = false, totalSeen = false, cursorSeen = false;
  bool booksSeen = false, librariesSeen = false, collectionsSeen = false, smartScopesSeen = false, continueSeen = false,
       discoverSeen = false;
  bool sourcesOpen = false, shelvesOpen = false, bookOpen = false, sourceOpen = false;
  bool authorsSeen = false, filesSeen = false, membershipsSeen = false;
  uint8_t bookFields = 0, sourceFields = 0;
  uint8_t membershipKinds = 0;
  bool fileOpen = false, fileFormatSeen = false;
  size_t membershipCounts[3]{};
  int32_t membershipIds[3][MAX_MEMBERSHIPS_PER_KIND]{};
  uint8_t formatCount = 0;
  uint8_t formatLengths[MAX_UNIQUE_FORMATS]{};
  char formatTokens[MAX_UNIQUE_FORMATS][MAX_FORMAT_TOKEN_BYTES + 1]{};
  int32_t sourceId = -1, sourceCount = -1;
  Array sourceArray = Array::None;
  std::string sourceName, seriesIndex;
  BookOrbitEntry book;
  BookOrbitSnapshotPage page;
};

// Keep three 500-ID buffers and the fixed token/format state (~8 KiB) off the small task stack.
BookOrbitCatalogSnapshotParser::BookOrbitCatalogSnapshotParser(const BookOrbitSnapshotCallbacks& callbacks)
    : impl(makeUniqueNoThrow<Impl>(callbacks)) {
#if defined(ARDUINO)
  if (!impl) LOG_ERR("BOCat", "OOM: snapshot parser state (~8 KiB)");
#endif
}
BookOrbitCatalogSnapshotParser::~BookOrbitCatalogSnapshotParser() = default;
void BookOrbitCatalogSnapshotParser::feed(const char* data, size_t length) {
  if (!impl || impl->error) return;
  impl->parser.feed(data, length);
  if (impl->parser.hasError()) impl->error = true;
}
bool BookOrbitCatalogSnapshotParser::hasError() const { return !impl || impl->error || impl->parser.hasError(); }
bool BookOrbitCatalogSnapshotParser::finish(BookOrbitSnapshotPage& out) {
  if (!impl || impl->error || !impl->parser.finish()) return false;
  if (!impl->rootClosed || !impl->versionSeen || impl->page.version != 1 || !impl->generationSeen ||
      impl->page.generation.empty() || !impl->etagSeen || impl->page.etag.empty() || !impl->totalSeen ||
      impl->page.total < 0 || !impl->cursorSeen || !impl->booksSeen || !impl->librariesSeen || !impl->collectionsSeen ||
      !impl->smartScopesSeen || !impl->continueSeen || !impl->discoverSeen)
    return false;
  out = std::move(impl->page);
  return true;
}
