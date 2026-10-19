#pragma once

enum wifi_mode_t : int { WIFI_MODE_NULL = 0, WIFI_MODE_STA = 1, WIFI_MODE_AP = 2, WIFI_MODE_APSTA = 3 };
constexpr int WL_CONNECTED = 3;
constexpr wifi_mode_t WIFI_STA = WIFI_MODE_STA;
constexpr wifi_mode_t WIFI_OFF = WIFI_MODE_NULL;

class BookmarkWorkerTestWiFi {
 public:
  int status() const { return connected_ ? WL_CONNECTED : 0; }
  wifi_mode_t getMode() const { return mode_; }
  int RSSI() const { return -42; }
  int getSleep() const { return 0; }
  void setSleep(bool) {}
  void persistent(bool) {}
  bool mode(wifi_mode_t mode) {
    mode_ = mode;
    return true;
  }
  void begin(const char*, const char*) { connected_ = true; }
  void disconnect(bool) {
    ++disconnectCount;
    connected_ = false;
  }

  void setConnected(bool connected) { connected_ = connected; }
  int disconnectCount = 0;

 private:
  wifi_mode_t mode_ = WIFI_MODE_NULL;
  bool connected_ = false;
};

inline BookmarkWorkerTestWiFi WiFi;
