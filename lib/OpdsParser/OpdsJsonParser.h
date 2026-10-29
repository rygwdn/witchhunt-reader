#pragma once
#include <array>

#include "OpdsJsonTokenizer.h"
#include "OpdsParser.h"

// One publication at a time. Feed bodies remain on SD, never in a JSON DOM.
class OpdsJsonParser {
 public:
  OpdsJsonParser();
  void feed(const uint8_t* data, size_t size);
  bool finish() const;
  const std::string& getSearchTemplate() const { return searchTemplate; }
  const std::string& getNextPageUrl() const { return nextPageUrl; }
  const std::string& getPrevPageUrl() const { return prevPageUrl; }
  std::function<void(OpdsEntry)> onEntryParsed;

 private:
  enum class Context : uint8_t {
    Root,
    Groups,
    Group,
    Publications,
    Publication,
    Metadata,
    GroupMetadata,
    Navigation,
    Link,
    Links,
    Images,
    Image,
    Authors,
    Author,
    BelongsTo,
    Series,
    SeriesArray,
    SeriesLinks,
    Facets,
    Facet,
    FacetLinks,
    Ignore
  };
  struct Frame {
    Context context = Context::Ignore;
    std::string key;
  };
  std::array<Frame, 32> frames;
  size_t depth = 0;
  bool failed = false;
  bool completed = false;
  bool inPublication = false;
  size_t publicationBytes = 0;
  bool sawCollection = false;
  bool linkAcquisition = false;
  bool linkNext = false;
  bool linkPrevious = false;
  bool linkSearch = false;
  bool navigationLink = false;
  Context linkOwner = Context::Ignore;
  OpdsEntry entry;
  std::string groupTitle, searchTemplate, nextPageUrl, prevPageUrl;
  std::string linkHref, linkType, linkTitle;
  // A series name and its position arrive as siblings in either order; the
  // position is held until the series object closes so "Saga" + 2 always
  // composes to "Saga #2".
  std::string seriesPosition;
  OpdsJsonTokenizer parser;
  Context current() const;
  void start(bool array);
  void end(bool array);
  void value(const char* value, size_t size);
  void relation(const std::string& value);
  void commitLink();
  static void key(void*, const char*, size_t);
  static void string(void*, const char*, size_t);
  static void number(void*, const char*, size_t);
  static void objectStart(void*);
  static void objectEnd(void*);
  static void arrayStart(void*);
  static void arrayEnd(void*);
};
