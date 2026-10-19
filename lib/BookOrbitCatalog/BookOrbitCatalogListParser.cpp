#include "BookOrbitCatalogListParser.h"

#include <cstdlib>
#include <cstring>
#include <utility>

namespace {
bool keyIs(const char* key, size_t len, const char* expected, size_t expectedLen) {
  return len == expectedLen && memcmp(key, expected, expectedLen) == 0;
}

// Separator between joined list values (authors, formats). A middle dot reads
// as a list on the panel without the comma ambiguity of "Last, First" author
// names, and matches the OPDS browser's format line.
constexpr char JOIN_SEPARATOR[] = " \xC2\xB7 ";

// Percentages arrive as 0..100, fractional (the server stores a float). Round
// to whole percent: the reader shows one integer and nothing finer is legible.
int16_t toPercent(const char* value) {
  const double v = strtod(value, nullptr);
  if (v <= 0.0) return 0;
  if (v >= 100.0) return 100;
  return static_cast<int16_t>(v + 0.5);
}
}  // namespace

#define KEY_IS(literal) keyIs(key, len, literal, sizeof(literal) - 1)

BookOrbitCatalogListParser::BookOrbitCatalogListParser(EntryCallback onEntry)
    : parser(JsonCallbacks{.ctx = this,
                           .onKey = sOnKey,
                           .onString = sOnString,
                           .onNumber = sOnNumber,
                           .onBool = sOnBool,
                           .onNull = sOnNull,
                           .onObjectStart = sOnObjectStart,
                           .onObjectEnd = sOnObjectEnd,
                           .onArrayStart = sOnArrayStart,
                           .onArrayEnd = sOnArrayEnd}),
      onEntry(std::move(onEntry)) {
  reset();
}

void BookOrbitCatalogListParser::reset() {
  parser.reset();
  depth = 0;
  rowDepth = 0;
  inRowArray = false;
  lastKey = Key::NONE;
  collecting = Key::NONE;
  rowGroup = BookOrbitEntryGroup::STANDARD;
  row = {};
  rowSeriesIndex.clear();
  rowIsBook = false;
  page = {};
  entries = 0;
}

void BookOrbitCatalogListParser::feed(const char* data, size_t len) { parser.feed(data, len); }

void BookOrbitCatalogListParser::appendJoined(std::string& target, const char* value, size_t len) const {
  if (len == 0) return;
  if (!target.empty()) target += JOIN_SEPARATOR;
  target.append(value, len);
}

void BookOrbitCatalogListParser::commitRow() {
  row.group = rowGroup;
  row.type = rowIsBook ? BookOrbitEntryType::BOOK : BookOrbitEntryType::NAVIGATION;

  // Series index is emitted as its own string field ("1", "2.5"), after the
  // name; fold it into one displayable line now that both have been seen.
  if (!row.series.empty() && !rowSeriesIndex.empty()) {
    row.series += " #";
    row.series += rowSeriesIndex;
  }

  // A row with neither a label nor a destination is not something the reader can
  // draw or follow — drop it rather than paint a blank selectable line.
  const bool usable = !row.title.empty() && (rowIsBook || !row.href.empty());
  if (usable && onEntry) {
    onEntry(row);
    ++entries;
  }

  row = {};
  rowSeriesIndex.clear();
  rowIsBook = false;
}

void BookOrbitCatalogListParser::sOnKey(void* ctx, const char* key, size_t len) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);
  self->lastKey = Key::NONE;

  if (self->inRow()) {
    if (KEY_IS("id"))
      self->lastKey = Key::ID;
    else if (KEY_IS("title"))
      self->lastKey = Key::TITLE;
    else if (KEY_IS("subtitle"))
      self->lastKey = Key::SUBTITLE;
    else if (KEY_IS("count"))
      self->lastKey = Key::COUNT;
    else if (KEY_IS("href") || KEY_IS("booksHref"))
      self->lastKey = Key::HREF;
    else if (KEY_IS("detailUrl"))
      self->lastKey = Key::DETAIL_URL;
    else if (KEY_IS("authors"))
      self->lastKey = Key::AUTHORS;
    else if (KEY_IS("formats"))
      self->lastKey = Key::FORMATS;
    else if (KEY_IS("seriesName"))
      self->lastKey = Key::SERIES_NAME;
    else if (KEY_IS("seriesIndex"))
      self->lastKey = Key::SERIES_INDEX;
    else if (KEY_IS("progressPercentage"))
      self->lastKey = Key::PROGRESS;
    else if (KEY_IS("hasCover"))
      self->lastKey = Key::HAS_COVER;
    return;
  }

  if (self->atResponseLevel()) {
    if (KEY_IS("items") || KEY_IS("sections"))
      self->lastKey = Key::ITEMS;
    else if (KEY_IS("continueReading"))
      self->lastKey = Key::CONTINUE_READING;
    else if (KEY_IS("discover"))
      self->lastKey = Key::DISCOVER;
    else if (KEY_IS("total"))
      self->lastKey = Key::TOTAL;
    else if (KEY_IS("page"))
      self->lastKey = Key::PAGE;
    else if (KEY_IS("nextUrl"))
      self->lastKey = Key::NEXT_URL;
    else if (KEY_IS("previousUrl"))
      self->lastKey = Key::PREV_URL;
  }
}

void BookOrbitCatalogListParser::sOnString(void* ctx, const char* value, size_t len) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);

  // Inside an authors/formats array: every element appends to one joined line.
  // lastKey is cleared by the tokenizer's first element, so the open array is
  // tracked by `collecting` instead.
  if (self->rowDepth == 2 && self->collecting != Key::NONE) {
    if (self->collecting == Key::AUTHORS) {
      self->appendJoined(self->row.subtitle, value, len);
    } else {
      self->appendJoined(self->row.formats, value, len);
    }
    return;
  }

  if (self->inRow()) {
    switch (self->lastKey) {
      case Key::TITLE:
        self->row.title.assign(value, len);
        break;
      case Key::SUBTITLE:
        self->row.subtitle.assign(value, len);
        break;
      case Key::HREF:
        self->row.href.assign(value, len);
        break;
      case Key::DETAIL_URL:
        self->row.href.assign(value, len);
        self->rowIsBook = true;
        break;
      case Key::SERIES_NAME:
        self->row.series.assign(value, len);
        break;
      case Key::SERIES_INDEX:
        self->rowSeriesIndex.assign(value, len);
        break;
      default:
        break;
    }
    self->lastKey = Key::NONE;
    return;
  }

  if (self->atResponseLevel()) {
    switch (self->lastKey) {
      case Key::NEXT_URL:
        self->page.nextUrl.assign(value, len);
        break;
      case Key::PREV_URL:
        self->page.prevUrl.assign(value, len);
        break;
      default:
        break;
    }
  }
  self->lastKey = Key::NONE;
}

void BookOrbitCatalogListParser::sOnNumber(void* ctx, const char* value, size_t /*len*/) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);

  if (self->inRow()) {
    switch (self->lastKey) {
      case Key::ID:
        // Only book rows carry a numeric id; navigation ids are strings
        // ("libraries", "series:12") and land in sOnString's default case.
        self->row.bookId = static_cast<int32_t>(strtol(value, nullptr, 10));
        self->rowIsBook = true;
        break;
      case Key::COUNT:
        self->row.count = static_cast<int32_t>(strtol(value, nullptr, 10));
        break;
      case Key::PROGRESS:
        self->row.progressPercent = toPercent(value);
        break;
      case Key::SERIES_INDEX:
        // Tolerate a server that sends the index as a number rather than the
        // documented string.
        self->rowSeriesIndex.assign(value);
        break;
      default:
        break;
    }
  } else if (self->atResponseLevel()) {
    switch (self->lastKey) {
      case Key::TOTAL:
        self->page.total = static_cast<int32_t>(strtol(value, nullptr, 10));
        break;
      case Key::PAGE:
        self->page.page = static_cast<int32_t>(strtol(value, nullptr, 10));
        break;
      default:
        break;
    }
  }
  self->lastKey = Key::NONE;
}

void BookOrbitCatalogListParser::sOnBool(void* ctx, bool value) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);
  if (self->inRow() && self->lastKey == Key::HAS_COVER) {
    self->row.hasCover = value;
  }
  self->lastKey = Key::NONE;
}

void BookOrbitCatalogListParser::sOnNull(void* ctx) {
  static_cast<BookOrbitCatalogListParser*>(ctx)->lastKey = Key::NONE;
}

void BookOrbitCatalogListParser::sOnObjectStart(void* ctx) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);

  if (self->rowDepth > 0) {
    ++self->rowDepth;  // a nested object inside the row: ignored wholesale
  } else if (self->inRowArray) {
    self->rowDepth = 1;
    self->row = {};
    self->row.group = self->rowGroup;
    self->rowSeriesIndex.clear();
    self->rowIsBook = false;
  } else {
    ++self->depth;
  }
  self->lastKey = Key::NONE;
}

void BookOrbitCatalogListParser::sOnObjectEnd(void* ctx) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);

  if (self->rowDepth > 1) {
    --self->rowDepth;
  } else if (self->rowDepth == 1) {
    self->rowDepth = 0;
    self->commitRow();
  } else if (self->depth > 0) {
    --self->depth;
  }
  self->lastKey = Key::NONE;
  self->collecting = Key::NONE;
}

void BookOrbitCatalogListParser::sOnArrayStart(void* ctx) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);

  if (self->rowDepth > 0) {
    // authors/formats are the only arrays the reader reads out of a row; any
    // other (collections, genres, relatedSections) is skipped by depth alone.
    self->collecting = (self->lastKey == Key::AUTHORS || self->lastKey == Key::FORMATS) ? self->lastKey : Key::NONE;
    ++self->rowDepth;
  } else if (!self->inRowArray && self->depth == 1 &&
             (self->lastKey == Key::ITEMS || self->lastKey == Key::CONTINUE_READING ||
              self->lastKey == Key::DISCOVER)) {
    self->inRowArray = true;
    self->rowGroup = self->lastKey == Key::CONTINUE_READING
                         ? BookOrbitEntryGroup::CONTINUE_READING
                         : (self->lastKey == Key::DISCOVER ? BookOrbitEntryGroup::DISCOVER
                                                          : BookOrbitEntryGroup::STANDARD);
  } else {
    ++self->depth;
  }
  self->lastKey = Key::NONE;
}

void BookOrbitCatalogListParser::sOnArrayEnd(void* ctx) {
  auto* self = static_cast<BookOrbitCatalogListParser*>(ctx);

  if (self->rowDepth > 1) {
    --self->rowDepth;
    self->collecting = Key::NONE;
  } else if (self->inRowArray && self->rowDepth == 0) {
    self->inRowArray = false;
    self->rowGroup = BookOrbitEntryGroup::STANDARD;
  } else if (self->depth > 0) {
    --self->depth;
  }
  self->lastKey = Key::NONE;
}

#undef KEY_IS
