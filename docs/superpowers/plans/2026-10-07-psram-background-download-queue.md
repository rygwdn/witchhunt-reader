# PSRAM Background Book Download Queue — Implementation Plan

> Proposal derived from a read-only feasibility investigation. No implementation changes are included in this plan.

**Goal:** Let users queue OPDS books while browsing, return to reading immediately, and have selected books stream sequentially to SD on supported PSRAM-equipped ESP32-S3 devices. Coordinate downloads and automatic KoSync through one background network owner. Never download during deep sleep or keep the device awake solely to complete a queue.

**Scope decision:** `SCOPE.md:25-37` includes library management and book transfer but explicitly rejects background Wi-Fi because it drains battery and complicates execution. Treat this as a narrow, user-initiated book-transfer exception for PSRAM S3 devices only. Obtain project-level scope approval before implementation; keep automatic/periodic catalog crawling and non-PSRAM support out of scope.

## Architecture and constraints

- **Single network owner:** Serialize KoSync and download requests. `SecureClient.cpp:39-56,124-136` documents global TLS callback/handshake state as single-threaded. Do not start a second concurrent TLS worker.
- **Queue metadata only:** Stream bodies directly to SD; never buffer complete books in RAM. `SecureHttpClient.cpp:248-262` currently streams through a 2 KiB body buffer.
- **Durable service lifetime:** Queue and worker outlive the OPDS activity; no activity pointer, callback, parser cache offset, or open feed-cache handle may be retained.
- **PSRAM is not TLS heap:** Worker stack and TLS allocations use internal RAM. Gate transfers using measured internal free and largest contiguous block. Current KoSync admission floors (`KOReaderAutoSync.cpp:24-29,175-185`) are not yet validated for a transfer concurrent with reading.
- **Reader responsiveness:** Reuse/extend the reader's service-busy coordination (`EpubReaderActivity.cpp:82-87,1594-1609`) to defer speculative work. Do not hold `RenderLock` across network operations. Continue servicing visible page rendering and input.
- **Safe publication:** Write each item to a unique staging path; verify and close it; publish only after successful completion. Existing `downloadToFile` deletes its target before writing and removes it on errors (`HttpDownloader.cpp:325-365`), so do not pass a final library path directly.
- **Bounded lifetime:** Downloads are allowed only while awake. On sleep, cancel and quiesce the active transfer, flush/close storage, destroy network sessions, then allow existing radio/storage teardown. Pending jobs survive sleep; partial staging files are discarded unless a separately designed resume feature is added.

## User-visible behavior

1. OPDS book rows can be selected for queuing without immediately starting a synchronous download. Preserve the current one-book direct-download action as an immediate enqueue-and-start behavior or replace it with an explicit queue action consistently; do not silently change the established format-choice semantics.
2. For a book with multiple acquisition formats, queueing requires choosing one format. A queued item snapshots its selected acquisition URL, MIME/format, extension, title/author, source, and destination policy.
3. Provide a queue view/status entry point through existing library/browser UI rather than adding a top-level activity unless existing surfaces cannot present queue state.
4. Show aggregate counts and per-item queued/running/completed/failed/cancelled state. Progress updates are throttled and must not trigger frequent e-ink refreshes.
5. Individual completed books remain available if a later item fails. A failed or cancelled current item leaves no visible partial book; existing library files remain untouched.
6. Explicit cancel/remove applies to queued items immediately and to an active transfer through the independent cancellation path described below.
7. Do not silently replace a destination on sanitized-name collision. Require a non-destructive conflict policy (skip with visible result is the initial recommendation); never overwrite an open book.

## Collection semantics

- “Queue this collection” means enqueue supported book acquisition links directly listed by that OPDS feed and follow that feed's pagination (`next`) links until exhausted.
- Do **not** recurse into nested navigation collections in this version. Navigation rows are Atom links rather than filesystem folders; recursive crawling can be unbounded or cyclic.
- Apply a traversal-wide maximum item/page bound and stop on repeated normalized feed URLs. The exact cap must be a named conservative constant chosen from bounded queue/storage budgets before implementation; it is not the parser's existing per-page 2,000-entry limit.
- Feed parsing must use the current `lib/OpdsParser` and `OpdsEntry` acquisition data. Resolve relative links against the feed URL. Do not send saved Basic credentials to an unrelated host: constrain credential forwarding to the configured source origin (including redirects) or fail the item safely.
- Folder traversal and book-transfer jobs are distinct service states so traversal can enqueue safely and be cancelled without retaining the OPDS activity.

## Queue and network contracts

### Queue ownership

Introduce a service-level queue with explicit bounded capacity and durable SD-backed metadata. Store only job descriptors and state in RAM; persist enough metadata to survive reset/sleep. Define queue versioning, corruption handling, duplicate detection, and atomic persistence before adding UI.

Each book job records:
- Source type and configured server identity; credentials by stable reference where possible, not copied into plaintext queue metadata.
- Resolved URL, expected format/MIME and extension, title/author, and a stable item identifier.
- Final destination decision and a separate staging destination. Snapshot folder/filename settings at enqueue so later setting changes do not retarget jobs.
- Current state, byte progress when known, result/error category, and whether a transfer was cancelled.

Never persist credentials in plaintext. Resolve the credential reference through the existing credential store when a job starts; if the server/credential was removed or changed, fail visibly instead of silently using unrelated credentials.

### Network scheduler

Extend the KoSync background worker/service into the sole serialized network owner, or replace its single-slot job contract with an equivalent owner whose queue and result lifetimes are explicit. The current `KOReaderSyncWorker` is one replaceable slot (`KOReaderSyncWorker.cpp:209-237,256-267`), not a multi-job queue; do not assume it safely retains unconsumed results.

Scheduler requirements:
- At most one active network operation and one TLS handshake/session at a time.
- KoSync jobs are admitted between book transfers; sleep-push deadlines remain bounded and retain current behavior.
- A foreground Wi-Fi activity must quiesce or take ownership before connecting; the worker must not tear down a radio it did not start or that foreground activity owns. Existing worker checks `currentActivityUsesWifi()` (`KOReaderSyncWorker.cpp:98-115`).
- Existing HTTP `Session` may be reused sequentially for consecutive same-host book files and destroyed when the batch becomes idle or sleep is requested (`HttpDownloader.cpp:195-224`). Never share it concurrently.
- Use Wi-Fi only for a queued batch or pending sync. Disconnect/turn off a worker-owned radio when idle; restore the existing power-save policy after each request.
- Admission uses internal heap metrics, not total heap/PSRAM. Validate both free-internal and largest-contiguous thresholds under representative reader/TLS overlap before choosing production values.

### Downloader cancellation and file safety

Add an explicit cancellation/quiesce contract through `HttpDownloader` and the underlying `SecureHttpClient`. Do not depend only on the current progress callback: the downloader forwards progress only when total size is known (`HttpDownloader.cpp:143-160`), while body/header waits can block without polling a cancellation token (`SecureHttpClient.cpp:184-207,283-315`). Cancellation must cover connection/handshake where technically supported, header reads, fixed/chunked/close-delimited body waits, and writes. If handshake interruption is unavailable, document and enforce its bounded maximum in pre-sleep behavior rather than claiming immediate cancellation.

Write to a unique temporary staging file in the destination filesystem. On success, flush/close, validate non-empty/readable output, check destination conflict, then use the storage HAL's safe same-filesystem rename/commit operation. On failure/cancel, close and remove only that job's staging file. Verify that commit cannot expose a partial artifact and that the currently open EPUB path cannot be replaced.

## Implementation tasks

### Task 1 — Confirm scope and define bounded behavior

- [ ] Approve the narrow PSRAM-only, user-initiated exception to `SCOPE.md:31-37`.
- [ ] Set queue capacity, feed traversal cap, duplicate policy, destination conflict behavior, queue indicator surface, sleep semantics, and credential-reference behavior.
- [ ] Confirm target boards and feature gate. Keep X4/C3 builds free of the feature and its persistent RAM cost.

### Task 2 — Define storage and queue contracts

- [ ] Inspect current storage HAL rename/atomic-write support; add a HAL operation only if required, keeping locking/logging in the HAL.
- [ ] Define versioned queue metadata, bounded capacity, state transitions, durable writes, corruption recovery, and duplicate detection.
- [ ] Ensure persistence excludes secret values and parser offsets; test reset between enqueue, transfer, and commit.
- [ ] Document filename collision and open-book protection behavior.

### Task 3 — Unify background network ownership

- [ ] Refactor the KoSync worker's single result slot into a network service with explicit queued-job and consumed-result ownership; migrate KoSync callers without compatibility shims.
- [ ] Serialize KoSync, feed traversal, and downloads. Preserve sync-on-wake, interval-push, and sleep-push semantics.
- [ ] Add lifecycle states for idle, connecting, active request, quiescing, and foreground handover. Preserve radio ownership rules.
- [ ] Expose a read-only busy/admission signal for reader speculative scheduling.
- [ ] Verify idle worker/task and session release so task stack and sockets are not retained indefinitely.

### Task 4 — Make HTTP transfers cancellable and atomic

- [ ] Add an independent cancellation/quiesce check throughout HTTP request/body waits; unknown `Content-Length` must remain cancellable.
- [ ] Stream into a unique staging path with no whole-book allocation. Keep storage operations through `HalStorage`/`HalFile`.
- [ ] Commit only verified completed files; clean staging files after errors, cancellation, reset recovery, and sleep.
- [ ] Preserve TLS policy, redirect downgrade protection, Basic-auth behavior, timeout behavior, and progress semantics for current foreground callers.
- [ ] Ensure cancellation closes the socket/session and restores Wi-Fi power-save exactly once.

### Task 5 — Add OPDS enqueue and collection traversal

- [ ] Change the OPDS book action to capture owned job data, including an explicit format when multiple acquisition links exist.
- [ ] Add bounded collection traversal that follows pagination only, skips repeated feed URLs, and never recursively follows nested navigation links.
- [ ] Enforce source-origin credential boundaries, including redirects and cross-host acquisition URLs.
- [ ] Reuse existing OPDS parser, URL resolution, format classification, and filename helper; do not retain activity/cache references.
- [ ] Report unsupported links, duplicate entries, feed failures, and traversal-cap exhaustion as item/queue results.

### Task 6 — Queue UI and reader integration

- [ ] Add a queue status/management entry point in existing OPDS/library UI; avoid a new top-level activity unless necessary.
- [ ] Implement enqueue, remove, cancel-active, clear-completed, and visible failure/result states.
- [ ] Keep the reader responsive: no network wait under `RenderLock`, no progress-driven refresh flood, and no speculative A/B work competing unnecessarily with transfer time.
- [ ] Extend the current KoSync background-busy coordination to queued transfers without suppressing visible rendering/current-section work.
- [ ] Route strings through `tr()` and update translations via the project's standard generator process.

### Task 7 — Sleep, foreground networking, and recovery

- [ ] Add a shared quiesce/join path before Wi-Fi shutdown and `Storage.prepareForSleep()`; do not rely on the current 60-second HTTP idle timeout.
- [ ] Preserve pending items across deep sleep while cancelling/cleaning the active staging file.
- [ ] On boot, reconcile persisted `running` entries to retryable/failed state and remove only uncommitted staging artifacts owned by the queue.
- [ ] Coordinate OPDS, BookOrbit, web-server, USB, and serial transfer activities with radio/storage ownership. Migrate every affected caller and remove obsolete paths.
- [ ] Verify no worker accesses activity-owned data after activity destruction.

### Task 8 — Tests, docs, and hardware acceptance

- [ ] Add deterministic tests for queue state transitions, persistence recovery, bounded/cyclic feed pagination, duplicate and name collisions, source-origin auth rules, and commit/failure/cancel behavior.
- [ ] Exercise failure boundaries: feed error, network timeout, unknown content length, stalled body, short write, SD-full, zero-byte result, cancellation, reboot, and sleep.
- [ ] Run firmware builds for at least X4 Pro and LilyGo T5S3 plus a non-PSRAM target to prove compile-time exclusion and no regression.
- [ ] On X4 Pro and T5S3, compare an otherwise identical no-download reader baseline against a multi-book queue during page turns, cold Background-B/C builds, and pre-render/image warm-up.
- [ ] Record internal free and largest block, PSRAM free/min, TLS handshake minima, worker/render stack high-water marks, page-turn and render p50/p95/max latency, max input-loop latency, A/B/C progress, SD/display lock wait, queue completion/cancellation errors, and sleep/wake integrity.
- [ ] Exercise sleep during handshake, stalled read, active body, between jobs, and final file commit. Verify Wi-Fi off, storage sleep preparation, wake, library visibility, and open-book integrity.
- [ ] Update user documentation and release notes with PSRAM-only support, battery/network behavior, sleep cancellation, collection semantics, and queue limits.

## Acceptance criteria

- A user can queue multiple OPDS books and return to reading without waiting for transfer completion.
- “Queue collection” processes supported direct book entries across pagination, with bounded traversal and no nested-recursive crawl.
- KoSync and downloads never run concurrent TLS/network operations; pending sync work is not lost or silently overwritten.
- Reader input/rendering remains responsive under the validated X4 Pro and T5S3 workloads; thresholds are based on measured internal heap/latency, not PSRAM total.
- Every successful book appears only after verified completion. Failed/cancelled transfers cannot replace existing files or expose partial books.
- Cancellation works with unknown-length and stalled bodies, and sleep/foreground handover quiesces network and storage before teardown.
- Queue state survives sleep/reset without credentials in plaintext; interrupted transfers recover deterministically.
- Non-PSRAM targets do not gain the worker, queue allocations, or UI surface.
- Existing KoSync, OPDS foreground downloads, BookOrbit downloads, and foreground Wi-Fi activities retain their documented behavior.

## Evidence and known risks

- OPDS currently downloads synchronously in its activity and uses an activity-capturing progress callback: `src/activities/browser/OpdsBookBrowserActivity.cpp:807-852`.
- OPDS activity exit removes its feed cache and disconnects/restarts Wi-Fi: `OpdsBookBrowserActivity.cpp:278-290`.
- OPDS currently has no multi-select state; selection is a feed-cache index: `OpdsBookBrowserActivity.h:47-67`, `OpdsBookBrowserActivity.cpp:382-387`.
- OPDS browser follows feed pagination only after user navigation; collection queueing needs its own bounded traversal: `OpdsBookBrowserActivity.cpp:650-704,742-780`.
- Existing transfer destination is removed before writing and on failure: `src/network/HttpDownloader.cpp:325-365`.
- Existing KoSync worker has one job/result slot, internal stack 10 KiB, and activity-owned radio checks: `src/activities/reader/KOReaderSyncWorker.h:33-47`, `KOReaderSyncWorker.cpp:98-142,209-267`.
- Existing sleep path drains KoSync before Wi-Fi teardown, but has no download-service quiesce: `src/main.cpp:666-690`.
- Reader B/C uses PSRAM scratch and a busy gate but has not been proven safe with simultaneous TLS transfer: `src/activities/reader/EpubReaderActivity.cpp:82-87,1594-1614`.
- X4 Pro SDMMC and T5S3 SD-SPI/display bus arrangements differ. The investigation found an apparent software lock coupling on T5S3 despite distinct physical display/SD buses; measure and address this HAL issue only if verified on the active branch before asserting concurrency expectations.
- Existing HTTP handshake minima and PSRAM reader logs are separate workloads; they do not establish safe concurrent admission thresholds.

**Verification status:** Plan only. No implementation, build, test, or device run performed.