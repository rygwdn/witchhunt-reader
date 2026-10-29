#include "OpdsJsonTokenizer.h"

#include <cctype>
#include <cstring>

void OpdsJsonTokenizer::append(char c) {
  if (length >= token.size() - 1) {
    if (truncateText)
      truncated = true;
    else
      error = true;
    return;
  }
  token[length++] = c;
}

bool OpdsJsonTokenizer::beginValue() {
  if (!depth) {
    if (started) {
      error = true;
      return false;
    }
    started = true;
    return true;
  }
  auto& frame = frames[depth - 1];
  if (frame.array ? frame.phase != 0 && frame.phase != 2 : frame.phase != 2) {
    error = true;
    return false;
  }
  frame.phase = frame.array ? 1 : 3;
  return true;
}

void OpdsJsonTokenizer::scan(char c) {
  if (c == ' ' || c == '\n' || c == '\r' || c == '\t') return;
  if (complete) {
    error = true;
    return;
  }
  if (c == '{' || c == '[') {
    if (!beginValue()) return;
    if (depth == frames.size()) {
      error = true;
      return;
    }
    frames[depth++] = {c == '[', 0};
    if (c == '{') {
      if (cb.onObjectStart) cb.onObjectStart(cb.ctx);
    } else if (cb.onArrayStart)
      cb.onArrayStart(cb.ctx);
  } else if (c == '}' || c == ']') {
    if (!depth) {
      error = true;
      return;
    }
    const auto frame = frames[depth - 1];
    if (frame.array != (c == ']') ||
        (frame.array ? frame.phase != 0 && frame.phase != 1 : frame.phase != 0 && frame.phase != 3)) {
      error = true;
      return;
    }
    if (c == '}') {
      if (cb.onObjectEnd) cb.onObjectEnd(cb.ctx);
    } else if (cb.onArrayEnd)
      cb.onArrayEnd(cb.ctx);
    if (--depth == 0) complete = true;
  } else if (c == ',') {
    if (!depth) {
      error = true;
      return;
    }
    auto& frame = frames[depth - 1];
    if (frame.array ? frame.phase != 1 : frame.phase != 3) {
      error = true;
      return;
    }
    frame.phase = frame.array ? 2 : 4;
  } else if (c == ':') {
    if (!depth || frames[depth - 1].array || frames[depth - 1].phase != 1) {
      error = true;
      return;
    }
    frames[depth - 1].phase = 2;
  } else if (c == '"') {
    key = depth && !frames[depth - 1].array && (frames[depth - 1].phase == 0 || frames[depth - 1].phase == 4);
    if (!key && !beginValue()) return;
    truncateText = !key && depth && frames[depth - 1].text;
    truncated = false;
    length = 0;
    state = State::String;
  } else if (c == '-' || (c >= '0' && c <= '9')) {
    if (!beginValue()) return;
    length = 0;
    append(c);
    state = State::Number;
  } else if (c == 't' || c == 'f' || c == 'n') {
    if (!beginValue()) return;
    literal = c == 't' ? "true" : c == 'f' ? "false" : "null";
    literalPosition = 1;
    state = State::Literal;
  } else
    error = true;
}

void OpdsJsonTokenizer::unicode(unsigned point) {
  if (point <= 0x7f)
    append(static_cast<char>(point));
  else if (point <= 0x7ff) {
    append(0xc0 | (point >> 6));
    append(0x80 | (point & 63));
  } else if (point <= 0xffff) {
    append(0xe0 | (point >> 12));
    append(0x80 | ((point >> 6) & 63));
    append(0x80 | (point & 63));
  } else {
    append(0xf0 | (point >> 18));
    append(0x80 | ((point >> 12) & 63));
    append(0x80 | ((point >> 6) & 63));
    append(0x80 | (point & 63));
  }
}

void OpdsJsonTokenizer::number() {
  size_t i = token[0] == '-' ? 1 : 0;
  const auto digit = [&](size_t at) { return at < length && token[at] >= '0' && token[at] <= '9'; };
  if (!digit(i)) {
    error = true;
    return;
  }
  if (token[i] == '0')
    ++i;
  else
    while (digit(i)) ++i;
  if (i < length && token[i] == '.') {
    ++i;
    if (!digit(i)) {
      error = true;
      return;
    }
    while (digit(i)) ++i;
  }
  if (i < length && (token[i] == 'e' || token[i] == 'E')) {
    ++i;
    if (i < length && (token[i] == '+' || token[i] == '-')) ++i;
    if (!digit(i)) {
      error = true;
      return;
    }
    while (digit(i)) ++i;
  }
  if (i != length) {
    error = true;
    return;
  }
  token[length] = 0;
  if (cb.onNumber) cb.onNumber(cb.ctx, token.data(), length);
}

void OpdsJsonTokenizer::feed(const char* data, size_t count) {
  for (size_t i = 0; i < count && !error; ++i) {
    const char c = data[i];
    switch (state) {
      case State::Scan:
        scan(c);
        break;
      case State::String:
        if (static_cast<unsigned char>(c) < 32) {
          error = true;
          break;
        }
        if (c == '\\')
          state = State::Escape;
        else if (c == '"') {
          if (truncated) {
            size_t boundary = length;
            while (boundary && (static_cast<unsigned char>(token[boundary - 1]) & 0xc0) == 0x80) --boundary;
            if (boundary && static_cast<unsigned char>(token[boundary - 1]) >= 0xc0) {
              // A lead byte ends the kept prefix. Count the continuation bytes that
              // followed it: a complete trailing character survives the cap; only a
              // partial sequence is dropped.
              const unsigned char lead = static_cast<unsigned char>(token[boundary - 1]);
              const size_t expected = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2 : 1;
              if (length - boundary < expected) length = boundary - 1;
            }
          }
          token[length] = 0;
          if (key) {
            auto& frame = frames[depth - 1];
            frame.phase = 1;
            frame.text = strcmp(token.data(), "description") == 0 || strcmp(token.data(), "title") == 0 ||
                         strcmp(token.data(), "name") == 0;
            if (cb.onKey) cb.onKey(cb.ctx, token.data(), length);
          } else if (cb.onString)
            cb.onString(cb.ctx, token.data(), length);
          state = State::Scan;
        } else
          append(c);
        break;
      case State::Escape:
        state = State::String;
        if (c == 'u') {
          code = digits = 0;
          state = State::Unicode;
        } else if (c == '"' || c == '\\' || c == '/')
          append(c);
        else if (c == 'b')
          append('\b');
        else if (c == 'f')
          append('\f');
        else if (c == 'n')
          append('\n');
        else if (c == 'r')
          append('\r');
        else if (c == 't')
          append('\t');
        else
          error = true;
        break;
      case State::Unicode: {
        unsigned value;
        if (c >= '0' && c <= '9')
          value = c - '0';
        else if (c >= 'a' && c <= 'f')
          value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
          value = c - 'A' + 10;
        else {
          error = true;
          break;
        }
        code = (code << 4) | value;
        if (++digits < 4) break;
        if (high) {
          if (code < 0xdc00 || code > 0xdfff) {
            error = true;
            break;
          }
          unicode(0x10000 + ((high - 0xd800) << 10) + code - 0xdc00);
          high = 0;
          state = State::String;
        } else if (code >= 0xd800 && code <= 0xdbff) {
          high = code;
          state = State::LowSlash;
        } else if ((code >= 0xdc00 && code <= 0xdfff) || code == 0)
          error = true;
        else {
          unicode(code);
          state = State::String;
        }
        break;
      }
      case State::LowSlash:
        if (c != '\\')
          error = true;
        else
          state = State::LowU;
        break;
      case State::LowU:
        if (c != 'u')
          error = true;
        else {
          code = digits = 0;
          state = State::Unicode;
        }
        break;
      case State::Number:
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
          append(c);
        else {
          number();
          state = State::Scan;
          if (!error) scan(c);
        }
        break;
      case State::Literal:
        if (c != literal[literalPosition++]) {
          error = true;
          break;
        }
        if (!literal[literalPosition]) {
          if (literal[0] == 'n') {
            if (cb.onNull) cb.onNull(cb.ctx);
          } else if (cb.onBool)
            cb.onBool(cb.ctx, literal[0] == 't');
          state = State::Scan;
        }
        break;
    }
  }
}
