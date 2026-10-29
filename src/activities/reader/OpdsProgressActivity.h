#pragma once
#include "activities/Activity.h"
#include "network/OpdsProgressSync.h"

class OpdsProgressActivity final : public Activity {
 public:
  OpdsProgressActivity(GfxRenderer& renderer, MappedInputManager& input, std::string path, bool upload = false,
                       const OpdsProgressSync::Remote* cached = nullptr)
      : Activity("OpdsProgress", renderer, input), path(std::move(path)), upload(upload), cached(cached != nullptr) {
    if (cached) remote = *cached;
  }
  void onEnter() override;
  void onExit() override;
  bool preventAutoSleep() override { return !failed; }
  bool usesWifi() const override { return true; }
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string path;
  OpdsProgressSync::Remote remote;
  bool ready = false, failed = false, trimmed = false;
  bool upload = false, cached = false, startedRadio = false;
};
