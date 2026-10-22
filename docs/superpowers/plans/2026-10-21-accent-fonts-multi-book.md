# WR-1 — Accent fonts from several books, without the flash erase

**Status:** the PSRAM route of §11 is built (see §12). The flash-format route (§10.2) and the banks (§6) are not.
**Code base read:** the live stack `work/compact-stack-20261019` (upstream `master` + the local compact stack).
Line numbers cite that branch unless stated otherwise. Upstream sources are cited as `upstream/master:<file>`.

---

## 1. The question

"Allow accent fonts from multiple books without erase."

The terms, as the code uses them:

- **Accent family** — one of the extra font families a book sets some of its text in (a display face for
  headings, a script face for a dedication, a monospace for code). Upstream calls the mode
  *Publisher Fonts*; the modes are Off / Body Text / **Accents** / All Text.
- **The erase** — the flash *erase* the glyph store performs when the reader opens a book whose font set is
  not the one the store holds. It costs seconds, throws away the cached glyphs of the other book, and wears
  the flash.

Goal: a user who reads two books with different accent families should be able to switch between them
without the store being erased and re-copied each time.

## 2. Where accent fonts already work, and where they do not

**Per-book storage on the card is already right.** `BookFonts` extracts the book's own faces out of the EPUB
into that book's cache directory — the body to `<cache>/fonts/`, each accent family to
`<cache>/fonts/accent<n>/` (`lib/Epub/Epub/BookFonts.cpp:28,449`, `familyDir()` at :551). Nothing there is
global, so any number of books can have their own accent faces on the card at the same time. **The card side
of the feature needs no change.**

**The flash glyph store is the single owner.** All TrueType families the reader draws come from one
persistent store in the `spiffs` partition (`lib/EpdFont/ttf/FlashGlyphStore.*`), and that store holds
exactly one *family set* at a time:

| Store element | Limit | Source |
|---|---|---|
| set identity (header) | 16 B, one per store | `GlyphStoreFormat.h:45`, `TtfFamily.cpp:336` (`setIdentity`) |
| style blocks | `kMaxStyles = 8` | `GlyphStoreFormat.h:41` |
| face table | `kFaceEntries = 32`, shared by the whole set | `GlyphStoreFormat.h:44` |
| glyph key | cp 21 bits · style 2 · face index 5 | `GlyphStoreFormat.h` (`makeKey`) |

The set identity is FNV-1a over the *ordered* identities of the families in the set
(`ttf::setIdentity`, `lib/EpdFont/ttf/TtfFamily.cpp:336-345`), each family identity being its file name,
size and card timestamp (`lib/EpdFont/SdCardFontManager.cpp:161`). Two books with different accent
families therefore produce two different identities, and only one of them can be installed.

## 3. The erase, exactly

Opening a book whose set differs from the installed one:

1. `TtfFontSet::load()` calls `store_.open(identity)` (`lib/EpdFont/ttf/TtfFont.cpp:113`).
2. The header's identity does not match → `OpenResult::OtherFamily`
   (`lib/EpdFont/ttf/FlashGlyphStore.cpp:201`).
3. `TtfFontSet::install()` runs (`TtfFont.cpp:186`): it plans a layout, works out which style blocks still
   hold exactly the same file and may be **kept** (`TtfFont.cpp:265-277`), then calls
   `FlashGlyphStore::beginInstall()`.
4. `beginInstall()` erases the header sector, erases every style block that is not kept, and then erases
   **the index, the pair index and the whole log** (`FlashGlyphStore.cpp:150-155`).
5. The families' style bytes are copied in again (`TtfFont.cpp:277-291`).
6. The log is empty, so every glyph of the new set is rasterized again, one at a time, on first use.

Measured upstream (X4, `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` §11.1.3, "The kept blocks"):

| Book switch | Wall time |
|---|---|
| all blocks kept (4 styles) | 4.6 s |
| one block copied (6 styles) | 5.6 s |
| nothing kept | 13.8 s (phase 2 measured 13–17 s) |

The erase itself is the log: a full Latin store is about 1.1 MB, 2.2–2.7 s on the X3 (spec §4.6). Every
switch also wears the log's sectors (about 100,000 erase cycles per sector, spec §4.7 — centuries at one
switch a day, so wear is not the argument; **time and lost cache are**).

The upstream authors have already named the missing piece. Spec §11.1.2 ("What is left"):

> Any change of the set installs every family again … An install that keeps a matching leading family's
> blocks (its face entries and log records stay valid if its index does) would spare that; **the identity
> would then be per family.**

Phase 3 built the *block-keeping* half of that sentence (`TtfFont.cpp:265-277`), and recorded what is still
missing (spec §11.1.3, "What is left"):

> The kept-blocks install spares the copy but not the reinstall of the index and log: a kept family's
> glyphs render again. **Keeping them would need a per-family identity in the header (a format change).**

So WR-1 is the open item upstream left on the table, not a new idea.

## 4. A second, related gap

The partition has one owner at a time (spec §4.1): the `.cpfont` cache (`FlashFontPartition`) and the TTF
glyph store share the `spiffs` partition, and a `.cpfont` write erases the whole area below the language
slot. Consequence today: **a reader font that is a `.cpfont` (an SD font) drops the accent families
entirely** — `src/SdCardFontSystem.cpp:262`: *"Accent font %s left out: the reader's font is a .cpfont
family"*. Spec §11.1.2 lists the same item as open ("the accent family beside it needs the `.cpfont` read
from SD while the store holds the accent, or the accent in SD mode").

## 5. The budget any solution must fit

| Resource | Size | Source |
|---|---|---|
| `spiffs` partition | 3,538,944 B (`0x360000`) | `partitions.csv` |
| UI language slot at its tail | 128 KB (never touched) | `lib/hal/FlashFontPartition.h:82` |
| **usable font area** | **3,407,872 B (≈3.25 MB)** | `EspNorFlash::size()` → `fontUsableSize()` |
| store overhead | header 4 KB + state sector 4 KB | spec §4.1 |
| index / pair index | 64 KB pages (index sized to the log; pairs 64 KB) | `GlyphStoreFormat.h` |
| style block | 64 KB aligned; flash mode = the whole TTF, SD mode = the tables only | spec §4.1, §8.2 |
| MMU pages free at runtime | ≈31 of 128 (index + log stay mapped; a flash-mode block maps alone) | spec §4.1 |

Two space facts decide the shape of the answer:

- **One book's set in flash mode nearly fills the area.** A full Latin body is ≈1.8 MB (4 styles). A body
  plus three accent families does not fit twice over.
- **SD mode makes accents cheap.** An accent family is a subset (50–300 KB); its tables fit in about one
  64 KB block and the outlines are read from the card. Spec §11.1: *"the font area is about 3.3 MB; embedded
  families are subsets (50-300 KB a style)"*.

And one table decides how many books fit:

- the face table has 32 entries shared by the whole set; a book's body claims 5–10 sizes and an accent
  family 2–4 (spec §11.1). One book ≈ 19 entries. **Two books of body + 3 accents exceed 32**, and taking
  the last entry makes the wipe pending — the next book open then wipes the store (spec §4.6).

## 6. Options

### Option A — N independent stores ("banks") in the font area

Split the 3.25 MB area into N banks, each a complete, independent store (header + state sector + style
blocks + index + pair index + log). A bank's offsets become bank-relative; the header carries a bank id.
A book whose set is installed in another bank opens that bank: **no erase, no copy, the other book's
glyphs are still cached**. When no bank holds the set, install into the least-recently-used bank (erase
that bank only).

- **What changes:** bank selection and an LRU policy; `NorFlash` region per bank; the planner
  (`glyphstore::planLayout`) budgets against the bank's size. *No change* to the glyph key, the face table,
  the pair index, the write-once rules or the power-cut sweeps — each bank is the store as it is today.
- **Cost:** each bank gets area/N for its log, so a per-book glyph cache is smaller; the index is duplicated
  per bank (≈128 KB each). With SD-mode accents (3 blocks × 64 KB + index + pairs ≈ 450 KB per bank), two
  banks of ≈1.6 MB leave ≈1.2 MB of log each. With a TrueType **reader font** in flash mode (1.8 MB for the
  body) only one bank fits — see §7 for how to handle that.
- **Effort:** small to medium; the store's region maths is already relative.

### Option B — One store, per-family identity (upstream's "what is left")

The header holds a *family table* (each installed family's identity and its style blocks) instead of one
set identity. The face table's entries become `{familySlot << 24 | ppem, claimed}`, so a glyph record stays
valid when the set around it changes. A book switch then adds or removes a few families: the new blocks are
copied, the header sector is rewritten, and **the index and the log are left alone** — both books' glyphs
stay cached in one shared log.

- **What changes:** format version 5; a family table in the header; the face table keyed by a global family
  slot; the pair slot's family field widened to that slot (`pairStyle(family, style)` has room in 16 bits);
  the install path split into "add/remove families" (no erase) and "full install" (erase). The glyph key
  stays 5 bits *if* ≤32 faces are claimed; two books of body + 3 accents exceed 32, so either the table
  grows to 64 (which needs a 6th face bit in the key, and the key's "bits 28–31 are zero" invariant and
  `isGlyphKey()` would change) or the face table's limit becomes the multi-book limit.
- **Cost:** the deepest change of the three — the header, the face table, the install, the wipe and the
  power-cut sweeps all move. The face table is append-only (write-once entries), so a family that leaves
  leaks its slots until the next wipe; a wipe empties the log for *every* book, so books now share their
  bad days.
- **Effort:** the spec priced the multi-family store work at 4–6 days when it was a single set; per-family
  identity with a shared log is larger, plus the device sweeps.

### Option C — Accent families never enter flash

Draw the accent faces straight from the card, with no store: no flash copy, no cached bitmaps, and a book
switch costs nothing in flash.

- **Cost:** every accent glyph is rasterized on every use. A heading is 20–60 glyphs at roughly 5–20 ms a
  glyph on the C3 → 0.1–1.2 s per page. **Rejected as the only path**, viable only as the fallback when the
  area has no room (which is what the store already does by dropping the least-used accent family).

### Option D — Per-book glyph cache on the card

Keep the rasterized accent glyphs in the book's own cache directory (`<cache>/glyphs.bin`), keyed by
(family identity, size, style, codepoint), and read them from the card.

- **Pros:** per-book by construction, survives sessions, no flash erase, no change to the flash store.
- **Cons:** SD latency per page (a page's accent glyphs are 2–10 KB → 4 KB reads at the X3's ~30–40 KB/s
  free); a second cache format with its own invalidation rules; more card writes. **Rejected** as the
  default path, worth keeping as a fallback for a family the area cannot hold.

### Option E — Split the `.cpfont` cache from the TTF store

Give the `.cpfont` flash cache and the TTF glyph store separate regions of the partition.

- **Pros:** small, independent change; fixes §4 (a `.cpfont` reader font would stop erasing the TTF store)
  and removes one reason for the "accent left out" message.
- **Cons:** the `.cpfont` cache shrinks (it wants ~2.3 MB for a family with 5 sizes); the areas are then
  carved out of the same 3.25 MB, so the multi-book budget gets tighter.
- **Effort:** small (a boundary plus a size parameter), and a prerequisite for Option A if `.cpfont` and
  TTF sets are to live at the same time.

## 7. Recommendation

**Do Option A (two banks) on top of Option E (area split), with the accent families in SD mode. Keep
Option B (per-family identity) as the follow-on, gated on a measurement.**

Why A first:

1. It answers the ask directly — switching between two books with different accent families opens the
   other bank: no erase, no copy, no re-rasterizing, the other book's glyph cache intact.
2. It changes **no invariant of the store**: the glyph key, the face table, the pair index, the
   write-once rules, the wipe, the reclaim and the power-cut sweeps all stay exactly as they are. Each bank
   is the store as it exists today. The risk sits in a new "which bank" policy, not in the flash formats.
3. The space works for the case the ask is about. Accent families are subsets; in SD mode a book's accents
   cost about one 64 KB block each. Two banks of ≈1.6 MB hold a book's accent set plus ≈1.2 MB of log each.
4. A wipe stays inside its bank: one book's full log no longer takes the other book's cache with it.

Why the accent families must be in SD mode (a precondition, not a preference): a body in flash mode is
1.8 MB and the area is 3.25 MB. Two banks cannot each hold a flash-mode body. So:

- **Built-in or `.cpfont` reader font** (the common case): the set is the accent families alone → two
  banks, both comfortable, and the `.cpfont` reader font is read from the card while the store holds the
  accents (this is exactly the open item in spec §11.1.2, and Option E is what frees the area for it).
- **TrueType reader font**: the body is flash mode and takes the area for itself. Keep the current
  single-store behaviour for that combination (one set at a time, erase on switch) and say so in the docs;
  the accents beside it are SD mode already (§11.1.3). Widening this later means the body moves to SD mode
  as well, which costs a card read per glyph miss — a page-time decision, not a font-store decision.

**For a reader whose body is a TTF in flash mode, this recommendation inverts — see §10.5:** the area is
then too full for two banks, and the shared log (Option B) is the variant that fits.

Why B stays on the shelf for now: a shared log is the better end state (one log, no duplicated index, every
book's glyphs in one place), but it moves the header, the face table, the install and the wipe, and the
32-entry face table is itself a two-book limit that would have to be widened (and with it the glyph key).
The trigger to pick it up: with banks in place, measure how often a book's log fills (wipes) — if a bank
runs out of log while the other bank is nearly empty, the fragmentation of Option A is the real cost and
Option B earns its keep.

Cheap wins to take regardless (Stage 1):

- **Option E** (area split) — removes the "a `.cpfont` reader font erases the TTF store" cliff.
- **The accent-left-out message** (`src/SdCardFontSystem.cpp:262`) is a symptom of the same area
  contention; with E in place and the accents in SD mode it can go away.

## 8. Staged plan

| Stage | Change | Done when |
|---|---|---|
| 0. Measure | On the device, time the book→book switch in Accents mode: wall time, bytes erased, "N families installed: M styles (K kept)" from the serial test API (`CMD:TTF`, `docs/serial-test-api.md`). One book with 2 accent families, then a second book with different ones. | Numbers on the task, X3 and X4. |
| 1. Area split (Option E) | Give `FlashFontPartition` (the `.cpfont` cache) and `EspNorFlash` (the TTF store) separate regions of the partition; both sizes become parameters. | A `.cpfont` reader font no longer erases the TTF store; the accent families survive it (SD mode). Host tests for both regions' bounds. |
| 2. Two banks (Option A) | Bank table in the header area; bank-relative offsets; LRU bank selection; the planner budgets per bank. | Book A → book B → book A costs no erase and no copy; the serial log shows the second bank opening. |
| 3. Follow-on (Option B), gated | Per-family identity, a shared log, a wider face table. | Only if Stage 2 shows per-bank log fragmentation (wipes while the other bank is empty). |

Risks to watch: the MMU budget is unchanged (only the active bank is mapped); the erase wear per switch
drops to one erase per bank reuse; the section cache key already covers the effective font set, so a bank
switch does not invalidate a book's cached sections.

## 9. Where this lands

The Publisher Fonts work (BookFonts, the accent extraction, the multi-family store) is **not** in this
thread's checkout: its base is `3c9265bc` (24 Sep 2026), 1,382 commits behind `upstream/master`, and
`lib/Epub/Epub/BookFonts.cpp` does not exist there. The feature lives on `upstream/master`
(PR #503 and its stages) and in the live stack `work/compact-stack-20261019` (upstream master + 30 local
commits), which is where any implementation for WR-1 has to be based.

## 10. Addendum: the shared-log variant (body in flash, accents in SD mode, one log for every book)

Asked after §7: *the reader's body is a TTF and both books have several accent families; can the body stay in
flash, the accents always be SD mode, and the log be shared across books instead of wiped?*

**Short answer: yes, that is the right shape for that configuration — it is upstream's per-family identity
item (§11.1.3, "What is left") — but it is two changes, not one. "Accents always SD mode" is small and
stands alone; "share the log across books" is a format change with three traps that have to be handled at
the same time. One number decides whether the shape fits: the accents' flash footprint and the mapping
budget, not the erase itself.**

### 10.1 Accents always in SD mode — small, and it stands alone

The mode is already a *style block's* (format v3), and the planner already requires the flash-mode blocks to
come before the SD-mode ones (`GlyphStoreFormat.cpp:28`: a flash block after an SD block is refused), so
"body in flash mode, accents in SD mode" is a legal layout today.

What decides it is the install loop (`lib/EpdFont/ttf/TtfFont.cpp:222-237`): it tries flash mode for as many
families as fit and accepts a plan only when the log keeps `kAccentFlashLogBytes` (8 pages ≈ 512 KB,
`TtfFont.cpp:36`) — an accent family keeps flash mode only while the log stays that big. Pinning the accents
to SD mode means accepting the `sdFrom = count - 1` plan (only the body in flash mode) unconditionally.

What it buys, measured (spec §8.2):

| | flash mode | SD mode |
|---|---|---|
| bytes copied into flash per style | the whole TTF: 300–630 KB (Inter 341 KB, Noto Sans 630 KB) | the tables only: 92–245 KB (Bookerly 176 KB, of which GPOS is 111 KB) |
| an accent family's block | a subset, 50–300 KB (spec §11.1) | less again — the tables of a subset |
| outlines | read from flash | read from the card on a miss |

So the accents' flash footprint drops by roughly 3–5×, and the copy per book switch with it. Two costs to
weigh: an accent glyph that misses the log reads its outlines from the card, and **every SD-mode block is
mapped at the same time** (`planLayout` counts `largestFlash + sdPages` against the mapping budget), so more
accent families in SD mode eat the pages the log needs (§10.4).

### 10.2 Sharing the log across books — the format change

Three things stand between today's store and a shared log.

**(a) The layout must be pinned.** `planLayout` puts the style blocks first and the index, pair index and
log after them (`GlyphStoreFormat.cpp:21,54,58`): the log's *address* depends on the style blocks' sizes. A
different book's accent set has different sizes, so the log would move and the cached records would be
invalid anyway. The log, the index and the pair index have to live at fixed addresses, with the style blocks
allocated after them (or in their own reserved area).

**(b) The identity must be per family, and the set must be allowed to be a subset.** The header holds one
16-byte *set* identity — FNV-1a over the ordered families (`TtfFamily.cpp:336-345`) — and `open()` answers
`OtherFamily` when it differs (`FlashGlyphStore.cpp:201`). The header needs a *family table* (each installed
family's identity and its block), and `open()` has to accept a set that is a subset of what is installed:
the body plus this book's accents, with the other book's accents still sitting in their blocks.

**(c) The faces and the kerning pairs must be keyed by the family's identity, not by its index in the set.**
This is the trap that would produce wrong text rather than a slow one:

- the face table entry is `{family << 24 | ppem, claimed}` (`faceOf`), where `family` is the family's *index
  in the set* (`FlashGlyphStore.cpp:587-630`). Under a shared log, book B's accent at index 1 would match
  book A's face entry for the same ppem — and book A's cached glyphs would be drawn as book B's.
- the pair index is keyed by `(left, right)` alone, with the owner `family << 2 | style` in the slot's value
  (`FlashGlyphStore.cpp:635-673`). Under a shared log, book B's accent would read book A's kerning for the
  same pair. (Today the pair index can only hold one owner per key; `insert()` writes the first free slot in
  the chain, so a second owner lands in another slot — it works, but each family's pairs consume slots.)

Both have to key on the family's identity (its file name, size and card timestamp hash) instead of the set
index. With identity-keyed entries, a family that leaves can never be matched by the family that replaces it.

### 10.3 The write-once tables leak, and the face table is already nearly full

Face entries and pair slots are programmed once and never rewritten (`insert()` only programs erased slots).
Under today's design that is harmless, because every set change reinstalls the store and starts the tables
empty. Under a shared log, a family that leaves keeps its entries forever.

The face table has 32 entries shared by the whole store (`GlyphStoreFormat.h:44`): a body claims 5–10 sizes
and an accent family 2–4 (spec §11.1). One book with a body and three accent families claims roughly
19–22 entries; **a second book of the same shape reaches 38–44 — past the table.** When the table is full the
store makes the wipe pending (`FlashGlyphStore.cpp:605-613`), and the next book open wipes the store — which,
in a shared log, wipes *both* books' caches.

So the shared-log variant needs, together:

- identity-keyed face entries and a table that is big enough for the books in play (64 entries at 12 B is
  768 B, still inside the 4 KB state sector) — and the glyph key's face field grows from 5 bits to 6, which
  takes bit 28 and changes the key's "bits 28–31 are zero" invariant and `isGlyphKey()`
  (`GlyphStoreFormat.h`, `makeKey`);
- a way to reclaim the entries of a family that leaves — either rewriting the face table (the state sector
  is erased in 4 KB granules, and it also holds the wipe word and the rebuild journal, so this is a
  deliberate act with its own rules) or letting the table grow and wiping when it fills;
- the same for the pair index, whose slots also name a family.

### 10.4 Does it fit? The two budgets for this configuration

Body in flash mode, two books with several accent families each:

| | size |
|---|---|
| body, 4 styles in flash mode | 4 × 300–630 KB = **1.2–2.5 MB** |
| 6 accent families in SD mode | 6 × ~92–245 KB (less for a subset) = **0.5–1.5 MB** |
| index (2–4 pages) + pair index (64 KB) + header/state (8 KB) | **0.2–0.4 MB** |
| font area | **3.25 MB** (3,407,872 B) |

The partition leaves roughly 0.3–1.2 MB for the log. The **mapping budget binds first**: ≈31 free MMU pages
(measured 32), of which the largest flash-mode block takes 5–10 pages, *every* SD-mode block is mapped
together (6 blocks × 1–4 pages = 6–24 pages), the index 2–4, the pairs 1, and 2 spare — leaving **6–12 pages
(384–768 KB) of log**. The partition may have a megabyte free that cannot be mapped.

Three levers, in order of value:

1. **Map one SD-mode block at a time** instead of all of them together. This is already an open item in the
   spec (§8.1: "Consider mapping one style's tables at a time in SD mode too") and it is worth more here than
   anywhere else: it hands the log back the 6–24 pages the accent blocks were holding.
2. **Keep the body in flash mode** (as asked) — it is the hot path and its outlines should not be read from
   the card.
3. **Put the body in SD mode too** if the log still comes out too small: the body's tables are 4 × 92–176 KB
   ≈ 0.4–0.7 MB instead of 1.2–2.5 MB, at the price of a card read per body glyph miss.

### 10.5 What this changes about §7's recommendation

For a **built-in or `.cpfont` reader font** (accent-only sets), the two-bank variant of §7 still wins: it
needs no format change at all.

For **a TTF body in flash mode** — this configuration — the banks do not fit: a bank cannot hold a
1.2–2.5 MB body plus a log inside half of a 3.25 MB area. So the shared-log variant (per-family identity) is
the one that fits, and it is also the variant upstream's own "what is left" points at.

Recommended order for this configuration:

| Stage | Change | Effort | Effect |
|---|---|---|---|
| 1 | accents always SD mode (`TtfFont.cpp:222-237`), body stays in flash mode | 1 day | the copy per switch drops by 3–5×; flash freed for the log |
| 2 | pin the layout (log/index/pairs at fixed addresses, blocks after them) | 2–3 days | a prerequisite for stage 3, and it makes the kept-blocks rule work across books |
| 3 | per-family identity: a family table in the header, a set may be a subset, identity-keyed faces and pairs, a 6-bit face index, the install split into "add/remove families" (no erase) and "full install" (erase) | 1.5–2 weeks, with the power-cut sweeps | **no erase on a book switch**; both books' glyphs stay cached in one log |
| 4 | reclaim the entries of a family that leaves (face table, pair index) | 3–5 days | the shared log does not die of leaked slots after a few books |

Stage 3 is the same item upstream named in spec §11.1.2/§11.1.3 and has not been built there either; doing
it in this stack means the format change, the host tests and the device sweeps land here rather than
upstream, so the work is worth sending upstream as a PR.

### 10.6 Verification for the shared-log variant

- Host: the store's tests (`test/`, `TtfFontSetTest`, the store's power-cut sweeps) gain the cases that
  matter for a shared log: a second family's glyphs do not answer for the first, a removed family's entries
  are not reused by the family that replaces it, a switch adds and removes families without erasing, and a
  wipe empties every book's cache at once.
- Device (serial test API, `CMD:TTF`): open book A (body + 3 accents), then book B (body + 3 other
  accents), then A again. Expect: no "installed" line on the second and third opens (no install), the log's
  used bytes to keep growing rather than reset, and the page times to stay flat across the switch.

## 11. Addendum: PSRAM — the accent families in RAM, and the flash store left alone

Asked after §10: *can PSRAM help — e.g. a book's accent glyphs in RAM instead of flash, when there is RAM to
spare?*

**Short answer: yes, and on the PSRAM boards it is a better route to "no erase on a book switch" than the
flash format change of §10.2 — because if the per-book accent families live in RAM, the flash store ends up
holding only the *global* reader font, whose set never changes, so the store is never reinstalled and never
erased. It applies to `env:x4pro` and `env:lilygo_t5s3` (ESP32-S3, 8 MB PSRAM). It does not apply to the
X4/X3 (ESP32-C3, no PSRAM), where §10's flash work remains the only path.**

### 11.1 Which boards, and what PSRAM is actually free for

| board | MCU | PSRAM | note |
|---|---|---|---|
| X4 / X3 | ESP32-C3 | none | the store's erase problem is unchanged here |
| X4 Pro | ESP32-S3 (N16R8) | 8 MB octal | `env:x4pro`, `-DBOARD_HAS_PSRAM`, both framebuffers in PSRAM |
| LilyGo T5S3 | ESP32-S3 (N16R8) | 8 MB octal | `env:lilygo_t5s3`, `-DBOARD_HAS_PSRAM` |

The allocator policy is not "PSRAM is the overflow" (P0.3, `docs/psram-utilization-audit-2026-09-17.md`):
ordinary `malloc`/`new` stay internal on an S3, external placement is explicit
(`psramAllocBytes()`, `MALLOC_CAP_SPIRAM`, no internal fallback — `lib/Memory/HeapPools.h`), and the heap
gates measure *internal* RAM. The audit's measurement: on a T5S3 at boot, 8,662,104 B free combined with
**325,956 B internal** — "≈8.3 MB of PSRAM is effectively idle after the display driver takes its share".

That is the right shape for the accent data: large, cold, rebuildable, never DMA or ISR reachable — exactly
what the explicit-PSRAM policy is for. On the S3, PSRAM reads are 80 MHz octal with a 32 KB data cache, so a
glyph bitmap read from PSRAM is not free, but neither is it slower in kind than a read from the mapped flash
log; the win is not raw speed, it is that **no flash is erased, no flash is written, and the card is not read
per miss.**

### 11.2 What the accent families would need in RAM

Per accent family (one style, the common case):

| | size | source |
|---|---|---|
| the family's font data | 50–300 KB (the subset the book embeds; spec §11.1) | copied from the book's cache dir (`fonts/accent<n>/`) at book open |
| its rasterized glyphs | 100–300 glyphs × ~80–150 B ≈ 10–45 KB a size | produced on first use |
| kerning/ligature tables, the scratch | a few KB | already RAM |

Two books × three accent families ≈ **0.4–1.5 MB** — comfortable in 8 MB, and it is *per session*, freed when
the book closes.

The important difference from SD mode: in SD mode the family's *tables* sit in a flash block and its
**outlines are read from the card** on every glyph miss. With the family in PSRAM, the outlines come from
PSRAM, so a miss costs a RAM read, not a card read. That also removes the accent blocks from the flash
mapping budget (the "map one SD-mode block at a time" item of §10.4 stops mattering for them).

### 11.3 The engine work, and where the seam is

The store is currently the only glyph source, and the seam is narrow:

- `TtfFont::glyph()` asks the store for the key (`lib/EpdFont/ttf/TtfFont.cpp:769`), and a miss rasterizes
  and commits into it (`TtfFont.cpp:817-830`), then re-reads the record from the store.
- the set owns exactly one `FlashGlyphStore` (`TtfFontSet`), and the renderer's glyph provider already
  receives a pointer to an `EpdGlyph` plus its bitmap.

So a RAM-backed family needs two things, both behind the same interface the renderer already uses:

1. **A RAM glyph store** — `find`/`commit` over a PSRAM arena (a hash map of key → `EpdGlyph` + bitmap),
   with a ring/eviction policy; the bitmap pointer is direct instead of an offset into the log.
2. **A RAM font-data source** — the family's bytes copied into PSRAM at book open and read in place.
   `SdBlock`/`FontBytes` already serve ranges from a pointer, so a PSRAM buffer is one more source, not a
   new engine.

`TtfFont` then picks its store by the family's mode, and the set holds both (the global body in the flash
store, the per-book accents in the RAM store). The renderer, the layout and the section cache do not change.

Book lifecycle: the accents are per-book, so they are allocated at book open and freed at book close (or at
the next book open) — explicit PSRAM alloc/free, no flash erase, no flash wear, and the switch cost becomes
"allocate PSRAM, read the accent subsets from the card, rasterize the accents' glyphs on first use".

### 11.4 What it does not fix

- **The C3.** No PSRAM: the flash format change (§10.2) or the banks (§6, Option A) remain the only routes.
  A PSRAM-only answer leaves the X4/X3 users with the erase.
- **A per-book body.** In Body Text / All Text mode the book's *body* family joins the set, and that is
  per-book → the flash set changes → an install and an erase. The RAM path as described covers the
  *accents*. If the body were RAM-backed too (its subset in PSRAM plus a RAM glyph cache), the flash store
  would be unused on PSRAM boards and the problem would disappear there entirely — at the price of a RAM
  body font instead of a mapped flash one (the body's outlines would come from PSRAM rather than the mapped
  block, and the body's glyph cache would live in RAM rather than in the log).
- **A `.cpfont` reader font.** Unrelated to the store: the `.cpfont` cache and the TTF store still share the
  partition (§4 of the earlier addendum, Option E).
- **The C3's reader font in flash mode plus accents**: unchanged by PSRAM work; that is §10.

### 11.5 Order, effort, and how the two paths meet

| Stage | Change | Board | Effort | Effect |
|---|---|---|---|---|
| A | a RAM glyph store and a RAM font-data source; the accents loaded into PSRAM at book open | x4pro, lilygo | ~1 week (source + store + manager wiring + host tests), then device runs | **no erase on a book switch on PSRAM boards**; no card read per accent miss; the flash store holds the body only |
| B | (only if a per-book body matters) the body family RAM-backed too | x4pro, lilygo | ~2–3 days on top of A | the flash store is unused on PSRAM boards |
| C | the per-family identity work of §10.2 | **C3** (and any board that keeps flash-backed accents) | 1.5–2 weeks | the same property on the C3 |

The two paths do not conflict: A is additive and compile-gated on `BOARD_HAS_PSRAM`, and it leaves the flash
store's format alone, so C can still be built on top if the C3 needs it. If the user's own device is the
C3 X4, then **C is the work that matters** and A is a separate line for the S3 boards; if the device is the
X4 Pro or the T5S3, **A is the shorter path to the goal** and C can wait.

This also belongs on the sibling task WR-2 ("additional uses for psram"): an accent-family cache is a
concrete P1 candidate in the audit's terms — large, cold, rebuildable, explicitly placed.

### 11.6 Verification for the PSRAM path

- Host: the RAM store's tests (find/commit/evict, a key that is not a glyph key, an arena that is full), and
  the manager's tests (the accents are allocated at book open and freed at book close; a book switch frees the
  old book's accents and allocates the new ones; the flash store is not reinstalled).
- Device (X4 Pro, T5S3): open book A (body + 3 accents), then book B (body + 3 other accents), then A again.
  Expect: no "installed" line and no erase on opens 2 and 3, the body's page times flat across the switch,
  PSRAM high-water reported by SystemStatus stable across books (the audit's pools are reported separately),
  and the accent misses served without card reads (the SD read counters in the serial test API stay flat).

## 12. What was built (2026-10-21, branch `feature/psram-accent-families`)

§11's route, for the X4 Pro (and the T5S3): **the flash store keeps the reader's own family alone, and a book's
own families are drawn from a store in RAM.** The flash store's set then never changes, so opening the next book
does not install it again, the flash is not erased, and the reader's glyphs stay cached.

The store in RAM is not a new store: it is the same `FlashGlyphStore` over a new device, `RamFlash` — a `NorFlash`
over a buffer in PSRAM. The chip's rules stand (bits program from 1 to 0 only, whole 4 KB sectors), so the store's
write-once rules, its index, its pair index, its face table, its log and its wipe all run unchanged over it.

| File | What |
|---|---|
| `lib/EpdFont/ttf/RamFlash.h/.cpp` | the device: a `NorFlash` over a caller's buffer, 1 -> 0 programming, sector erases, no MMU pages |
| `lib/EpdFont/ttf/RamFontStore.h/.cpp` | the buffer (PSRAM, 2 MB) and the device over it; nothing where there is no PSRAM |
| `lib/EpdFont/SdCardFontManager.*` | the split: the first `globalFamilies` families in the font partition, the rest in the RAM store; two sets, two stores, one font ID and one native-size window per family |
| `src/SdCardFontSystem.cpp` | which families are the reader's own (the setting's family, and neither the book's body family nor a font chosen for that book alone) |
| `src/SerialTestApi.cpp`, `docs/serial-test-api.md` | `CMD:TTF` reports the RAM store's fields with a `ram_` prefix |

What it costs and what it saves:

- The partition is written once, when the reader's family is first installed. A book switch writes nothing there.
- The book's families are copied from the card into PSRAM at every book switch (the same copy the flash store made),
  and their glyphs are cached in the RAM log, not in the partition's.
- A board without PSRAM (the C3) is unchanged: the RAM store is not made, and the font partition takes every family,
  as before.
- A book whose families do not fit in RAM (or a board whose PSRAM cannot serve the buffer) falls back to the font
  partition, with a log line.

Tests: `test/glyph_store/RamFlashTest.cpp` (the device's rules, and the glyph store over it) and
`test/ttf_font/TtfFontTest.cpp`'s *TheReaderFamilyStaysInThePartitionWhileTheBookFamiliesAreInRam* (the partition
is not written while the book's families are installed in RAM, and the reader's glyph is read from the store, not
drawn again). The host suite: 3265 tests, all passing. `env:x4pro` and `env:default` both build.

On the device (X4 Pro, `env:x4pro`): open a book with accent families, then another, then the first. The serial log
should show no *installed* line for the partition on the second and third opens (the RAM store is installed instead),
and `CMD:TTF` should show the reader's family in the partition's fields and the book's families in the `ram_` ones.

## 13. Sources

- `docs/superpowers/specs/2026-10-13-ttf-fonts-design.md` (upstream master) — §4.1 partition layout,
  §4.3 keys, §4.6 when the store fills, §4.7 wear, §11.1 Stage B, §11.1.2 and §11.1.3 "what is left",
  with the X4 switch timings.
- `lib/EpdFont/ttf/FlashGlyphStore.cpp:116-201` — `beginInstall()` (the erase) and the identity compare.
- `lib/EpdFont/ttf/TtfFont.cpp:113-114,186-293` — open, install, kept blocks, the copy.
- `lib/EpdFont/ttf/GlyphStoreFormat.h:36,41,44,45` — format version, style blocks, face table, identity.
- `lib/EpdFont/ttf/TtfFamily.cpp:336-345` — `setIdentity`.
- `lib/EpdFont/SdCardFontManager.cpp:161-169` — family identity and the set identity.
- `lib/Epub/Epub/BookFonts.cpp:28,449,551` — per-book extraction to `fonts/accent<n>/`.
- `src/SdCardFontSystem.cpp:262` — the accent dropped beside a `.cpfont` reader font.
- `lib/hal/FlashFontPartition.h:82`, `partitions.csv` — the language slot and the partition size.
- `platformio.ini` (`env:x4pro`, `env:lilygo_t5s3`, `-DBOARD_HAS_PSRAM`) — the PSRAM boards.
- `docs/psram-utilization-audit-2026-09-17.md` — the P0.3 allocator policy and the measured pools.
- `lib/Memory/HeapPools.h` — `psramAllocBytes()`, the internal/external split, the heap gates.
- `lib/EpdFont/ttf/TtfFont.cpp:762-830` — the glyph lookup and the commit into the store (the seam a RAM store
  would plug into).
