#pragma once

#include <cstddef>
#include <cstdint>

// Ported from crosspoint-reader/crosspoint-reader (MIT),
// initially authored in PR #1810 by znelson and contributors.
struct JsonCallbacks {
  void* ctx;
  void (*onKey)(void* ctx, const char* key, size_t len);
  void (*onString)(void* ctx, const char* value, size_t len);
  void (*onNumber)(void* ctx, const char* value, size_t len);
  void (*onBool)(void* ctx, bool value);
  void (*onNull)(void* ctx);
  void (*onObjectStart)(void* ctx);
  void (*onObjectEnd)(void* ctx);
  void (*onArrayStart)(void* ctx);
  void (*onArrayEnd)(void* ctx);
  void (*onStringPart)(void* ctx, const char* value, size_t len, bool first, bool last) = nullptr;
  bool (*onStringStart)(void* ctx) = nullptr;
  bool (*onStringChunk)(void* ctx, const char* value, size_t len) = nullptr;
  bool (*onStringEnd)(void* ctx) = nullptr;
  // Optional. A string value longer than the token buffer is otherwise dropped whole, unseen. This
  // hands over its first TOKEN_BUF_SIZE - 1 bytes the moment the buffer fills, so a caller after
  // only the start of a long text (release notes) has it without reading on to the end. onString
  // is then not called for that value. An overlong key is still just dropped. Kept last so adding
  // the prefix callback does not shift existing positional custom-callback initializers.
  void (*onStringOverflow)(void* ctx, const char* prefix, size_t len) = nullptr;
};

class StreamingJsonParser {
 public:
  static constexpr size_t TOKEN_BUF_SIZE = 512;
  static constexpr size_t MAX_NESTING = 32;

  explicit StreamingJsonParser(const JsonCallbacks& callbacks);
  void reset();
  void feed(const char* data, size_t len);
  bool finish();
  bool hasError() const { return error; }

 private:
  enum class State : uint8_t {
    SCANNING,
    IN_STRING_KEY,
    IN_STRING_VALUE,
    IN_UNICODE_ESCAPE,
    IN_NUMBER,
    IN_LITERAL,
    SKIP_STRING,
  };
  enum class Container : uint8_t {
    NONE,
    OBJECT,
    ARRAY,
  };

  void handleScanning(char c);
  void handleStringChar(char c);
  void handleNumber(char c);
  void handleLiteral(char c);
  void handleSkipString(char c);
  void handleUnicodeEscape(char c);
  void appendCodepoint(uint32_t codepoint);
  bool flushStream();
  void appendToken(char c);
  void emitToken();
  bool inArray() const { return nestingDepth > 0 && nestingStack[nestingDepth - 1] == Container::ARRAY; }

  JsonCallbacks cb;
  char tokenBuf[TOKEN_BUF_SIZE];
  size_t tokenLen;
  State state;
  bool expectingValue;
  bool escaped;
  bool tokenOverflow;
  bool longString;
  bool error;
  char streamBuf[64];
  size_t streamLen;
  bool streamStringActive;
  State unicodeReturnState;
  uint16_t unicodeValue;
  uint8_t unicodeDigits;
  uint16_t pendingHighSurrogate;

  Container nestingStack[MAX_NESTING];
  uint8_t nestingDepth;
  bool rootSeen;
  bool rootComplete;

  char literalExpected[6];
  uint8_t literalLen;
  uint8_t literalPos;
};
