#pragma once
#include <StreamingJsonParser.h>

#include <array>

// Strict bounded JSON tokenizer. Unlike the release-metadata parser, catalog text
// needs decoded Unicode and truncated URLs must fail instead of becoming links.
class OpdsJsonTokenizer {
 public:
  explicit OpdsJsonTokenizer(JsonCallbacks callbacks) : cb(callbacks) {}
  void feed(const char* data, size_t length);
  bool hasError() const { return error || !complete || state != State::Scan; }

 private:
  enum class State { Scan, String, Escape, Unicode, LowSlash, LowU, Number, Literal };
  struct Frame {
    bool array;
    unsigned phase;
    bool text = false;
  };
  JsonCallbacks cb;
  std::array<Frame, 32> frames{};
  std::array<char, 4096> token{};
  size_t depth = 0, length = 0;
  State state = State::Scan;
  bool error = false, complete = false, started = false, key = false;
  bool truncateText = false, truncated = false;
  unsigned code = 0, digits = 0, high = 0, literalPosition = 0;
  const char* literal = nullptr;
  bool beginValue();
  void scan(char c);
  void append(char c);
  void unicode(unsigned point);
  void number();
};
