#include "OpdsJsonParser.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <utility>

OpdsJsonParser::OpdsJsonParser()
    : parser(JsonCallbacks{this, key, string, number, nullptr, nullptr, objectStart, objectEnd, arrayStart, arrayEnd}) {
}

OpdsJsonParser::Context OpdsJsonParser::current() const { return depth ? frames[depth - 1].context : Context::Ignore; }
void OpdsJsonParser::feed(const uint8_t* data, size_t size) { parser.feed(reinterpret_cast<const char*>(data), size); }
bool OpdsJsonParser::finish() const { return completed && !failed && !parser.hasError() && sawCollection; }
void OpdsJsonParser::key(void* p, const char* value, size_t size) {
  auto& self = *static_cast<OpdsJsonParser*>(p);
  if (self.depth) self.frames[self.depth - 1].key.assign(size <= 64 ? value : "", size <= 64 ? size : 0);
}
void OpdsJsonParser::string(void* p, const char* value, size_t size) {
  static_cast<OpdsJsonParser*>(p)->value(value, size);
}
void OpdsJsonParser::number(void* p, const char* value, size_t size) {
  auto& self = *static_cast<OpdsJsonParser*>(p);
  if (self.current() == Context::Metadata &&
      self.frames[self.depth - 1].key == "https://bookorbit.app/ns/readingProgress") {
    const float progress = strtof(value, nullptr);
    if (std::isfinite(progress) && progress >= 0 && progress <= 1)
      self.entry.progressPercent = static_cast<int>(progress * 100 + 0.5f);
  }
  if (self.current() == Context::Series && self.frames[self.depth - 1].key == "position")
    self.seriesPosition.assign(value, size);
}
void OpdsJsonParser::objectStart(void* p) { static_cast<OpdsJsonParser*>(p)->start(false); }
void OpdsJsonParser::objectEnd(void* p) { static_cast<OpdsJsonParser*>(p)->end(false); }
void OpdsJsonParser::arrayStart(void* p) { static_cast<OpdsJsonParser*>(p)->start(true); }
void OpdsJsonParser::arrayEnd(void* p) { static_cast<OpdsJsonParser*>(p)->end(true); }

void OpdsJsonParser::start(bool array) {
  if (failed || depth >= frames.size() || completed) {
    failed = true;
    return;
  }
  Context next = Context::Ignore;
  const auto parent = current();
  const std::string& name = depth ? frames[depth - 1].key : linkTitle;
  if (!depth) {
    if (array) {
      failed = true;
      return;
    }
    next = Context::Root;
  } else if ((parent == Context::Root || parent == Context::Group) && array) {
    if (name == "facets")
      next = Context::Facets;
    else if (name == "groups")
      next = Context::Groups;
    else if (name == "publications")
      next = Context::Publications;
    else if (name == "navigation")
      next = Context::Navigation;
    else if (name == "links")
      next = Context::Links;
    if (next == Context::Publications || next == Context::Navigation || next == Context::Groups) sawCollection = true;
  } else if (parent == Context::Facets && !array)
    next = Context::Facet;
  else if (parent == Context::Facet && name == "links" && array)
    next = Context::FacetLinks;
  else if (parent == Context::Groups && !array) {
    next = Context::Group;
    groupTitle.clear();
  } else if (parent == Context::Publications && !array) {
    next = Context::Publication;
    entry = {};
    seriesPosition.clear();
    inPublication = true;
    publicationBytes = 0;
    entry.type = OpdsEntryType::BOOK;
    entry.groupTitle = groupTitle;
    entry.acquisitionLinks.reserve(4);
  } else if (name == "metadata" && !array) {
    if (parent == Context::Publication)
      next = Context::Metadata;
    else if (parent == Context::Group)
      next = Context::GroupMetadata;
  } else if (parent == Context::Publication && array) {
    if (name == "links")
      next = Context::Links;
    else if (name == "images")
      next = Context::Images;
  } else if (parent == Context::Metadata) {
    if (name == "author")
      next = array ? Context::Authors : Context::Author;
    else if (name == "belongsTo" && !array)
      next = Context::BelongsTo;
  } else if (parent == Context::Authors && !array)
    next = Context::Author;
  else if (parent == Context::BelongsTo && name == "series")
    next = array ? Context::SeriesArray : Context::Series;
  else if (parent == Context::SeriesArray && !array)
    next = Context::Series;
  else if (parent == Context::Series && name == "links" && array)
    next = Context::SeriesLinks;
  else if ((parent == Context::Links || parent == Context::Navigation || parent == Context::SeriesLinks ||
            parent == Context::FacetLinks) &&
           !array) {
    next = Context::Link;
    linkOwner = depth > 1 ? frames[depth - 2].context : Context::Root;
    navigationLink = parent == Context::Navigation || parent == Context::FacetLinks;
    linkHref.clear();
    linkType.clear();
    linkTitle.clear();
    fileProgressionHref.clear();
    fileStatsHref.clear();
    linkAcquisition = linkProgression = linkStats = linkNext = linkPrevious = linkSearch = false;
  } else if (parent == Context::Images && !array) {
    next = Context::Image;
    linkHref.clear();
    linkType.clear();
  } else if (parent == Context::Link && name == "properties" && !array)
    next = Context::LinkProperties;
  else if (parent == Context::Link && name == "rel" && array)
    next = Context::Link;
  frames[depth].context = next;
  frames[depth].key.clear();
  ++depth;
}

void OpdsJsonParser::end(bool array) {
  if (!depth) {
    failed = true;
    return;
  }
  const auto context = current();
  if (!array && context == Context::Publication) {
    inPublication = false;
    if (!entry.title.empty() && !entry.acquisitionLinks.empty() && onEntryParsed) onEntryParsed(std::move(entry));
  } else if (!array && context == Context::Link)
    commitLink();
  else if (!array && context == Context::Image && entry.imageHref.empty() &&
           (linkType == "image/jpeg" || linkType == "image/png"))
    entry.imageHref = linkHref;
  else if (!array && context == Context::Series) {
    if (!entry.seriesTitle.empty() && !seriesPosition.empty()) {
      entry.seriesTitle += " #";
      entry.seriesTitle += seriesPosition;
    }
    seriesPosition.clear();
  }
  if (!array && context == Context::Group) groupTitle.clear();
  --depth;
  if (!depth) completed = true;
}

void OpdsJsonParser::relation(const std::string& value) {
  linkAcquisition |= value == "acquisition" || value == "download" || value == "http://opds-spec.org/acquisition" ||
                     value == "http://opds-spec.org/acquisition/open-access";
  linkProgression |= value == "http://opds-spec.org/progression";
  linkStats |= value == "https://bookorbit.app/rel/reading-stats";
  linkNext |= value == "next";
  linkPrevious |= value == "previous" || value == "prev";
  linkSearch |= value == "search";
}

void OpdsJsonParser::value(const char* value, size_t size) {
  if (!depth || failed) return;
  // Account for all scalar text in a publication before retaining any of it. A
  // bounded parser stack alone would still allow sixteen huge acquisition URLs.
  if (inPublication) {
    publicationBytes += size;
    if (publicationBytes > 24576) {
      failed = true;
      return;
    }
  }
  const auto context = current();
  const auto& name = frames[depth - 1].key;
  std::string text(value, size);
  if (context == Context::Metadata) {
    if (name == "title")
      entry.title = text;
    else if (name == "identifier")
      entry.id = text;
    else if (name == "description")
      entry.summary = text;
    else if (name == "author")
      entry.author = text;
  } else if (context == Context::Authors || (context == Context::Author && name == "name")) {
    if (entry.author.size() + size < 1024) {
      if (!entry.author.empty()) entry.author += ", ";
      entry.author += text;
    }
  } else if (context == Context::GroupMetadata && name == "title")
    groupTitle = text;
  else if (context == Context::Series && name == "name")
    entry.seriesTitle = text;
  else if (context == Context::Link) {
    if (name == "href")
      linkHref = text;
    else if (name == "type")
      linkType = text;
    else if (name == "title")
      linkTitle = text;
    else if (name == "rel" || name.empty())
      relation(text);
  } else if (context == Context::LinkProperties && name == "https://bookorbit.app/ns/progression")
    fileProgressionHref = text;
  else if (context == Context::LinkProperties && name == "https://bookorbit.app/ns/reading-stats")
    fileStatsHref = text;
  else if (context == Context::Image) {
    if (name == "href")
      linkHref = text;
    else if (name == "type")
      linkType = text;
  }
}

void OpdsJsonParser::commitLink() {
  if (linkHref.empty()) return;
  if (navigationLink && !linkTitle.empty() && (linkType == "application/opds+json" || linkType.empty())) {
    OpdsEntry navigation;
    navigation.title = linkTitle;
    navigation.href = linkHref;
    navigation.groupTitle = groupTitle;
    if (onEntryParsed) onEntryParsed(std::move(navigation));
  } else if (linkOwner == Context::Publication) {
    if (linkProgression) entry.progressionHref = linkHref;
    if (linkStats && linkType == "application/vnd.bookorbit.reading-stats+json") entry.statsHref = linkHref;
    if (linkAcquisition && entry.acquisitionLinks.size() < 16) {
      OpdsAcquisitionLink link;
      link.href = linkHref;
      link.mimeType = linkType;
      link.progressionHref = fileProgressionHref;
      link.statsHref = fileStatsHref;
      if (linkType == "application/epub+zip") {
        link.formatKey = "epub";
        link.fileExtension = ".epub";
      } else if (linkType == "text/plain") {
        link.formatKey = "txt";
        link.fileExtension = ".txt";
      } else if (linkType == "text/markdown" || linkType == "text/x-markdown") {
        link.formatKey = "md";
        link.fileExtension = ".md";
      }
      const auto cleanHref = linkHref.substr(0, linkHref.find_first_of("?#"));
      if (linkType == "application/vnd.xteink.xtc" || cleanHref.ends_with(".xtc")) {
        link.formatKey = "xtc";
        link.fileExtension = ".xtc";
      }
      if (linkType == "application/vnd.xteink.xtch" || cleanHref.ends_with(".xth") || cleanHref.ends_with(".xtch")) {
        link.formatKey = "xtch";
        link.fileExtension = ".xtch";
      }
      if (!link.formatKey.empty()) {
        if (entry.href.empty()) entry.href = linkHref;
        entry.acquisitionLinks.push_back(std::move(link));
      }
    }
  } else if (linkOwner == Context::Series)
    entry.seriesHref = linkHref;
  else if (linkOwner == Context::Root) {
    if (linkNext) nextPageUrl = linkHref;
    if (linkPrevious) prevPageUrl = linkHref;
    if (linkSearch && linkType == "application/opds+json") searchTemplate = linkHref;
  }
}
