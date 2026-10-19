#pragma once
// Host stub: a virtual clock (delay advances it instantly) and the small String/IPAddress surface
// SecureHttpClient touches.
//
// Ported from Free-Ink/freeink-sdk f80a99c (libs/network/SecureNet/test/host/stubs, Justin
// Mitchell).
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

inline unsigned long& fakeNow() {
  static unsigned long now = 0;
  return now;
}
inline unsigned long millis() { return fakeNow(); }
inline void delay(unsigned long ms) { fakeNow() += ms; }

#include <WString.h>
#include <Print.h>

class Stream : public Print {};

class IPAddress {};
