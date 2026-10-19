#include "EpubReaderActivity.h"

#if CROSSPOINT_KOREADER_AUTOSYNC

#include <Arduino.h>
#include <Logging.h>

#include <utility>

#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "BookOrbitBookmarkSync.h"
#include "KOReaderDocumentId.h"
#include "ProgressComparison.h"
#include "ReadingSessionTracker.h"
#include "activities/ActivityManager.h"

namespace {
// The reader's position as ProgressComparison wants it: the page; the paragraph LUT at the end of
// this page and of the one before; and the page's content-offset window [start, end), where end is
// the first later page start greater than this page's (UINT32_MAX on the last page).
LocalReadingPosition localReadingPosition(const Section& section, const int spineIndex, const int page) {
  LocalReadingPosition local;
  local.spineIndex = spineIndex;
  local.page = page;
  local.paragraphAtPageEnd = section.getParagraphIndexForPage(static_cast<uint16_t>(page)).value_or(0);
  if (page > 0) {
    local.paragraphAtPreviousPageEnd = section.getParagraphIndexForPage(static_cast<uint16_t>(page - 1)).value_or(0);
  }
  if (const auto start = section.getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
    local.visibleOffsetAtPage = *start;
    local.visibleOffsetAtNextPage =
        section.getVisibleTextOffsetAfterPage(static_cast<uint16_t>(page)).value_or(UINT32_MAX);
    local.hasVisibleOffset = true;
  }
  return local;
}

DocumentMatchMethod otherMethod(const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
}

std::string hashFor(const std::string& path, const DocumentMatchMethod method) {
  return method == DocumentMatchMethod::FILENAME ? KOReaderDocumentId::calculateFromFilename(path)
                                                 : KOReaderDocumentId::calculate(path);
}
}  // namespace

// Auto-sync runs on the loop task (serviceAutoSync() from loop()), but what it reads belongs to
// the render task: `section`, which the render task replaces under the render lock, and `epub`'s
// metadata cache, whose seek-then-read pairs the status bar also runs (BookMetadataCache keeps
// one file position). It used to peek at the lock and then read with nothing held, so a pass that
// started in between could free the Section being read or move the cache's file position mid-read.
//
// So every entry point takes the lock WITHOUT waiting (RenderLock::Mode::Try) and holds it for
// all of its section and epub reads. A busy lock means a pass is in flight: skip the tick, as
// the peek meant to. The cost is that a pass wanting the lock waits for the read to finish --
// for a push or an upload that includes the chapter inflate behind the local XPath. That happens
// once per push interval at most, and only while the reader is otherwise idle.
//
// The lock is NOT recursive, so nothing that takes it itself may run under it: the compare screen
// is launched after release (pollAutoSyncPull), and silentApplyRemote() relies on its caller's lock.

// The caller holds the render lock (see above).
bool EpubReaderActivity::autoSyncReaderIsQuiet() const {
  return epub != nullptr && section != nullptr && !section->hasActiveBuild() && readerPhase_ == ReaderPhase::READING;
}

// The caller holds the render lock: this reads `section`'s lookup tables, and ProgressMapper reads
// `epub`'s metadata cache.
KOReaderPosition EpubReaderActivity::currentKoPosition(const int page, const int pageCount) const {
  CrossPointPosition pos{};
  pos.spineIndex = currentSpineIndex;
  pos.pageNumber = page;
  pos.totalPages = pageCount;
  if (section) {
    if (const auto off = section->getVisibleTextOffsetForPage(static_cast<uint16_t>(page))) {
      pos.visibleTextOffset = *off;
      pos.hasVisibleTextOffset = true;
    }
    if (const auto paragraph = section->getParagraphIndexForPage(static_cast<uint16_t>(page))) {
      pos.paragraphIndex = *paragraph;
      pos.hasParagraphIndex = true;
    }
  }
  return ProgressMapper::toKOReader(epub, pos);
}


void EpubReaderActivity::prepareAutoBookmarkSnapshot(KOReaderSyncJob& job, const size_t changeOffset) {
  if (!epub || bookOrbitHash_.empty() || !KOREADER_STORE.hasCredentials() || !KOREADER_STORE.getCatalogCapable()) {
    return;
  }
  size_t totalChanges = 0;
  uint32_t revision = 0;
  if (!BookOrbitBookmarkSync::prepareExchange(bookmarkStore, epub, bookOrbitHash_, changeOffset,
                                              job.bookmarkExchange, totalChanges, revision)) {
    LOG_ERR("AutoSync", "Bookmark snapshot failed for %s", epub->getPath().c_str());
    return;
  }
  job.hasBookmarkExchange = true;
  job.bookmarkRevision = revision;
  job.bookmarkChangeOffset = changeOffset;
  job.bookmarkTotalChanges = totalChanges;
  job.bookmarkBatchSize = job.bookmarkExchange.changes.size();
  if (changeOffset != 0) {
    job.bookmarkExchange.keys.clear();
    job.bookmarkExchange.keysComplete = false;
  }
}

bool EpubReaderActivity::handleAutoBookmarkResponse(KOReaderSyncJob& job, const bool allowFollowup) {
  if (!job.hasBookmarkExchange) {
    if (!allowFollowup || job.kind == KOReaderSyncJob::BOOKMARK_CONTINUE ||
        (job.result != KOReaderSyncClient::OK && job.result != KOReaderSyncClient::NOT_FOUND)) {
      return false;
    }
    KOReaderSyncJob next;
    next.kind = KOReaderSyncJob::BOOKMARK_CONTINUE;
    prepareAutoBookmarkSnapshot(next);
    uint64_t seq = 0;
    if (next.hasBookmarkExchange && KOReaderSyncWorker::post(std::move(next), &seq)) {
      autoSyncBookmarkSeq = seq;
      return true;
    }
    return false;
  }
  if (!job.bookmarkSucceeded || !job.bookmarkResponseReady) {
    AUTOSYNC_STATE.markSyncFailed();
    return false;
  }

  KOReaderSyncJob next;
  next.kind = KOReaderSyncJob::BOOKMARK_CONTINUE;
  bool applied = false;
  if (bookmarkStore.revision() == job.bookmarkRevision) {
    BookOrbitBookmarkSync::ApplyResult result;
    applied = BookOrbitBookmarkSync::applyResponse(bookmarkStore, epub, epub->getPath(), epub->getCachePath(),
                                                   epub->getTitle(), job.bookmarkRevision, job.bookmarkResponse, result);
    if (applied && (!result.ack.applied.empty() || !result.ack.deleted.empty())) {
      next.hasBookmarkAck = true;
      next.bookmarkAck = std::move(result.ack);
    }
    if (result.positionFailure) {
      AUTOSYNC_STATE.markSyncFailed();
    }
    if (!applied) {
      AUTOSYNC_STATE.markSyncFailed();
    }
  } else {
    LOG_INF("AutoSync", "Bookmark response superseded by local edits; not applying or acknowledging");
  }

  const bool localMore = job.bookmarkChangeOffset + job.bookmarkBatchSize < job.bookmarkTotalChanges;
  const bool remoteMore = job.bookmarkResponse.more;
  if (allowFollowup && applied && localMore) {
    prepareAutoBookmarkSnapshot(next, job.bookmarkChangeOffset + job.bookmarkBatchSize);
  } else if (allowFollowup && applied && remoteMore) {
    next.hasBookmarkExchange = true;
    next.bookmarkExchange.hash = bookOrbitHash_;
    next.bookmarkRevision = bookmarkStore.revision();
  }

  if (!next.hasBookmarkAck && !next.hasBookmarkExchange) {
    next.hasBookmarkExchange = false;
  }
  uint64_t seq = 0;
  if (next.hasBookmarkAck || next.hasBookmarkExchange) {
    if (KOReaderSyncWorker::post(std::move(next), &seq)) {
      autoSyncBookmarkSeq = seq;
      return true;
    }
    LOG_DBG("AutoSync", "Bookmark follow-up deferred: sync worker busy");
  } else {
    // The worker retains WiFi only while a main-thread response is outstanding.
    next.hasBookmarkExchange = false;
    KOReaderSyncWorker::post(std::move(next), &seq);
  }
  return false;
}
void EpubReaderActivity::maybeAutoPullOnWake() {
  if (!KOReaderAutoSync::pullEnabled()) {
    autoSyncPullPending = false;
    return;
  }
  if (!KOReaderAutoSync::automaticAttemptAllowed()) {
    // This resume is skipped, not queued for a timer-driven retry.
    autoSyncPullPending = false;
    return;
  }
  if (KOReaderAutoSync::jobActive()) {
    return;
  }
  // Take a stable document path while the render task cannot replace the EPUB. Hashing the copy
  // below does not need the render lock.
  std::string path;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock() || !autoSyncReaderIsQuiet()) {
      return;
    }
    path = epub->getPath();
  }
  if (!KOReaderAutoSync::heapAllowsBackgroundSession("Wake pull")) {
    return;
  }

  const DocumentMatchMethod method = KOReaderAutoSync::effectiveMatchMethod(path);
  std::string primaryHash = hashFor(path, method);
  if (primaryHash.empty()) {
    LOG_ERR("AutoSync", "Wake pull: document hash failed for %s", path.c_str());
    autoSyncPullPending = false;
    return;
  }

  KOReaderSyncJob job;
  job.kind = KOReaderSyncJob::FETCH_PROGRESS;
  if (KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART) {
    std::string altHash = hashFor(path, otherMethod(method));
    if (!altHash.empty() && altHash != primaryHash) {
      job.smartProbe = true;
      job.altHash = std::move(altHash);
    }
  }
  job.primaryHash = std::move(primaryHash);
  // The bookmark snapshot reads EPUB/cache metadata; revalidate the same quiet document first.
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock() || !autoSyncReaderIsQuiet() || epub->getPath() != path) {
      return;
    }
    job.bookOrbitHash = globalReadingSessionTracker().getBookOrbitHash();
    prepareAutoBookmarkSnapshot(job);
  }

  uint64_t seq = 0;
  if (!KOReaderSyncWorker::post(std::move(job), &seq)) {
    LOG_DBG("AutoSync", "Wake pull deferred: sync worker busy");
    return;
  }
  autoSyncPullPending = false;
  autoSyncPullSeq = seq;
  LOG_DBG("AutoSync", "Wake pull started in background: %s", path.c_str());
}

bool EpubReaderActivity::pollAutoSyncPull() {
  if (autoSyncPullSeq != 0) {
    if (!KOReaderSyncWorker::isDone(autoSyncPullSeq)) {
      return false;
    }
    autoSyncPullJob = KOReaderSyncWorker::consume(autoSyncPullSeq);
    autoSyncPullSeq = 0;
    autoSyncPullJobPending = true;
  }
  if (!autoSyncPullJobPending) {
    return false;
  }
  bool askUser = false;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock() || !autoSyncReaderIsQuiet()) {
      return false;
    }
    autoSyncPullJobPending = false;
    askUser = evaluateAutoSyncPull();
  }
  // After release: launching the compare screen pushes an activity, which takes the lock.
  autoSyncPullDialogLaunched = askUser && handOffToInteractiveSync();
  return autoSyncPullDialogLaunched;
}

// The caller holds the render lock. Settles what it can silently and returns true when the user
// has to be asked instead (the caller launches the compare screen once the lock is released).
bool EpubReaderActivity::evaluateAutoSyncPull() {
  autoSyncPullDialogLaunched = false;
  KOReaderSyncJob job = std::exchange(autoSyncPullJob, {});
  handleAutoBookmarkResponse(job);

  const bool smart = KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART;
  const std::string path = epub->getPath();

  // What each record says from its XPath alone (spine, body-child paragraph): enough to compare
  // without inflating a chapter, which only an upload or an apply needs.
  const int spineCount = epub->getSpineItemsCount();
  const auto peek = [spineCount](const KOReaderProgress& progress) {
    return ProgressMapper::peekRemote({progress.progress, progress.percentage}, spineCount);
  };

  KOReaderSyncClient::Error result = job.result;
  KOReaderProgress remote = std::move(job.remote);
  if (smart && job.altResult == KOReaderSyncClient::OK && !job.altHash.empty() &&
      (result == KOReaderSyncClient::NOT_FOUND ||
       selectRemoteRecord(peek(remote), remote.percentage, peek(job.altRemote), job.altRemote.percentage) ==
           RemoteRecordChoice::Alternate)) {
    const DocumentMatchMethod altMethod = otherMethod(KOReaderAutoSync::effectiveMatchMethod(path));
    LOG_INF("AutoSync", "Wake pull: found remote progress under the alternate id; adopting it");
    KOReaderDocumentId::saveLearnedMatchMethod(path, altMethod);
    remote = std::move(job.altRemote);
    result = KOReaderSyncClient::OK;
  }

  if (result == KOReaderSyncClient::NOT_FOUND) {
    if (!smart) {
      return true;
    }
    LOG_DBG("AutoSync", "Wake pull: no remote progress; uploading local silently");
    silentUploadCurrentPosition();
    return false;
  }
  if (result != KOReaderSyncClient::OK) {
    LOG_INF("AutoSync", "Wake pull failed (%s); keeping current position", KOReaderSyncClient::errorString(result));
    AUTOSYNC_STATE.markSyncFailed();
    return false;
  }

  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  // The percentage only. The local XPath costs an inflate and two parses of the chapter, and is
  // computed by silentUploadCurrentPosition() if an upload turns out to be needed.
  const float localPercentage = ProgressMapper::percentageFor(epub, currentSpineIndex, page, total);
  const ProgressComparison comparison = compareProgress(localReadingPosition(*section, currentSpineIndex, page),
                                                        localPercentage, peek(remote), remote.percentage);
  switch (comparison) {
    case ProgressComparison::Synchronized:
      AUTOSYNC_STATE.markSynced(path, currentSpineIndex, page);
      LOG_DBG("AutoSync", "Wake pull: remote matches local (%.6f at %s); nothing to do", remote.percentage,
              remote.progress.c_str());
      return false;
    case ProgressComparison::LocalAhead:
      if (smart) {
        LOG_DBG("AutoSync", "Wake pull: local ahead, uploading silently");
        silentUploadCurrentPosition();
        return false;
      }
      break;
    case ProgressComparison::RemoteAhead:
      if (smart) {
        LOG_DBG("AutoSync", "Wake pull: remote ahead, applying silently");
        silentApplyRemote(remote);
        return false;
      }
      break;
    case ProgressComparison::Unknown:
      LOG_DBG("AutoSync", "Wake pull: cannot tell which side is further; asking");
      break;
  }

  return true;
}

bool EpubReaderActivity::handOffToInteractiveSync() {
  if (!KOREADER_STORE.hasCredentials() || !epub) {
    return false;
  }
  LOG_DBG("AutoSync", "Wake pull: progress differs, handing off to the compare screen");
  launchKOReaderSync(SyncLaunchMode::COMPARE, nullptr, {KOReaderSyncPostAction::Reader, {}});
  return true;
}

// The caller holds the render lock.
void EpubReaderActivity::silentUploadCurrentPosition() {
  if (autoSyncPushSeq != 0) {
    LOG_DBG("AutoSync", "Wake upload deferred: another push in flight");
    return;
  }
  if (!epub || !section) {
    return;
  }
  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  if (page < 0 || total <= 0) {
    return;
  }
  const KOReaderPosition local = currentKoPosition(page, total);
  KOReaderSyncJob job = KOReaderAutoSync::makeSilentPushJob(epub->getPath(), local, currentSpineIndex, page,
                                                           epub->getTitle(), epub->getAuthor(),
                                                           globalReadingSessionTracker().getBookOrbitHash());
  prepareAutoBookmarkSnapshot(job);
  autoSyncPushSeq = KOReaderAutoSync::pushSilentAsync(std::move(job));
}

// The caller holds the render lock, which covers both the position mapping (epub's metadata
// cache) and the jump below. It used to take the lock just for the jump; under the caller's
// lock that would now deadlock, the mutex not being recursive.
void EpubReaderActivity::silentApplyRemote(const KOReaderProgress& remote) {
  if (!epub) {
    return;
  }
  const KOReaderPosition koPos{remote.progress, remote.percentage};
  const int total = section ? section->estimatedTotalPages() : 0;
  const CrossPointPosition remotePos = ProgressMapper::toCrossPoint(epub, koPos, currentSpineIndex, total);
  if (remotePos.spineIndex < 0 || remotePos.spineIndex >= epub->getSpineItemsCount()) {
    LOG_ERR("AutoSync", "Wake apply: remote position unmappable (spine %d)", remotePos.spineIndex);
    return;
  }

  NavigationTarget target;
  if (remotePos.hasVisibleTextOffset) {
    target = NavigationTarget::makeVisibleOffset(remotePos.visibleTextOffset, remotePos.pageNumber);
  } else if (remotePos.hasListItemIndex) {
    target = NavigationTarget::makeListItem(remotePos.listItemIndex, remotePos.pageNumber);
  } else if (remotePos.hasParagraphIndex) {
    target = NavigationTarget::makeParagraph(remotePos.paragraphIndex, remotePos.pageNumber);
  } else {
    target = NavigationTarget::makePage(remotePos.pageNumber);
  }
  target.cachedSpineIdx = remotePos.spineIndex;

  currentSpineIndex = remotePos.spineIndex;
  navTarget = target;
  section.reset();
  if (!writeReaderProgressCache(epub->getCachePath(), remotePos.spineIndex, remotePos.pageNumber, 0, 0)) {
    LOG_ERR("AutoSync", "Wake apply: failed to persist remote position; live state still seeded");
  }
  AUTOSYNC_STATE.markSynced(epub->getPath(), remotePos.spineIndex, remotePos.pageNumber);
  requestUpdate();
  LOG_DBG("AutoSync", "Wake apply: jumped to spine %d page %d (%.6f)", remotePos.spineIndex, remotePos.pageNumber,
          remote.percentage);
}

void EpubReaderActivity::maybeAutoPushInterval() {
  if (activityManager.inSleepTransition()) {
    return;
  }
  if (!KOReaderAutoSync::intervalPushEnabled() || !KOReaderAutoSync::automaticAttemptAllowed()) {
    return;
  }
  if (!AUTOSYNC_STATE.lastNoteWasTurn()) {
    return;
  }
  const uint16_t interval = KOREADER_STORE.getPushIntervalPages();
  if (interval == 0 || AUTOSYNC_STATE.getTurnsSincePush() < interval) {
    return;
  }
  if (autoSyncPushSeq != 0 ||
      (autoSyncLastPushAt != 0 && millis() - autoSyncLastPushAt < KOReaderAutoSync::MIN_INTERVAL_PUSH_GAP_MS) ||
      KOReaderAutoSync::jobActive()) {
    return;
  }

  // Taken last, once a push is actually due, so the cheap checks above never touch the mutex.
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || !autoSyncReaderIsQuiet()) {
    return;
  }
  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  if (page < 0 || total <= 0) {
    return;
  }
  const KOReaderPosition local = currentKoPosition(page, total);
  autoSyncLastPushAt = millis();
  KOReaderSyncJob job = KOReaderAutoSync::makeSilentPushJob(epub->getPath(), local, currentSpineIndex, page,
                                                           epub->getTitle(), epub->getAuthor(),
                                                           globalReadingSessionTracker().getBookOrbitHash());
  prepareAutoBookmarkSnapshot(job);
  autoSyncPushSeq = KOReaderAutoSync::pushSilentAsync(std::move(job));
}

// Keep a completed job unconsumed on a busy tick; its response handler reads EPUB/cache state.
void EpubReaderActivity::pollAutoSyncJob() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || autoSyncPushSeq == 0 || !KOReaderSyncWorker::isDone(autoSyncPushSeq)) {
    return;
  }
  KOReaderSyncJob job = KOReaderSyncWorker::consume(autoSyncPushSeq);
  autoSyncPushSeq = 0;
  KOReaderAutoSync::finalizeSilentPush(job);
  handleAutoBookmarkResponse(job);
}

// The response may read the EPUB/cache through handleAutoBookmarkResponse().
void EpubReaderActivity::pollAutoBookmarkJob() {
  RenderLock lock(RenderLock::Mode::Try);
  if (!lock.ownsLock() || autoSyncBookmarkSeq == 0 || !KOReaderSyncWorker::isDone(autoSyncBookmarkSeq)) {
    return;
  }
  KOReaderSyncJob job = KOReaderSyncWorker::consume(autoSyncBookmarkSeq);
  autoSyncBookmarkSeq = 0;
  if (job.bookmarkResponseReady) {
    handleAutoBookmarkResponse(job);
  } else if (job.result != KOReaderSyncClient::OK || !job.bookmarkSucceeded) {
    AUTOSYNC_STATE.markSyncFailed();
  }
}

void EpubReaderActivity::refreshAutoSyncIndicator() {
  SyncIndicator indicator = SyncIndicator::None;
  if (KOREADER_STORE.getShowSyncIndicator()) {
    if (KOReaderSyncWorker::isBusy()) {
      indicator = SyncIndicator::Active;
    } else if (AUTOSYNC_STATE.lastSyncFailed()) {
      indicator = SyncIndicator::Failed;
    }
  }
  if (indicator == autoSyncIndicator) {
    return;
  }
  autoSyncIndicator = indicator;
  requestUpdate();
}

void EpubReaderActivity::serviceAutoSync() {
  pollAutoSyncJob();
  if (autoSyncPullPending) {
    maybeAutoPullOnWake();
  }
  if (pollAutoSyncPull()) {
    return;
  }
  pollAutoBookmarkJob();
  refreshAutoSyncIndicator();

  int spine = 0;
  int page = 0;
  std::string path;
  {
    RenderLock lock(RenderLock::Mode::Try);
    if (!lock.ownsLock()) {
      return;
    }
    if (epub == nullptr || section == nullptr) {
      return;
    }
    path = epub->getPath();
    spine = currentSpineIndex;
    page = section->currentPage;
  }
  // Outside the lock: noteProgress() may write its state file.
  AUTOSYNC_STATE.noteProgress(path, spine, page);
  maybeAutoPushInterval();
}


void EpubReaderActivity::releaseAutoSyncSlot() {
  if (autoSyncPushSeq != 0 && KOReaderSyncWorker::isDone(autoSyncPushSeq)) {
    KOReaderSyncJob job = KOReaderSyncWorker::consume(autoSyncPushSeq);
    KOReaderAutoSync::finalizeSilentPush(job);
    handleAutoBookmarkResponse(job, false);
  }
  autoSyncPushSeq = 0;

  if (autoSyncPullSeq != 0 && KOReaderSyncWorker::isDone(autoSyncPullSeq)) {
    KOReaderSyncJob job = KOReaderSyncWorker::consume(autoSyncPullSeq);
    handleAutoBookmarkResponse(job, false);
  }
  autoSyncPullSeq = 0;
  if (autoSyncPullJobPending) {
    handleAutoBookmarkResponse(autoSyncPullJob, false);
  }
  autoSyncPullJob = KOReaderSyncJob{};
  autoSyncPullJobPending = false;

  if (autoSyncBookmarkSeq != 0 && KOReaderSyncWorker::isDone(autoSyncBookmarkSeq)) {
    KOReaderSyncJob job = KOReaderSyncWorker::consume(autoSyncBookmarkSeq);
    handleAutoBookmarkResponse(job, false);
  }
  autoSyncBookmarkSeq = 0;
}

// From onExit(), which ActivityManager runs under the exclusive render lock, so the section and
// epub reads here are already covered.
void EpubReaderActivity::maybeAutoPushOnSleep() {
  if (epub != nullptr && section != nullptr) {
    AUTOSYNC_STATE.noteProgress(epub->getPath(), currentSpineIndex, section->currentPage);
  }
  AUTOSYNC_STATE.flush();

  if (!activityManager.inSleepTransition() || !KOReaderAutoSync::sleepPushEnabled() || !AUTOSYNC_STATE.isPending()) {
    return;
  }
  // A link followed in THIS session, not the persisted Back stack (footnoteDepth): a stack loaded
  // from linkstack.bin would otherwise skip the push on every sleep until the reader pressed Back,
  // and every link pushes, contents links included.
  if (sessionLinkDepth_ > 0) {
    LOG_DBG("AutoSync", "Footnote open at sleep; push deferred to the next boundary");
    return;
  }
  if (!epub || !section) {
    return;
  }
  const int page = section->currentPage;
  const int total = section->estimatedTotalPages();
  if (page < 0 || total <= 0) {
    return;
  }

  saveProgress(currentSpineIndex, page, total);
  const KOReaderPosition local = currentKoPosition(page, total);
  KOReaderSyncJob job = KOReaderAutoSync::makeSilentPushJob(epub->getPath(), local, currentSpineIndex, page,
                                                           epub->getTitle(), epub->getAuthor(),
                                                           globalReadingSessionTracker().getBookOrbitHash());
  prepareAutoBookmarkSnapshot(job);
  KOReaderAutoSync::stashSleepPush(std::move(job));
}

#endif
