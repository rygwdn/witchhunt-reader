# OPDS 2.0 (JSON) catalog browsing — Bookorbit and Witchhunt

This is stage 1 of the OPDS 2 work: opening Bookorbit's JSON OPDS 2.0 dashboard in
Witchhunt's OPDS browser. Progression sync, reading stats, and reading-status
handling arrive in the follow-up stages; this document covers only what this
change implements.

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
  and `seriesHref` fields; readers of old cache files see missing tails as a miss
  and refetch.
- One HTTP+TLS session (HttpDownloader::Session) is reused across the feed fetch,
  cover pages, and downloads in a browsing session, so the TLS handshake — the
  heap/CPU spike — runs once per host instead of per request. The session is
  dropped on exit.

## Verification

Not yet run as part of preparing this patch series; planned checks:

- Host tests: `cmake -S test -B test/build && cmake --build test/build --target
  OpdsJsonParserTest && ctest --test-dir test/build -R OpdsJsonParser`
  (`test/README`). The suite covers dashboard streaming with groups, series
  (both key orders), unicode, progress percent, facets, malformed/truncated feed
  rejection, oversized-URL rejection, and long-feed memory behavior.
- Firmware build: `pio run -e default` (C3) and the X4 Pro environment from
  `platformio.ini`.
- Manual: connect to a Bookorbit `/api/v1/opds/v2` URL with credentials, confirm
  shelves/percent/series render, page through, search via `{?query}`, open a
  series, and confirm covers appear after a second visit to a page (cache reuse).
