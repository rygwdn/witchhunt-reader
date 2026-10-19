#pragma once
#include <string>
struct DownloaderSettings {
  bool skipHttpsValidation = false;
  std::string ntpServer;
};
inline DownloaderSettings downloaderSettings;
#define SETTINGS downloaderSettings
