#pragma once

struct BookmarkWorkerTestSettings {
  bool skipHttpsValidation = false;
  const char* ntpServer = nullptr;
};

extern BookmarkWorkerTestSettings SETTINGS;
