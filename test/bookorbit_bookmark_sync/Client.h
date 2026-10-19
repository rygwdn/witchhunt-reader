#pragma once

#include <cstddef>
#include <cstdint>

#include <Print.h>

class IPAddress {
 public:
  uint8_t operator[](const size_t index) const { return octets_[index]; }

 private:
  uint8_t octets_[4]{};
};

class Client : public Print {
 public:
  ~Client() override = default;
  virtual void setTimeout(uint32_t) {}
  virtual int connect(IPAddress ip, uint16_t port) = 0;
  virtual int connect(const char* host, uint16_t port) = 0;
  virtual uint8_t connected() = 0;
  virtual void stop() = 0;
  virtual int available() = 0;
  virtual int read() = 0;
  virtual int read(uint8_t* buffer, size_t size) = 0;
  virtual int peek() = 0;
  virtual operator bool() { return connected() != 0; }
};
