#pragma once

#include "KOReaderAutoSync.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <cstddef>
#include <cstdint>
#include <string>

#include "KOReaderSyncClient.h"
#include <BookOrbitBookmarkTypes.h>


struct KOReaderSyncJob {
  enum Kind { FETCH_PROGRESS, SILENT_PUSH, BOOKMARK_CONTINUE };
  Kind kind = FETCH_PROGRESS;

  std::string primaryHash;
  std::string bookOrbitHash;
  std::string altHash;
  bool smartProbe = false;

  KOReaderProgress progress;
  std::string path;
  int spine = -1;
  int page = -1;
  // Baseline used to retain turns accumulated while an async push is running.
  int32_t turnsSincePushAtStart = -1;

  unsigned long connectTimeoutMs = 0;

  enum AttemptOutcome { COMPLETED, DEFERRED, CANCELLED };
  AttemptOutcome attemptOutcome = COMPLETED;
  int httpStatus = 0;
  int transportError = 0;

  KOReaderSyncClient::Error result = KOReaderSyncClient::OK;
  KOReaderProgress remote;
  KOReaderSyncClient::Error altResult = KOReaderSyncClient::NOT_FOUND;
  KOReaderProgress altRemote;

  bool hasBookmarkExchange = false;
  bool hasBookmarkAck = false;
  bool bookmarkResponseReady = false;
  bool bookmarkSucceeded = false;
  BookOrbitBookmarkExchangeBook bookmarkExchange;
  BookOrbitBookmarkExchangeResult bookmarkResponse;
  BookOrbitBookmarkAckBook bookmarkAck;
  uint32_t bookmarkRevision = 0;
  size_t bookmarkBatchSize = 0;
  size_t bookmarkChangeOffset = 0;
  size_t bookmarkTotalChanges = 0;
};

namespace KOReaderSyncWorker {
// Internal DRAM. Peak use measured at 5628 bytes on an x4 pro
constexpr uint32_t WORKER_STACK_BYTES = 10240;

bool isBusy();

bool post(KOReaderSyncJob&& job, uint64_t* seqOut = nullptr);

bool isDone(uint64_t seq);

bool waitFor(uint64_t seq, unsigned long timeoutMs);

KOReaderSyncJob consume(uint64_t seq);

bool drain(unsigned long timeoutMs);

}  // namespace KOReaderSyncWorker

#endif
