#include "OpdsProgressActivity.h"

#include <I18n.h>
#include <WiFi.h>

#include "KOReaderSyncWorker.h"
#include "activities/NetworkMemoryTrim.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void OpdsProgressActivity::onEnter() {
  Activity::onEnter();
#if CROSSPOINT_KOREADER_AUTOSYNC
  if (!KOReaderSyncWorker::drain(10000)) {
    failed = true;
    requestUpdate();
    return;
  }
#endif
  startedRadio = WiFi.getMode() == WIFI_MODE_NULL;
  trimMemoryForNetworkSession(renderer, "OPDS_SYNC");
  trimmed = true;
  if (cached || WiFi.status() == WL_CONNECTED)
    ready = true;
  else
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled || !std::holds_alternative<WifiResult>(result.data) ||
                                 !std::get<WifiResult>(result.data).connected) {
                               ActivityResult cancelled;
                               cancelled.isCancelled = true;
                               setResult(std::move(cancelled));
                               finish();
                             } else
                               ready = true;
                           });
  requestUpdate();
}

void OpdsProgressActivity::onExit() {
  if (upload && startedRadio) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
  }
  if (trimmed) {
    renderer.reallocSecondaryBuffer();
    renderer.setSingleBufferFastDiff(!renderer.hasSecondaryBuffer());
  }
  Activity::onExit();
}

void OpdsProgressActivity::loop() {
  if (failed) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult cancelled;
      cancelled.isCancelled = true;
      setResult(std::move(cancelled));
      finish();
    }
    return;
  }
  if (!ready) return;
  ready = false;
  if (upload) {
    if (!OpdsProgressSync::flushForBook(path)) {
      failed = true;
      requestUpdate();
      return;
    }
    finish();
    return;
  }
  if (!cached && !OpdsProgressSync::fetch(path, remote)) {
    failed = true;
    requestUpdate();
    return;
  }
  char percent[32];
  snprintf(percent, sizeof(percent), "%.2f%%", static_cast<double>(remote.progression * 100));
  const auto body = std::string(percent) + " - " + remote.deviceName + "\n" + remote.modified;
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_OPDS_APPLY_PROGRESS), body),
      [this](const ActivityResult& result) {
        if (result.isCancelled) {
          ActivityResult cancelled;
          cancelled.isCancelled = true;
          setResult(std::move(cancelled));
        } else
          setResult(ActivityResult(OpdsProgressResult{remote.progression, remote.reference}));
        finish();
      });
}

void OpdsProgressActivity::render(RenderLock&&) {
  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 20, (upload ? tr(STR_UPLOAD_LOCAL) : tr(STR_OPDS_PULL_PROGRESS)), true,
                            EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_10_FONT_ID, renderer.getScreenHeight() / 2,
                            failed ? (upload ? tr(STR_SYNC_FAILED_MSG) : tr(STR_OPDS_PROGRESS_UNAVAILABLE))
                                   : (upload ? tr(STR_UPLOAD_PROGRESS) : tr(STR_FETCH_PROGRESS)));
  const auto hints = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}
