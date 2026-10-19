#include "StreamingJsonParser.h"

#include <cstring>

StreamingJsonParser::StreamingJsonParser(const JsonCallbacks& callbacks) : cb(callbacks) { reset(); }

void StreamingJsonParser::reset() {
  tokenLen = 0;
  state = State::SCANNING;
  expectingValue = false;
  escaped = false;
  tokenOverflow = false;
  longString = false;
  error = false;
  streamLen = 0;
  streamStringActive = false;
  unicodeReturnState = State::SCANNING;
  unicodeValue = 0;
  unicodeDigits = 0;
  pendingHighSurrogate = 0;
  nestingDepth = 0;
  rootSeen = false;
  rootComplete = false;
  literalPos = 0;
}

bool StreamingJsonParser::finish() {
  if (state == State::IN_NUMBER && !tokenOverflow) {
    tokenBuf[tokenLen] = '\0';
    if (cb.onNumber) cb.onNumber(cb.ctx, tokenBuf, tokenLen);
    state = State::SCANNING;
  }
  if (!rootSeen || !rootComplete || nestingDepth != 0 || state != State::SCANNING) error = true;
  return !error;
}

void StreamingJsonParser::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len && !error; ++i) {
    char c = data[i];
    if (rootComplete && c != ' ' && c != '\n' && c != '\r' && c != '\t') {
      error = true;
      break;
    }
    switch (state) {
      case State::SCANNING:
        handleScanning(c);
        break;
      case State::IN_STRING_KEY:
      case State::IN_STRING_VALUE:
        handleStringChar(c);
        break;
      case State::IN_UNICODE_ESCAPE:
        handleUnicodeEscape(c);
        break;
      case State::IN_NUMBER:
        handleNumber(c);
        break;
      case State::IN_LITERAL:
        handleLiteral(c);
        break;
      case State::SKIP_STRING:
        handleSkipString(c);
        break;
    }
  }
}

void StreamingJsonParser::handleScanning(char c) {
  switch (c) {
    case '"':
      tokenLen = 0;
      tokenOverflow = false;
      escaped = false;
      pendingHighSurrogate = 0;
      streamLen = 0;
      streamStringActive = false;
      if (expectingValue || inArray()) {
        state = State::IN_STRING_VALUE;
        streamStringActive = cb.onStringChunk != nullptr;
        if (cb.onStringStart && !cb.onStringStart(cb.ctx)) {
          error = true;
          return;
        }
      } else {
        state = State::IN_STRING_KEY;
      }
      break;
    case '{':
    case '[': {
      if (nestingDepth == 0) {
        if (rootSeen) {
          error = true;
          return;
        }
        rootSeen = true;
      }
      if (nestingDepth >= MAX_NESTING) {
        error = true;
        return;
      }
      nestingStack[nestingDepth++] = c == '{' ? Container::OBJECT : Container::ARRAY;
      if (c == '{') {
        if (cb.onObjectStart) cb.onObjectStart(cb.ctx);
      } else if (cb.onArrayStart)
        cb.onArrayStart(cb.ctx);
      expectingValue = false;
      break;
    }
    case '}':
    case ']': {
      if (nestingDepth == 0 || nestingStack[nestingDepth - 1] != (c == '}' ? Container::OBJECT : Container::ARRAY)) {
        error = true;
        return;
      }
      const Container closed = nestingStack[--nestingDepth];
      if (closed == Container::OBJECT) {
        if (cb.onObjectEnd) cb.onObjectEnd(cb.ctx);
      } else if (cb.onArrayEnd)
        cb.onArrayEnd(cb.ctx);
      if (nestingDepth == 0) rootComplete = true;
      expectingValue = false;
      break;
    }
    case ':':
      expectingValue = true;
      break;
    case ',':
      expectingValue = false;
      break;
    case 't':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "true", 4);
        literalLen = 4;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    case 'f':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "false", 5);
        literalLen = 5;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    case 'n':
      if (expectingValue || inArray()) {
        memcpy(literalExpected, "null", 4);
        literalLen = 4;
        literalPos = 1;
        state = State::IN_LITERAL;
      }
      break;
    default:
      if ((expectingValue || inArray()) && (c == '-' || (c >= '0' && c <= '9'))) {
        tokenLen = 0;
        tokenOverflow = false;
        appendToken(c);
        state = State::IN_NUMBER;
      }
      break;
  }
}

void StreamingJsonParser::handleStringChar(char c) {
  if (pendingHighSurrogate != 0) {
    if (!escaped) {
      if (c != '\\') {
        error = true;
        return;
      }
      escaped = true;
      return;
    }
    if (c != 'u') {
      error = true;
      return;
    }
    escaped = false;
    unicodeReturnState = state;
    unicodeValue = 0;
    unicodeDigits = 0;
    state = State::IN_UNICODE_ESCAPE;
    return;
  }
  if (escaped) {
    escaped = false;
    switch (c) {
      case '"':
      case '\\':
      case '/':
        appendToken(c);
        break;
      case 'b':
        appendToken('\b');
        break;
      case 'f':
        appendToken('\f');
        break;
      case 'n':
        appendToken('\n');
        break;
      case 'r':
        appendToken('\r');
        break;
      case 't':
        appendToken('\t');
        break;
      case 'u':
        unicodeReturnState = state;
        unicodeValue = 0;
        unicodeDigits = 0;
        state = State::IN_UNICODE_ESCAPE;
        break;
      default:
        appendToken('\\');
        appendToken(c);
        break;
    }
    return;
  }

  if (c == '\\') {
    escaped = true;
    return;
  }
  if (c == '"') {
    if (!flushStream()) return;
    if (cb.onStringEnd && state == State::IN_STRING_VALUE && !cb.onStringEnd(cb.ctx)) {
      error = true;
      return;
    }
    streamStringActive = false;
    emitToken();
    return;
  }
  appendToken(c);
}

void StreamingJsonParser::handleNumber(char c) {
  if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E') {
    appendToken(c);
    return;
  }

  if (!tokenOverflow && cb.onNumber) {
    tokenBuf[tokenLen] = '\0';
    cb.onNumber(cb.ctx, tokenBuf, tokenLen);
  }
  state = State::SCANNING;
  expectingValue = false;

  handleScanning(c);
}

void StreamingJsonParser::handleLiteral(char c) {
  if (c == literalExpected[literalPos]) {
    ++literalPos;
    if (literalPos == literalLen) {
      if (literalExpected[0] == 't') {
        if (cb.onBool) cb.onBool(cb.ctx, true);
      } else if (literalExpected[0] == 'f') {
        if (cb.onBool) cb.onBool(cb.ctx, false);
      } else {
        if (cb.onNull) cb.onNull(cb.ctx);
      }
      state = State::SCANNING;
      expectingValue = false;
    }
  } else {
    error = true;
  }
}
void StreamingJsonParser::handleUnicodeEscape(const char c) {
  uint8_t digit;
  if (c >= '0' && c <= '9')
    digit = static_cast<uint8_t>(c - '0');
  else if (c >= 'a' && c <= 'f')
    digit = static_cast<uint8_t>(c - 'a' + 10);
  else if (c >= 'A' && c <= 'F')
    digit = static_cast<uint8_t>(c - 'A' + 10);
  else {
    error = true;
    return;
  }
  unicodeValue = static_cast<uint16_t>((unicodeValue << 4) | digit);
  if (++unicodeDigits < 4) return;
  state = unicodeReturnState;
  if (unicodeValue >= 0xD800 && unicodeValue <= 0xDBFF) {
    if (pendingHighSurrogate != 0)
      error = true;
    else
      pendingHighSurrogate = unicodeValue;
    return;
  }
  if (unicodeValue >= 0xDC00 && unicodeValue <= 0xDFFF) {
    if (pendingHighSurrogate == 0) {
      error = true;
      return;
    }
    const uint32_t codepoint = 0x10000u + ((pendingHighSurrogate - 0xD800u) << 10) + (unicodeValue - 0xDC00u);
    pendingHighSurrogate = 0;
    appendCodepoint(codepoint);
    return;
  }
  if (pendingHighSurrogate != 0) {
    error = true;
    return;
  }
  appendCodepoint(unicodeValue);
}

void StreamingJsonParser::appendCodepoint(const uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    appendToken(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    appendToken(static_cast<char>(0xC0 | (codepoint >> 6)));
    appendToken(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    appendToken(static_cast<char>(0xE0 | (codepoint >> 12)));
    appendToken(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    appendToken(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    appendToken(static_cast<char>(0xF0 | (codepoint >> 18)));
    appendToken(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    appendToken(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    appendToken(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

void StreamingJsonParser::handleSkipString(char c) {
  if (escaped) {
    escaped = false;
    return;
  }
  if (c == '\\') {
    escaped = true;
    return;
  }
  if (c == '"') {
    state = State::SCANNING;
    expectingValue = false;
  }
}

void StreamingJsonParser::appendToken(char c) {
  if (tokenLen < TOKEN_BUF_SIZE - 1) {
    tokenBuf[tokenLen++] = c;
  } else if (state == State::IN_STRING_VALUE && cb.onStringPart != nullptr && !streamStringActive) {
    cb.onStringPart(cb.ctx, tokenBuf, tokenLen, !longString, false);
    longString = true;
    tokenLen = 0;
    tokenBuf[tokenLen++] = c;
  } else {
    if (!tokenOverflow && state == State::IN_STRING_VALUE && cb.onStringOverflow && !streamStringActive) {
      tokenBuf[tokenLen] = '\0';
      cb.onStringOverflow(cb.ctx, tokenBuf, tokenLen);
    }
    tokenOverflow = true;
  }
  if (state == State::IN_STRING_VALUE && streamStringActive) {
    streamBuf[streamLen++] = c;
    if (streamLen == sizeof(streamBuf)) flushStream();
  }
}

bool StreamingJsonParser::flushStream() {
  if (!streamStringActive || streamLen == 0) return true;
  const bool accepted = cb.onStringChunk(cb.ctx, streamBuf, streamLen);
  streamLen = 0;
  if (!accepted) error = true;
  return accepted;
}

void StreamingJsonParser::emitToken() {
  if (state == State::IN_STRING_KEY) {
    if (!tokenOverflow && cb.onKey) {
      tokenBuf[tokenLen] = '\0';
      cb.onKey(cb.ctx, tokenBuf, tokenLen);
    }
    state = State::SCANNING;
  } else {
    if (longString && cb.onStringPart) {
      cb.onStringPart(cb.ctx, tokenBuf, tokenLen, false, true);
    } else if (!tokenOverflow && cb.onString) {
      tokenBuf[tokenLen] = '\0';
      cb.onString(cb.ctx, tokenBuf, tokenLen);
    }
    state = State::SCANNING;
    expectingValue = false;
    longString = false;
  }
}
