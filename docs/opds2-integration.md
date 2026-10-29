# OPDS 2.0 and progression: Bookorbit and Witchhunt

This is stage 3 of the OPDS 2 work: the stage-1 JSON OPDS 2.0 catalog browsing
plus stage-2 reading-position (progression) sync and reading stats for books
downloaded from it. Further reading-status handling arrives in the follow-up
stage; this document covers only what these changes implement.

## Connect

Enable OPDS in Bookorbit and create an OPDS account. The settings page offers copy
buttons for both catalog versions:

- Atom OPDS: `/api/v1/opds`
- JSON OPDS 2.0 dashboard: `/api/v1/opds/v2`

Configure Witchhunt with the full JSON URL and the OPDS username/password. Opening
the server shows Continue Reading, Up Next, Recently Added, and Browse as groups
with their preview publications, plus faceted sort/filter links and paginated
navigation. Existing Atom connections keep working: the browser sniffs the body
and dispatches to the JSON or Atom parser per feed.

Credentials are sent only when the fetched URL's host matches the configured
server's host; cross-host redirects or off-host cover/image links are fetched
anonymously rather than leaking the OPDS password.

## JSON catalog browsing

- Groups render as shelf labels above their publications; each publication is a
  taller row with cover thumbnail, title, author (with the inline dashboard
  percent when Bookorbit includes `metadata["https://bookorbit.app/ns/readingProgress"]`),
  and its series label.
- `belongsTo.series` renders as `Name #position`. Name and position may arrive in
  either order in the JSON; the label composes after the series object closes, so
  both orders produce the same text. When the series advertises a link, the book
  detail screen opens that series' feed with the Left button (hint shows
  "Series").
- `facets` links become navigable entries; `navigation` entries behave as in Atom.
- Pagination follows `rel: next` / `rel: previous` links, resolved against the
  feed URL.
- Search: the OPDS 2 search link (`rel: search`, `application/opds+json`) is used
  as a URI template. `{?query}`, `{query}`, and the Atom-era OpenSearch
  `{searchTerms}` placeholders are substituted with the URL-encoded query.

The JSON parser is streaming and bounded: one publication is held at a time, the
per-publication byte budget is 24 KiB, and URLs or keys that would overflow a
buffer fail the feed instead of being silently truncated. Truncated or malformed
JSON fails with the usual "Failed to parse feed" error.

## Progression

The [OPDS Progression draft](https://github.com/opds-community/drafts/blob/main/opds-progression-1.0.md)
supports both fetching and uploading the last reading position. Publications
advertise a link with relation `http://opds-spec.org/progression` and media type
`application/opds-progression+json`.

`GET /api/v1/opds/:bookId/progression` returns the last progression document, or
an empty `200` response. `PUT` accepts a document containing:

```json
{
  "modified": "2026-01-01T12:00:00.000Z",
  "device": { "id": "urn:reader:example", "name": "Reader" },
  "progression": 0.42,
  "references": ["chapter.xhtml#paragraph"]
}
```

`title` and `references` are optional. The draft fraction is 0 to 1; Bookorbit's
existing percentage is 0 to 100. The API converts between them. The service
preserves references and bridges representable CFI, PDF page, and simple
audiobook time references to existing position fields.

Updates are scoped to the authenticated account and accessible file. The optional
`fileId` query distinguishes downloadable files belonging to one book.
Authorization, library access, and content filters are checked on reads and
writes.

Older timestamps receive `409 Conflict`. Identical retries are idempotent. A
newer timestamp can move backward intentionally. Conditional upsert plus
row/advisory locking protects the first-write race with other readers.
Timestamps more than five minutes in the future are rejected. Successful updates
also use Bookorbit's existing automatic reading-status rules and external Kobo
progress bridge.

Authentication failures return an OPDS Authentication Document. Progression
errors, including JSON parser errors, use Problem Details. Cover-image tokens
cannot authenticate progression writes.

The draft covers the last position, not reading-session history, statistics,
ratings, annotations, or bookmarks. Exact locations are not universally
portable: CFI, KOReader XPath, and device pagination differ. Witchhunt prefers a
resolvable element anchor and otherwise resumes by fractional position.

Two namespaced hints complement the standard document:

- `metadata["https://bookorbit.app/ns/readingProgress"]`: inline dashboard
  fraction, avoiding one progression request per visible book.
- Acquisition-link `properties["https://bookorbit.app/ns/progression"]`: the
  progression URL for that specific file, avoiding inferred endpoint paths for
  alternate formats/editions.

Other clients can ignore these hints. Witchhunt follows ordinary OPDS feeds and
publication progression links when they are absent.

## Reading stats extension

This is a Bookorbit extension advertised through OPDS, separate from the OPDS Progression draft. Publication links advertise:

- `rel`: `https://bookorbit.app/rel/reading-stats`
- `type`: `application/vnd.bookorbit.reading-stats+json`
- `href`: `/api/v1/opds/{bookId}/stats`

Acquisition-link `properties["https://bookorbit.app/ns/reading-stats"]` supplies the URL with `?fileId=...` for the exact downloaded file. Clients follow the advertised URL. Atom entries advertise the same publication-level relation and media type.

POST with OPDS Basic authentication and that media type. The version 1 body contains `device: {id, name}` and `sessions`, with 1 to 16 immutable events:

```json
{
  "version": 1,
  "device": { "id": "urn:witchhunt:0123456789ab", "name": "Witchhunt Reader" },
  "sessions": [
    {
      "id": "c38cbe7a-2051-42be-91e2-8571ba992825",
      "started": "2026-01-01T12:00:00Z",
      "ended": "2026-01-01T12:05:00Z",
      "activeSeconds": 240,
      "pagesTurned": 8,
      "startProgression": 0.2,
      "endProgression": 0.25
    }
  ]
}
```

Session IDs are UUID v4; device IDs are URIs. Timestamps include a timezone. Durations are positive integer seconds, no greater than elapsed wall time; sessions span at most seven days. End times cannot be more than five minutes ahead. Pages turned are nonnegative integers (maximum 1,000,000); optional progress fractions are between 0 and 1. Payloads are limited to 32 KiB.

A successful response is HTTP 200 with `{"version":1,"accepted":["session UUID", "..."]}` and the same media type. Identical retries are acknowledged without adding time again. IDs are scoped to the authenticated user and device. Reusing an ID with different data or a different file returns HTTP 409 and rolls back the entire batch. Clients delete only acknowledged events.

The server bulk-inserts sessions and updates the existing reading-session/daily-totals system in one transaction, splitting totals at midnight in the user's timezone. There are no per-session SQL round trips. The source is `opds`; original device/session payloads, including pages turned, are retained in `reading_sessions.external_stats`. Existing reading-time reporting includes these sessions. Pages turned are retained for future reporting, with no new pages-turned chart in this change. Existing source-bucket reports group OPDS under Bookorbit.

Stats ingestion never changes current progress or reading status. A stale/rejected position does not discard a valid reading session. This also prevents late historical stats from reopening a completed book.

Witchhunt queues the tracker's idle-clamped active seconds and page turns at session close. Each completed session has a stable ID across retries. Its separate account-scoped outbox sends up to eight sessions per file per request and up to four requests before opening the catalog; one HTTP client reuses the connection during this pass. Only acknowledged sessions are removed. Stats upload runs independently of the progression outbox. Local reading history still works without a server association or a synchronized clock; server upload requires valid timestamps. There is no automatic historical backfill.

## Reading status

Successful progression updates run Bookorbit's existing reading-status and
reading-attempt rules, including library reading/completion thresholds and
rereading behavior. Confirming Witchhunt's finished-book screen additionally
queues a progression of 1.0 for the associated book, before any move to
`/COMPLETED` or removal from recents. This covers EPUB, TXT/Markdown, and XTC.
Cancelling the screen does not send a completion confirmation. Older queued
positions and same-millisecond last-page replays cannot overwrite that queued
confirmation.

Status sync occurs on the next OPDS connection, subject to authentication and
stale-update protection. Manual server status overrides continue to follow
Bookorbit's existing rules. The standard progression document has no arbitrary
status field: this change does not add want-to-read, abandoned, or manual status
editing in Witchhunt.

## Witchhunt reading workflow

An OPDS download associates its local path with the advertised progression
service. Existing unrelated SD-card books are not automatically matched to
server publications.

Position changes in the EPUB, TXT, Markdown, and XTC readers are queued at
reader close. Reopening the OPDS catalog sends pending uploads before fetching
the feed. The device records the time of the last position change, not
reconnection time. Simply opening and closing a book does not produce a new
position update. An unset device clock does not produce invented timestamps.
Closing the reader also ends the stats session.

The EPUB reader additionally offers **Sync > Fetch OPDS progress**. It displays
the remote percentage, device, and timestamp, and asks before moving. Choosing
the remote position clears the queued/conflicting local update for that book.

Failed uploads stay queued. Stale uploads are retained in a conflict directory
and shown as a warning in the OPDS browser. Fetch the server position from the
EPUB Sync menu to choose it. Continuing to read locally creates a new
timestamped update. Each connection sends at most eight pending updates.
Outboxes are partitioned by account, separate from download associations, so
syncing does not scan the downloaded library. Backup/rename handling preserves
prior queue records during replacement.

## Automatic sync through the KOReader scheduler

The existing KOReader sync preferences also control OPDS-associated EPUBs. The
provider is selected per book: an OPDS download association uses the advertised
OPDS service and its OPDS account credentials; an unassociated EPUB keeps the
existing KOReader provider. OPDS automatic sync does not require KOReader
credentials, a KOReader document hash, or an XPath calculation.

- **On close:** the existing auto-push-on-close setting and minimum session-page
  count apply on every board. The reader saves its pending position, ends the
  stats session, releases its live section, and opens the foreground OPDS
  upload activity. The activity connects through the existing WiFi selector when
  necessary. Success or cancellation returns Home; failures keep the outboxes for
  retry. Upload activities release a radio they started.
- **On sleep and while reading:** the existing sleep-send preference, page
  interval, one-minute minimum interval, radio ownership checks, heap gates,
  status indicator, and shared network worker apply on S3 devices with PSRAM.
  Closing/sleeping includes completed session stats; intervals send queued
  events but do not cut the current stats session into artificial sessions.
  C3 retains its existing restriction to foreground close-time sync.
- **On wake:** on S3 with PSRAM, the existing receive-on-wake preference fetches
  the associated OPDS position in the shared background worker. Positions within
  the currently displayed local page need no dialog, including fractional
  positions rounded to the page start. A different position opens the OPDS
  confirmation screen using the response already fetched, without another GET.
  If the reader has moved while fetching, the automatic result is not applied.
  KOReader's furthest-wins option is deliberately not applied to OPDS, whose
  newer positions may move backward for rereads. A missing remote position can
  trigger the existing upload path.
- **Finished-book sync:** the existing finished-book sync toggle uploads the
  queued completion and stats for an associated EPUB. Its Home / next book /
  author search destination is preserved. Moving the book to `/COMPLETED` copies
  the download association so future rereading can still sync.

Uploads drain the associated account's bounded outbox, including queued changes
from other books in that account. The existing per-connection limits still
apply. A short SD mutex protects outbox mutations; it is never held during HTTP.
When an upload finishes, its acknowledgement or conflict handling only
removes/moves the exact queued position it sent, preserving a newer page or
completion queued while the request was in flight. Failed queue writes keep the
live position dirty for retry. Sleep joins retain the existing time budget;
unfinished network requests leave durable queue records for the next connection.

The shared automatic scheduler currently belongs to the EPUB reader. TXT,
Markdown, and XTC continue queueing progress at close and syncing through the
OPDS catalog; they do not gain background scheduling from this change.

## Covers

JSON rows show a 64x96 1-bit cover thumbnail. On first scroll onto a page the
browser downloads that page's covers (JPEG or PNG, told apart by the magic byte),
converts and stores them under `/.crosspoint/opds-covers/<hash>.bmp`, keyed by
cover URL. Later visits reuse the cache. The book detail screen keeps the larger
cover, also accepting PNG feeds now.

## Caching and sessions

- The Atom nav-entry discovery cache is skipped for `/opds/v2` URLs: a JSON
  dashboard's groups and facets are not representable in that cache, so the feed
  is fetched each time.
- The per-entry cache file gains `progressPercent`, `groupTitle`, `seriesTitle`,
  `seriesHref`, `progressionHref`, and `statsHref` fields; readers of old cache
  files see a missing or shifted tail as a miss and refetch.
- One HTTP+TLS session (HttpDownloader::Session) is reused across the feed fetch,
  cover pages, and downloads in a browsing session, so the TLS handshake — the
  heap/CPU spike — runs once per host instead of per request. The session is
  dropped on exit.

## Verification

Not yet run as part of preparing this patch series; planned checks:

- Host tests: `cmake -S test -B test/build && cmake --build test/build --target
  OpdsJsonParserTest OpdsParserTest && ctest --test-dir test/build -R Opds`
  (`test/README`). The suite covers dashboard streaming with groups, series
  (both key orders), unicode, progress percent, publication-level and per-file
  progression hints, facets, malformed/truncated feed rejection, oversized-URL
  rejection, and the OPDS sync policy (completion survival, credential-free
  automatic sync, in-flight acknowledgements, wake rounding).
- Firmware build: `pio run -e default` (C3) and the X4 Pro environment from
  `platformio.ini`.
- Manual: with only OPDS credentials configured, download a book from the JSON
  dashboard, read, close, reopen the catalog, and confirm the server progress
  moves; confirm the finished-book screen queues a 1.0 that later page replays
  cannot overwrite; attempt a stale upload after changing the server position
  and confirm the browser warning plus reader-menu fetch; confirm Fetch OPDS
  progress asks before resuming and clears the queued update on acceptance; use
  the existing automatic preferences for close/sleep/interval/wake flows.

References: [OPDS 2.0](https://github.com/opds-community/specs/blob/master/opds-2.0.md) and
[OPDS Progression draft](https://github.com/opds-community/drafts/blob/main/opds-progression-1.0.md).
Both source documents were inspected directly during implementation.
