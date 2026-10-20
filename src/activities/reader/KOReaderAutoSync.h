#pragma once

#include <BoardConfig.h>

#if FREEINK_MCU_S3 && defined(BOARD_HAS_PSRAM)
#define CROSSPOINT_KOREADER_AUTOSYNC 1
#else
#define CROSSPOINT_KOREADER_AUTOSYNC 0
#endif

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <cstdint>
#include <string>

#include "KOReaderCredentialStore.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

class AutoSyncState {
  static AutoSyncState instance;

 public:
  // Which stage of the automatic sync the recorded failure happened in. Kept small and
  // persisted so the reader menu's Sync tab can say why the last sync failed, not just that
  // it did — surviving a reboot, unlike KOReaderSyncClient::lastFailureDetail().
  enum class FailureStage : uint8_t {
    None = 0,
    Pull,            // wake pull (GET progress)
    Push,            // silent push (PUT progress)
    PushDeferred,    // sleep push never got to run (worker busy / join deadline)
    Bookmark,        // bookmark exchange or acknowledgment
    BookmarkApply,   // exchange succeeded but applying the response locally failed
    ManualPreFlight  // manual sync's warmup GET failed before the real operation
  };

  static AutoSyncState& getInstance() { return instance; }

  bool saveToFile();
  bool loadFromFile();

  void flush();

  bool isPending() const { return pending; }

  bool isPositionSynced(const std::string& bookPath, int spine, int page) const;

  void noteProgress(const std::string& bookPath, int spine, int page);

  // A nonnegative snapshot preserves turns accrued while an async push was running.
  void markSynced(const std::string& bookPath, int spine, int page, int32_t turnsAtPushStart = -1);

  // Records the last failure with its stage and, when the failure came from a KOReader sync
  // request, the client error and HTTP status. Errors from other stages (bookmark exchange,
  // apply, deferrals) pass Error 0 (OK) meaning "not a KOReader client error", rendered as
  // a stage-level description by the menu.
  void markSyncFailed(FailureStage stage, KOReaderSyncClient::Error error = KOReaderSyncClient::OK, int httpStatus = 0);
  bool lastSyncFailed() const { return lastSyncFailedFlag; }
  FailureStage lastFailureStage() const { return static_cast<FailureStage>(lastFailureStage_); }
  KOReaderSyncClient::Error lastFailureError() const {
    return static_cast<KOReaderSyncClient::Error>(lastFailureError_);
  }
  int lastFailureHttpStatus() const { return lastFailureHttpStatus_; }

  int32_t getTurnsSincePush() const { return turnsSincePush; }
  bool lastNoteWasTurn() const { return lastNoteWasTurnFlag; }

  void seedLastSeen(int spine, int page);

 private:
  std::string lastDocPath;
  int32_t lastPushedSpine = -1;
  int32_t lastPushedPage = -1;
  bool pending = false;
  bool lastSyncFailedFlag = false;
  uint8_t lastFailureStage_ = static_cast<uint8_t>(FailureStage::None);
  uint8_t lastFailureError_ = static_cast<uint8_t>(KOReaderSyncClient::OK);
  int16_t lastFailureHttpStatus_ = 0;

  int32_t lastSeenSpine = -2;
  int32_t lastSeenPage = -2;
  int32_t turnsSincePush = 0;
  bool lastNoteWasTurnFlag = false;
  bool dirty = false;

  AutoSyncState() = default;

 public:
  AutoSyncState(const AutoSyncState&) = delete;
  AutoSyncState& operator=(const AutoSyncState&) = delete;
};

#define AUTOSYNC_STATE AutoSyncState::getInstance()
struct KOReaderSyncJob;

class KOReaderAutoSync {
 public:
  static constexpr unsigned long SLEEP_PUSH_CONNECT_TIMEOUT_MS = 6000;
  static constexpr unsigned long SLEEP_PUSH_JOIN_TIMEOUT_MS = 15000;

  static constexpr unsigned long MIN_INTERVAL_PUSH_GAP_MS = 60 * 1000;

  static bool jobActive();

  static bool heapAllowsBackgroundSession(const char* what);

  static DocumentMatchMethod effectiveMatchMethod(const std::string& epubPath);
  static std::string documentHashFor(const std::string& epubPath);

  static bool pullEnabled();
  static bool sleepPushEnabled();
  static bool intervalPushEnabled();

  static KOReaderSyncJob makeSilentPushJob(const std::string& epubPath, const KOReaderPosition& localKoPos, int spine,
                                           int page, const std::string& title = "", const std::string& authors = "",
                                           const std::string& bookOrbitHash = "");
  static bool automaticAttemptAllowed();
  static void resetRetryPolicy();
  static void recordAutomaticResult(const KOReaderSyncJob& job);
  static uint64_t pushSilentAsync(KOReaderSyncJob&& job);
  static bool finalizeSilentPush(const KOReaderSyncJob& job);
  static bool finalizeSilentPush(uint64_t seq);

  static bool stashSleepPush(KOReaderSyncJob&& job);
  static uint64_t pushStashedSleepPushAsync();
  static bool sleepPushPending(uint64_t seq);
  static void joinStashedSleepPush(uint64_t seq);
};

#endif
