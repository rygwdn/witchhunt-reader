#pragma once
using esp_err_t = int;
constexpr int ESP_OK = 0;
constexpr int WIFI_PS_NONE = 0;
constexpr int WIFI_PS_MIN_MODEM = 1;
inline int esp_wifi_set_ps(int) { return ESP_OK; }
