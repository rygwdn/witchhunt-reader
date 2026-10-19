#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <Client.h>

class ScriptedWiFiTransport {
 public:
  struct Exchange {
    std::string response;
    std::string request;
  };

  static void reset() {
    exchanges().clear();
    nextExchange() = 0;
  }

  static void enqueue(std::string response) { exchanges().push_back({std::move(response), {}}); }
  static const std::vector<Exchange>& history() { return exchanges(); }

 private:
  friend class WiFiClient;
  static std::vector<Exchange>& exchanges() {
    static std::vector<Exchange> value;
    return value;
  }
  static size_t& nextExchange() {
    static size_t value = 0;
    return value;
  }
};

class WiFiClient : public Client {
 public:
  int connect(IPAddress, uint16_t) override { return 0; }

  int connect(const char*, uint16_t) override {
    if (ScriptedWiFiTransport::nextExchange() >= ScriptedWiFiTransport::exchanges().size()) return 0;
    exchangeIndex_ = ScriptedWiFiTransport::nextExchange()++;
    connected_ = true;
    return 1;
  }

  void setTimeout(uint32_t) override {}
  uint8_t connected() override { return connected_; }
  void stop() override { connected_ = false; }
  int available() override {
    if (!connected_ || exchangeIndex_ >= ScriptedWiFiTransport::exchanges().size()) return 0;
    const auto& response = ScriptedWiFiTransport::exchanges()[exchangeIndex_].response;
    const size_t remaining = response.size() - responseOffset_;
    return static_cast<int>(std::min(remaining, static_cast<size_t>(64)));
  }
  int read() override {
    uint8_t byte = 0;
    return read(&byte, 1) == 1 ? byte : -1;
  }
  int read(uint8_t* buffer, size_t size) override {
    if (!buffer || size == 0 || !connected_ || exchangeIndex_ >= ScriptedWiFiTransport::exchanges().size()) return 0;
    const auto& response = ScriptedWiFiTransport::exchanges()[exchangeIndex_].response;
    const size_t remaining = response.size() - responseOffset_;
    const size_t amount = std::min({size, remaining, static_cast<size_t>(37)});
    if (amount == 0) return 0;
    std::copy_n(reinterpret_cast<const uint8_t*>(response.data() + responseOffset_), amount, buffer);
    responseOffset_ += amount;
    return static_cast<int>(amount);
  }
  int peek() override {
    if (available() == 0) return -1;
    return static_cast<unsigned char>(ScriptedWiFiTransport::exchanges()[exchangeIndex_].response[responseOffset_]);
  }
  void flush() override {}
  size_t write(uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, size_t size) override {
    if (!data || !connected_ || exchangeIndex_ >= ScriptedWiFiTransport::exchanges().size()) return 0;
    ScriptedWiFiTransport::exchanges()[exchangeIndex_].request.append(reinterpret_cast<const char*>(data), size);
    return size;
  }

 private:
  size_t exchangeIndex_ = 0;
  size_t responseOffset_ = 0;
  bool connected_ = false;
};
