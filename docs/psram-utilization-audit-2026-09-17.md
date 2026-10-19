# PSRAM Utilisation Audit (S3 boards) — 2026-09-17

Status: P0.1, P0.2, telemetry, and the P0.3 policy are implemented. §7.1/§7.4 have device
evidence; §7.2's X4 Pro framebuffer A/B ran on 2026-09-29 (see §10); §7.3 and P1–P3 remain open.
Broader P0.3 placement confirmation remains pending.
This audit began as an investigation and answered the question left open three times:
[multi-board-bringup-2026-08-14.md:361-374](multi-board-bringup-2026-08-14.md) ("enable but
don't spend"), [multiboard-bringup-handover-2026-08-15.md:967-971](multiboard-bringup-handover-2026-08-15.md)
(same, restated), and [display-capability-audit-2026-08-17.md:151-154](display-capability-audit-2026-08-17.md)
("whether the release/realloc dance should happen there **at all** is an open question").

Scope: ESP32-S3 boards (`env:x4pro*`, `env:lilygo_t5s3*`, 8 MB octal PSRAM). The C3
(X3/X4) path must stay behaviourally identical — every policy change is compile-gated on PSRAM.

Sections 1–8 and the placement logs describe the pre-P0.3 allocator policy. The current
internal-default allocation policy and explicit cache overflow path are recorded in §9.
The X4 Pro framebuffer's PSRAM default and A/B results are recorded in §10.

---

## 1. Historical finding under the pre-P0.3 allocator

**Before 2026-09-28, "enable but don't spend" was never in force. The firmware spent PSRAM
implicitly, and its placement policy sent the largest allocations external.**

Proof chain, all verifiable on this machine:

1. Both S3 Arduino variants the envs link (`board_build.arduino.memory_type = dio_opi` at
   [platformio.ini:326](../platformio.ini), `qio_opi` at [platformio.ini:396](../platformio.ini))
   ship this policy:
   ```
   #define CONFIG_SPIRAM_USE_MALLOC 1
   #define CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL 4096
   #define CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL 0
   ```
   `~/.platformio/packages/framework-arduinoespressif32-libs/esp32s3/{dio_opi,qio_opi}/include/sdkconfig.h:880-893`.
   Also `CONFIG_SPIRAM_MODE_OCT 1`, `CONFIG_SPIRAM_SPEED 80`, and a 32 KB / 8-way / 32 B-line
   data cache (`CONFIG_ESP32S3_DATA_CACHE_SIZE=0x8000`, top-level `esp32s3/sdkconfig:2163-2173`).
2. The core arms it at boot: `psramAddToHeap()` calls
   `heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)`
   (`framework-arduinoespressif32/cores/esp32/esp32-hal-psram.c:103-105`), reached from
   `initArduino()` (`esp32-hal-misc.c:301,311`). `CONFIG_ARDUINO_ISR_IRAM` is not defined in
   either variant, so the `#if` passes.
3. ESP-IDF semantics for that threshold: allocations **≤ 4096 B try internal first**,
   allocations **> 4096 B try PSRAM first**, each falling back to the other pool
   (`esp32s3/include/heap/include/esp_heap_caps.h:365-372`; ESP-IDF "Support for External RAM").
4. `new` is not exempt. In the S3 libstdc++ the only undefined symbol in `new_op.o`
   (`_Znwj`) is `malloc`; `new_opv.o` (`_Znaj`), `new_opnt.o` and `new_opvnt.o` all resolve to
   `_Znwj`:
   ```
   $ xtensa-esp-elf-nm -A --undefined-only .../esp32s3/libstdc++.a | grep new_op
   new_op.o:    U malloc
   new_opnt.o:  U _Znwj
   new_opv.o:   U _Znwj
   new_opvnt.o: U _Znaj
   ```
   So `makeUniqueNoThrow<T[]>(n)` ([lib/Memory/Memory.h:27-37](../lib/Memory/Memory.h)),
   `new (std::nothrow)`, `std::vector` growth and raw `malloc` are all the *same* placement
   policy. There is no allocator in this firmware that opts out.
5. `HalPowerManager.cpp:29-31` already states the conclusion for one consequence (CPU
   frequency scaling): *"every allocation over 4 KB lands in PSRAM"*. That comment is correct
   and generalises further than it claims.

### What the old default allocator therefore put in PSRAM on X4 Pro / T5S3

| buffer | size B | site | why it is >4 KB |
|---|---:|---|---|
| Facade framebuffer ×2 | 48,000 / 64,800 each | `FreeInkDisplay.cpp:221-230,378-389` | `FREEINK_FB_PSRAM` is **off** for these boards (`BoardConfig.h:333-334`), so the `malloc` fallback runs — and lands external anyway |
| AA capture planes `capLsb`/`capMsb` | 48,000 / 64,800 each | `EpubReaderActivity.cpp:4483-4490, 4901-4906` | `makeUniqueNoThrow` → `malloc` |
| BW snapshot chunks | 8,000 × 6..9 | `GfxRenderer.cpp:3505-3515`, `GfxRenderer.h:34-35` | per-chunk `malloc(8000)` |
| Warm-pass scratch arena | 41,216 | `Section.cpp:1940` | `WARM_PASS_SCRATCH_BYTES = 32*1024 + 2*4096 + 256` |
| Extract arena | 37,888 | `Epub.h:220-224` | `EXTRACT_ARENA_BYTES` |
| ZIP/PNG inflate ring | up to 32,768 | `InflateReader.cpp:8` | `INFLATE_DICT_SIZE` |
| Pixel-cache band | up to 24,576 + 1 row | `PixelCache.h:83-125` | `MAX_BAND_BYTES` |
| Cover extraction chunk | 16,384 | `HomeActivity.cpp:293-300` | `COVER_EXTRACT_CHUNK` |
| TJpgDec work pool | 12,288 | `JpegToFramebufferConverter.cpp:263-287` | `TJPG_WORK_POOL_SIZE` |
| Section parse arena | 10,240 | `Section.cpp:256` | `SCT_PARSE_ARENA_BYTES` |
| SD-font page bitmap cache | Σ glyph `dataLength` | `SdCardFont.cpp:1549-1568` | routinely >4 KB for a text page |
| Section page LUT | 4 × pageCount | `Section.cpp:594-611` | >4 KB past 1024 pages |

Already explicitly external and correct — leave alone: LilyGo `g_canvas` 518,400 B
(`setPsram(true)`, `LgfxEpdDriver.cpp:224-236`), its `g_lsb`/`g_msb` 64,800 B each
(`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, same lines), X4 Pro `_grayBase` 48,000 B
(`Uc8179Driver.cpp:227-230`, `Uc8279X4Driver.cpp:294-300`).

Already explicitly internal and correct — leave alone: the SDMMC bounce buffer, 4,096 B
`MALLOC_CAP_DMA | MALLOC_CAP_8BIT` (`SdmmcBlockDevice.cpp:67-72`), which exists precisely
*because* "SdFat's caches may be in PSRAM or unaligned"
([xteink-x4pro-support.md:269-271](../freeink-sdk/docs/xteink-x4pro-support.md)). This is why
PSRAM destinations for `file.read()` are already safe on X4 Pro: the transport bounces.

Measured headroom, T5S3 boot ([multiboard-bringup-handover-2026-08-15.md:378-381](multiboard-bringup-handover-2026-08-15.md)):
`free=8,662,104` combined, `contig=6,160,372`, **`325,956` internal**. So the scarce pool on
an S3 is internal RAM at ~326 KB, and ~8.3 MB of PSRAM is effectively idle after the display
driver takes its share.

---

## 2. Consequence A — the C3 scarcity choreography still runs, and it costs quality

The borrow/release protocol exists because the C3 has one tight heap
([memory-allocation-strategy.md §1](memory-allocation-strategy.md)). On an S3 it buys nothing
— the arena it is borrowing *for* would have gone to PSRAM anyway — and it charges real
quality:

- `beginBackgroundBorrow()` takes the secondary framebuffer and sets
  `secondaryBufferDegraded_ = true` ([EpubReaderActivity.cpp:1213-1248](../src/activities/reader/EpubReaderActivity.cpp)).
  Its gates are `esp_get_free_heap_size()` and
  `heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)` (`:1456-1459`) —
  the **combined** pool, so on an S3 they always pass. Background-B therefore always borrows.
- While degraded: AA is switched off for the render (`:4318-4322`) and the page ships with
  `FULL_REFRESH` (`:4987-4989`) — the slowest waveform — instead of the fast/half cycle.
- A page turn tears the live build down before returning the region (`endBackgroundBorrow`,
  `:1251-1286`), **discarding the parse**. `BG_BUILD_MAX_PREEMPTIONS = 2` (`:220-222`) then
  abandons the spine to Background-C, i.e. the user gets the "Indexing…" freeze that B existed
  to prevent. Cost of one discarded attempt, measured: *"attempt 2 (extraction already banked)
  is setup 123 + parse 818 = ~950 ms measured on X3 alice spine 1"*
  ([background-rendering.md:129-130](background-rendering.md)).
  **Caveat:** after the `BG_BUILD_BORROW_QUIET_MS` + input-queued fixes, `preempt=` was
  measured at 0 for a whole C3 session (`:210-212`), so on the C3 this cost is currently
  latent, not routine. It has never been measured on an S3 — where the quiet window competes
  with a ~605 ms deferred-AA pass that does not exist on the C3, so the race is different.
  The AA-off + `FULL_REFRESH` cost above, by contrast, is unconditional on *every* borrow.

Same shape elsewhere: `HomeActivity` releases the secondary to decode covers and retries the
realloc later (`HomeActivity.cpp:229-243, 626-642`); `FontSelectionActivity` stops preview
warm-up when the *combined* largest block drops under `WARMUP_MIN_CONTIGUOUS = 18,432`
(`FontSelectionActivity.h:81-84`, `.cpp:294-307`) — a floor derived from an X3 measurement of
~22.5 KB.

**None of that needs PSRAM spending to fix. It needs the S3 to stop pretending it is a C3.**

## 3. Consequence B — every heap gate measures the wrong pool

`heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT)` and
`esp_get_free_heap_size()` appear in ~40 places (see `grep -n heap_caps_get_largest_free_block
src lib`). On an S3 both include PSRAM, so:

- Every floor passes unconditionally — they can no longer refuse anything, so they are not
  protecting the S3.
- `maybeRestartForFragmentedHeap` (`EpubReaderActivity.cpp:3455`, sized against
  `SECONDARY_BUFFER_BYTES = 52*1024`) can never fire on an S3. Dead code, not a safety net.
- The pool that *is* scarce (internal, ~326 KB, carrying task stacks and every DMA buffer)
  is watched in exactly one place: `scarceHeapWatermark()` switches to
  `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` under `BOARD_HAS_PSRAM`
  (`EpubReaderActivity.cpp:425-430`). That is the right pattern; it is applied once.
- `SystemStatus` reports no PSRAM fields at all (`SystemStatus.h:69-74, 98-125`), so the web
  UI and the user-facing memory screen cannot show the S3's real state.

## 4. Consequence C — hot data is accidentally external

The 4 KB threshold sorts by *size*, but the right axis is *access temperature*. The two
hottest buffers in the system are the framebuffer (written per glyph, per page) and the BW
snapshot (48 KB copied twice per AA page), and both are external today purely because they
are large. The SDK says as much for the framebuffer: *"DRAM is faster than cache-backed PSRAM
and the framebuffer is touched heavily during composition, so it stays off for every other
device"* ([freeink-sdk/README.md:386-390](../freeink-sdk/README.md)) — a policy that
`FREEINK_FB_PSRAM=0` expresses but `malloc` then silently overrides.

Magnitude is **unmeasured**, and the one timing breakdown available suggests it is not the
dominant term: the deferred AA pass is `planes=80 ms gray=473 ms restore=52 ms ≈ 605 ms`
([background-rendering.md:76-78](background-rendering.md), logged at
`EpubReaderActivity.cpp:1084`), i.e. dominated by the 473 ms panel waveform, not by memory.
Page turns are waveform-bound on the S3 too: pre-render hit ≈ 580 ms vs miss ≈ 670 ms
(`background-rendering.md:80-82`) — the whole render compute is only ~90 ms.

So: do **not** assume pulling the framebuffer back to internal is a win. Measure it (§7.1).
It is listed in §5 as a measurement, not a change.

---

## 5. Proposal, in priority order

Design rules for all of it:

- **Optional, never required.** Gate on `#if defined(BOARD_HAS_PSRAM) && BOARD_HAS_PSRAM`
  plus a runtime `esp_psram_get_size()`/allocation-success check. Every path must degrade to
  today's behaviour when the allocation fails. C3 preprocessor output must be unchanged.
- **Reuse the existing seam.** `MemoryManager` already has `MemPool{Internal,Psram,Default}`,
  `capsFor()`, PSRAM-capable `arenaCreate/arenaAlloc`, pool-aware `freeBytes`/
  `largestFreeBlock`/`minEverFree`, and an evictable-sink registry
  ([freeink-sdk/libs/hardware/MemoryManager](../freeink-sdk/libs/hardware/MemoryManager)).
  It is currently linked by nothing in `src/`. Adopt it rather than inventing a parallel
  allocator.
- **Sort by temperature, not size.** Cold + large + rebuildable → PSRAM. Hot, DMA-facing, or
  touched-per-pixel → internal.

### P0.1 — Correct the gates (no PSRAM spending, removes dead code)

Add `internalFree()` / `internalContig()` helpers (or `MemoryManager::freeBytes(MemPool::Internal)`)
and point the reader/render floors at the pool they were written against:
`BG_BUILD_*`, `RESIDENT_BUILD_ABORT_*`, `IN_PLACE_BUILD_*`, `PRE_RENDER_MIN_FREE_HEAP_BYTES`,
`EMBEDDED_STYLE_MIN_FREE_HEAP_BYTES`, `MIN_FREE_HEAP_FOR_INDEXING_POPUP`,
`WARMUP_MIN_CONTIGUOUS`, `heapAllowsBatchSizes` (`BookMetadataCache.cpp:297-299`),
`Epub::spineStats_`'s gate (`Epub.cpp:1698-1705`), and `maybeRestartForFragmentedHeap`.
On C3 the two pools are the same allocator, so the C3 numbers do not move.
Effort **M** (mechanical, many sites). Win: the gates start meaning something again; the
fragmentation-recovery reboot stops being unreachable.

### P0.2 — Stop borrowing the framebuffer on PSRAM boards

Give Background-B and Background-C a dedicated PSRAM build arena instead of the secondary
framebuffer. Size it from the existing budgets: parse 10,240 + extract chunk 8,192 + inflate
ring ≤32,768 ≈ **~52 KB**, i.e. the same budget the borrowed region provides
(`Section.cpp:690-735`, `Section.cpp:1083-1104`). Wire it through the existing
`setExternalBuildScratch()` / `BuildArena(external, size)` seam — no new mechanism.

Consequences, all of them wins and none of them needing new caches:

- `secondaryBufferDegraded_` never gets set by B/C → AA stays on, refreshes stay on the
  fast/half cycle instead of forced `FULL_REFRESH`.
- A page turn no longer has to destroy a live build. `endBackgroundBorrow`'s teardown, the
  preemption counter and `BG_BUILD_MAX_PREEMPTIONS` stop firing on PSRAM boards, so B
  actually finishes the spine and the chapter-crossing freeze goes away (the ~950 ms parse is
  paid once, in idle slices, instead of up to three times and then handed to C).
- Same for `HomeActivity`'s cover-decode release and `NetworkMemoryTrim`.

This is the highest-confidence item in the document: it *removes* work rather than adding a
cache, and it directly answers `display-capability-audit-2026-08-17.md:151-154`.
Effort **M**. Risk: the arena must be reset per build (`initArena()` already calls
`external->reset()`); the borrow path must stay compiled and exercised for C3.

### P1 — PSRAM frame cache: the layer between the SD cache and the framebuffer

Today Background-A pre-renders exactly **one** page, directly into the write framebuffer
(`renderPreRenderPass`, `EpubReaderActivity.cpp:3216-3273`), and it is destroyed by anything
else that draws — including the deferred AA pass, which is why *"for roughly 600 ms after every
page the reader has nothing pre-rendered"* (`background-rendering.md:80`). Back-navigation and
jumps have no pre-render at all.

Proposal: an N-entry PSRAM cache of finished page planes, keyed by
`(spineIndex, propertyHash, pageIndex, layoutHash)` — `propertyHash` already exists for
section cache variants (`Section.cpp:301-319`). Each entry stores the BW plane plus, when AA
is on, the two grayscale planes:

| panel | BW | +2 AA planes | per page | ±4 pages |
|---|---:|---:|---:|---:|
| X4 Pro 800×480 | 48,000 | 96,000 | **144,000** | 1.15 MB |
| T5S3 960×540 | 64,800 | 129,600 | **194,400** | 1.56 MB |

Of 8 MB that is affordable even alongside LilyGo's 518 KB canvas. A hit becomes a `memcpy`
into the framebuffer plus the waveform.

Be honest about the size of the win: turns are waveform-bound, so a hit saves the render
compute only — **~90 ms** on the measured S3 figures (580 ms hit vs 670 ms miss). The real
value is (a) back-nav and TOC jumps, which currently always pay full render, (b) removing the
600 ms post-AA window where nothing is staged, (c) letting look-ahead go N pages deep so a
fast reader stops outrunning it. Effort **M**. Must invalidate on font/margin/AA/orientation
change — the `layoutHash` component is not optional.

### P2 — SD font resident image in PSRAM

This one has a named beneficiary the flash cache cannot serve. The font flash cache is the
`spiffs` partition (`FlashFontPartition.cpp:37-38`): `0x360000 = 3,538,944 B`
([partitions.csv:6](../partitions.csv)) minus `LANG_RESERVED_BYTES = 65,536`
(`FlashFontPartition.h:77`) = **3,473,408 B usable**, minus a 776 B header. Against the
shipped assets (`assets/sd-fonts/fonts.json`):

- `NotoSerifCJK` 12 pt = **3,680,444 B** → **does not fit at all**. That reader is on the
  pure-SD per-glyph path forever.
- `NotoSerifCJK` 10 pt = 2,615,442 B → fits alone, leaves no room for a second size or family.
- Font *previews* deliberately bypass the flash cache (`FlashCachePolicy::ReadOnly`,
  `SdCardFontManager.cpp:141-147`) because populating it erases the partition and costs
  "seconds" plus an erase cycle — so browsing the font list is always pure-SD.

A PSRAM image of the one selected `.cpfont` removes the per-glyph SD metadata+bitmap reads
(`SdCardFont.cpp:1497-1510, 1583-1601`: one 16-byte read per glyph plus one bitmap read per
glyph, up to `MAX_PAGE_GLYPHS = 512`) with no flash wear and no erase latency. The code shape
already exists: `loadFromMmap()` (`SdCardFont.cpp:897-940`) serves intervals/metrics/bitmaps
from a mapped base pointer — a PSRAM image is the same pointer from a different source. Keep
the alignment-sensitive interval/ligature copies as they are.
Effort **L** (new image owner + source abstraction in `SdCardFontManager`), win largest for
CJK and for font browsing. Note per-glyph rasterisation then reads PSRAM instead of internal;
measure (§7.3) before preferring it over a flash-cache hit.

### P3 — Sections resident in PSRAM

Let Background-B build two or three sections ahead and keep their serialized page blobs in a
PSRAM LRU keyed by `(spineIndex, propertyHash)`, sized from the section file's own byte span
(`lut[i+1] - lut[i]`, `Section.cpp:1848-1867`). Removes the SD seek + deserialize on a chapter
boundary. Only worth doing *after* P0.2, because P0.2 is what lets B finish a section at all.
Effort **L**.

---

## 6. Explicit non-opportunities

- **"Copy the secondary buffer to PSRAM instead of clearing it."** Obsoleted by P0.2: with a
  PSRAM build arena nothing borrows the framebuffer, so there is no content to park. And the
  byte loss is not why a restored secondary is unusable — `reallocSecondaryBuffer()` already
  copies the live frame in, then sets `_redBaselineAuthoritative` so the next FAST refresh
  deliberately **ignores** it and diffs against the controller's RED RAM
  (`FreeInkDisplay.cpp:455-518`). Parking a copy would add a 48 KB copy and change nothing.
  (Aside: [secondary-buffer-management.md](secondary-buffer-management.md) described realloc
  as initialising to white, and told callers to reseed RED RAM from it afterwards. Corrected
  along with the two stale reader comments that repeated the claim — see §9.)
- **Task stacks in PSRAM.** `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` is set, but the TCB
  must stay internal and an external stack may not run with the flash cache disabled
  (`esp32s3/include/freertos/.../freertos_idf_additions_priv.h:284-287`). The render task
  (10,240 B, `ActivityManager.cpp:81-91`), button sampler (2,048/4,096 B, `HalGPIO.cpp:585-587`)
  and framework tasks all touch flash/SPI/I2C paths. Keep internal.
- **DMA buffers in PSRAM.** `MALLOC_CAP_DMA` is internal-only by definition. The 4,096 B
  SDMMC bounce buffer stays where it is (`SdmmcBlockDevice.cpp:67-72`).
- **Anything already explicitly `MALLOC_CAP_SPIRAM`.** LilyGo canvas/planes, IT8951 planes,
  Paper Mono's eight buffers, X4 Pro `_grayBase`. Wrapping them in a second abstraction buys
  no placement change.
- **Small/threshold-bound buffers.** Scaled-glyph cache (1,280 + 3,584 B,
  `GfxRenderer.h:128-129`), mini-kern matrix (<1 KB, `SdCardFont.cpp:455-462`), the 8-entry
  glyph overflow ring (`SdCardFont.h:224-246`), dictionary scan buffers (1,024 B), the 4,096 B
  serialization/ZIP/storage buffers. All are hot lookups or already internal; moving them
  trades latency for KBs that are not scarce.
- **Replacing the chunked `storeBwBuffer` with one contiguous allocation.** The 8,000 B
  chunking with 4,096/2,048/1,024 retries exists to avoid demanding a contiguous 48 KB block
  (`GfxRenderer.cpp:3413-3416, 3531-3558`). It is cheap and it is the C3's safety net.
- **A user-facing PSRAM/memory setting.** Placement is an implementation decision; exposing it
  would make C3/S3 behaviour diverge per user with no reading benefit.
- **Whole-font or all-preview PSRAM residency for every family.** `SdCardFontManager` loads
  only the selected size (`SdCardFontManager.h:27-30`); previews already persist as SD
  sidecars (`FontPreviewCache`, ~11,980 B for a 460×207 strip). One selected font image (P2)
  is the bounded version of this idea; "all of them" is 3.68 MB × N.

---

## 7. Measurement plan and outcomes

Runs were performed on `env:x4pro` (`LOG_LEVEL=2`) and, where the panel differed, on
`env:lilygo_t5s3`. Completion status and remaining work are listed per item.

1. **Placement census.** Run on the X4 Pro on 2026-09-18; results are retained in §9,
   "Placement census: device results". The temporary `DEBUG_MEMORY_PLACEMENT` hook logged
   pointer, size and pool for large allocations and was removed after the census.
2. **Internal vs external framebuffer A/B.** Run on X4 Pro on 2026-09-29; results and
   limitations are in §10. The test compared internal-heap telemetry and existing page/AA
   timing lines.
3. **Font path timing.** `FDC`/`SDMGR` timings for a page prewarm on (a) flash-mmap hit,
   (b) pure SD, (c) a prototype PSRAM image, using `NotoSerifCJK` 12 pt — the size that cannot
   use the flash cache at all. Decides P2's ordering against the flash cache.
4. **Background-B completion rate.** Count `Background-B: returned secondary buffer (… build
   discarded …)` and `preemptions=2` occurrences over a chapter-crossing read, before and
   after P0.2. This is the acceptance criterion for P0.2. **Met on device 2026-09-18** (§9,
   "Placement census: device results", second pass): 8 completed background builds with
   `borrow=0` and four spines built inside the PSRAM arena.
5. **PSRAM telemetry in `SystemStatus`.** Implemented (§9) — this was the one §7 item that
   needed no device run.

---

## 8. Decision requested

1. Adopt **P0.1 + P0.2** as the PSRAM policy: *spend PSRAM to delete the C3 scarcity
   choreography on S3 boards, and point the floors at the pool that is actually scarce.* This
   supersedes "enable but don't spend", which the build has not implemented since
   `BOARD_HAS_PSRAM` landed.
2. Route new placement through `MemoryManager`'s existing `MemPool` API rather than a new
   allocator in `lib/Memory/Memory.h`.
3. Treat **P1/P2/P3** as separately-justified features, each gated on its §7 measurement.
4. Keep `FREEINK_FB_PSRAM` at 0 for these boards until §7.2 says otherwise — and note that
   the flag currently does not control what it appears to control.

---

## 9. Implementation log

What has actually landed on `psram`, in commit order. Each entry names the decision taken
where it differs from §5/§8 above.

### P0.1 — heap gates point at the internal pool

`lib/Memory/HeapPools.h` provides `internalFreeBytes()` / `internalContigBytes()` /
`internalMinEverFreeBytes()`: the `MALLOC_CAP_INTERNAL` query under `BOARD_HAS_PSRAM`, and
byte-identical to the previous expressions otherwise, so the C3 does not move. Every heap
*decision* gate now goes through them: the reader's pre-render and Background-B/C floors,
both `RESIDENT_BUILD_ABORT_*` pairs, `heapAllowsInPlaceBuild`, every
`maybeRestartForFragmentedHeap` input, `Section::heapAllowsEmbeddedStyle`, the Epub
CSS-compile and `spineStats_` gates, `BookMetadataCache::heapAllowsBatchSizes`, the four
`ChapterHtmlSlimParser` gates, `FontSelectionActivity`'s warm-up brake and
`rejectIfLowMemory()` in the web server. No constant changed.

Corrections to §3 found while implementing:

- `ESP.getFreeHeap()`, `ESP.getMinFreeHeap()` and `ESP.getMaxAllocHeap()` already query
  `MALLOC_CAP_INTERNAL` in this framework
  (`framework-arduinoespressif32/cores/esp32/Esp.cpp:159-174`), so gates written against
  them were never reading PSRAM. The genuinely wrong queries are `esp_get_free_heap_size()`
  and the `MALLOC_CAP_8BIT` / `MALLOC_CAP_DEFAULT` largest-block calls. Sites still using the
  Arduino wrappers (Dictionary, `SleepActivity`, `SdCardFont`, the image converters) were
  therefore left alone.
- Under the old allocator preference, these gates deliberately stayed on the combined pool.
  P0.3 changes ordinary allocation placement to internal, so `HalDisplay::fbufContig()` and
  the KOReader TLS preflight/telemetry now use `internalContigBytes()` as well.
- Telemetry keeps the combined queries; `SystemStatus` reports both pools separately instead
  (below).

The host stubs (`test/epub_pipeline/esp_{heap_caps,system}.h`) now derive every heap query
from the single `ESP` stub knob, so `ESP.setFreeHeap()` moves all gates together.

### P0.2 — background builds use a PSRAM arena, not the framebuffer

On `BOARD_HAS_PSRAM`, Background-B and Background-C build inside a dedicated 52 KB external
arena (`PSRAM_BUILD_SCRATCH_BYTES`, allocated on first use and held for the reader session)
installed through the existing `setExternalBuildScratch()` seam. Consequences, which are the
point of §2: the secondary framebuffer stays resident, so AA stays on and no page is forced
onto `FULL_REFRESH`; `endBackgroundBorrow()` is framebuffer-only and therefore a no-op, so a
page turn no longer discards a live Background-B build and `BG_BUILD_MAX_PREEMPTIONS` stops
firing; Background-C needs no RED-RAM reseed or single-buffer opt-in on this path.

Details worth knowing before touching it:

- The resident-build low-heap aborts skip an arena-backed build — its working set is
  external, so it is not what is starving internal RAM.
- B→C adoption transfers arena ownership, and `chooseSectionBuildMode()` then reports
  `IncrementalResident`, because the framebuffer was never taken.
- Font-slot sharing (`ScopedSlotArena`) is restricted to the current section's arena. With a
  borrow that was implicit (a borrow is always returned before a render); a PSRAM arena
  survives page turns, and `BuildArena` only releases newest-first.
- The Section must release its parser and arena blocks before the caller destroys the arena
  wrapper or reuses its backing region. `setExternalBuildScratch(nullptr)` (or a different arena)
  now aborts a live build using the old external arena before returning; setting the same pointer
  preserves it. This covers PSRAM cancellation and framebuffer handback with the same lifetime
  rule. Background-B also refuses a second scratch acquisition while its PSRAM arena is active.
- Every path falls back to today's borrow/release choreography when the external allocation
  fails, and compiles to `return false` on a board without PSRAM.

Acceptance (§7.4) still has to be measured on hardware.

### Telemetry — the two pools are reported separately

`SystemStatus` gained `psramTotalBytes` / `psramFreeBytes` / `psramMinFreeBytes` /
`psramMaxAllocBytes` (0 on a board without PSRAM, so the surfaces hide the row), and its
internal triple now comes from one pool: `maxAllocHeapBytes` used to be a `MALLOC_CAP_8BIT`
largest block, i.e. a ~4 MB external number sitting next to ~200 KB of free internal RAM.
`/api/status`, `/api/status/fast`, `HomePage.html` and `SystemInformationActivity` all show
both rows; `STR_MEM_COMBINED` was replaced by `STR_MEM_INTERNAL` / `STR_MEM_PSRAM`.

### Decisions taken against §8

1. **§8.1 adopted** as written: P0.1 + P0.2 are the PSRAM policy.
2. **§8.2 declined.** `MemoryManager` (`freeink-sdk/libs/hardware/MemoryManager`) is an
   unmodifiable submodule whose `MemPool` API is `std::function`-based with a registry no
   caller uses; routing placement through it would add indirection on the C3 for no
   placement change. P0.2 instead reuses `BuildArena` — already in-tree, already the arena
   the borrow path installs — and a header-only `lib/Memory/HeapPools.h` that compiles to
   the pre-existing expressions on the C3.
3. **§8.3 holds:** P1/P2/P3 are unimplemented and each still gated on its §7 measurement.
4. **§8.4 holds:** `FREEINK_FB_PSRAM` is untouched at 0.

### Release sites outside the reader build

The five paths that freed the secondary framebuffer to make room for something else
(Home's cover decode, `ReaderActivity`'s first-open indexing, `NetworkMemoryTrim`,
Calibre Connect, SerialTransfer's low-heap trim) now ask
`freeingReclaimsInternal(renderer.getFrameBuffer())` first. All five want internal RAM,
and the framebuffer is external on these boards, so the release cost them AA and a
full/half waveform in exchange for bytes nothing could use. The probe is a runtime
`esp_ptr_external_ram()` check, so the release still happens if PSRAM was exhausted and
malloc fell back to internal RAM. `SleepActivity`'s pre-deep-sleep release is untouched:
terminal, nothing renders after it.

### Placement-census instrument (removed after measurement)

The temporary `lib/Memory/PlacementCensus.h` helper and its `NOTE_PLACEMENT` callsites logged
pointer, size, and `EXTERNAL`/`internal` pool for large allocations. It was enabled only by
`DEBUG_MEMORY_PLACEMENT=1` on PSRAM builds. The helper and callsites have been removed; the
device evidence below is retained as historical verification.

### Placement census: device results

X4 Pro, `env:x4pro` with `DEBUG_MEMORY_PLACEMENT=1`, 2026-09-18. First pass: boot, open a
previously-read EPUB, ~10 page turns including image pages, back to Home, reopen.

```
[   1052] PLACE framebuffer@boot      48000 B @ 0x3c5c2f94 EXTERNAL
[  79661] PLACE framebuffer@reader    48000 B @ 0x3c5c2f94 EXTERNAL
[ 166959] PLACE build arena backing   10240 B @ 0x3c5e76fc EXTERNAL
[ 167113] PLACE build arena backing    6988 B @ 0x3c5ea000 EXTERNAL
[ 207742] PLACE framebuffer@reader    48000 B @ 0x3c5ceb98 EXTERNAL
```

What this settles:

- **§1's proof chain holds on silicon.** Every buffer above `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
  landed in external RAM with no explicit capability flag: the framebuffer (`0x3c5c2f94`),
  the 10,240 B section parse arena and the 6,988 B ZIP arena. §4's premise — the framebuffer
  is *already* in PSRAM and internal RAM is the scarce pool — is now measured, not deduced.
- **The release/realloc path stays external.** After Home and a reopen the framebuffer probe
  reports a *different* pointer (`0x3c5ceb98`) that is still `EXTERNAL`, so a mid-session
  realloc does not silently migrate the frame into internal RAM.
- **P0.2's premise is confirmed; the arena path itself is exercised in the second pass below.**
  `borrow=0` for the whole session and `Background build spine=28 complete: 9 pages` after
  `B runs=6 completes=1`: the background build finished across six slices and several
  navigation preemptions (`AA replay skipped: page preempted by navigation`) without ever
  taking the framebuffer. This first book reached its spine through the resident path, so
  `acquirePsramBuildScratch()` was not called here.
- **Idle external headroom is real.** `PSRAM: Free: 8,214,528 / Total: 8,388,608` at rest,
  dropping only to `Min Free: 8,206,844` across the whole session: ~7,700 B of external RAM
  consumed at peak, against 8.2 MB idle. The telemetry added in §9 is what reports this.

Second pass: a book opened for the first time on this firmware, so the section and `.pxc`
caches were cold.

```
[ 368424] PLACE inflate ring           5774 B @ 0x3c5e9d00 EXTERNAL
[ 368731] PLACE inflate ring           6362 B @ 0x3c5e76fc EXTERNAL
[ 369340] PLACE build arena backing   10240 B @ 0x3c5e76fc EXTERNAL
[ 369569] PLACE inflate ring           4096 B @ 0x3fcc8a6c internal
[ 372579] PLACE build arena backing   10891 B @ 0x3c5ea000 EXTERNAL
[ 374163] PLACE psram (explicit)      53248 B @ 0x3c5e76fc EXTERNAL
[ 374164] PLACE external build arena  53248 B @ 0x3c5e76fc EXTERNAL
```

- **P0.2 works as designed, on hardware.** Four consecutive spines built inside the
  dedicated arena while the framebuffer stayed resident:
  `Background-B: building spine 5 in the PSRAM arena (53248 bytes, internalFree=182652
  contig=131060)`, repeated for spines 6, 7 and 8, each followed by a
  `Background build spine=N complete` line (2, 3, 3 and 40 pages). Session counters ended at
  `B runs=45 completes=8 | C runs=2 completes=1` with **`borrow=0` throughout** — eight
  completed background builds and not one framebuffer borrow. The pre-P0.2 failure mode this
  replaces is a borrow that ends in `build discarded`. The arena is allocated once
  (`psram (explicit) 53248 B`) and re-viewed by each subsequent `external build arena` line at
  the same address, which is the intended alloc-once-reuse behaviour.
- **The 4,096 B threshold is exact, and visible.** Two inflate rings of exactly 4,096 B landed
  `internal` (`0x3fcc8a6c`, `0x3fcc926c`) while 5,774 B and 6,362 B rings went `EXTERNAL`.
  `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=4096` is therefore inclusive: allocations of *exactly*
  the threshold size are still served from internal RAM. Any future sizing decision that lands
  on 4,096 B should assume internal placement.
- **Background-C also stays resident.** `Background-C: building spine 0 incrementally, buffer
  resident` — the C path reached its build without releasing the secondary buffer either.

Third pass: a freshly-added book with uncached JPEGs, plus a deliberate failed open.

```
[393749] PLACE external build arena  48000 B @ 0x3c5c2f94 EXTERNAL
[393870] PLACE TJpg work pool        12288 B @ 0x3c5c2f98 EXTERNAL
[408958] PLACE build arena backing   32751 B @ 0x3c5f7404 EXTERNAL
[ 79033] PLACE inflate ring          24296 B @ 0x3c5e9d00 EXTERNAL
[ 79690] PLACE inflate ring          27880 B @ 0x3c5e9d00 EXTERNAL
[ 97328] SCT createSectionFile spine=9 arena: cap=53248 highWater=23164 failedAlloc=0
```

- **The decode path is external too.** The 12,288 B TJpgDec work pool, the ~32 KB ZIP arena
  and inflate rings up to 27,880 B all landed `EXTERNAL`. No decode-path buffer competes for
  internal RAM on this board.
- **The PSRAM arena is being used for the builds that need it.** `spine=9 arena: cap=53248
  highWater=23164` — a section whose parse genuinely exceeded the 10,240 B owned arena, served
  by the dedicated external arena rather than by borrowing the framebuffer.
- **Gap found: the image warm pass still borrows the framebuffer.** `external build arena
  48000 B @ 0x3c5c2f94` is the *framebuffer's own address* — this is
  `EpubReaderActivity.cpp`'s warm-scratch path (`borrowSecondaryBuffer` feeding
  `warmScratch`), which P0.2 did not convert. The TJpgDec pool is then carved from it
  (`0x3c5c2f98`, four bytes in). On a PSRAM board this borrow is unnecessary for the same
  reason B's was: an external arena is not display state. See "Still open".
- **A failed open is not a memory failure.** The book that would not load reported
  `ZIP: File too small to be a valid zip` → `EBP: Could not find content.opf` →
  `READER: Failed to load epub`. A truncated/invalid EPUB file, unrelated to placement; the
  reader degraded cleanly and the next book opened normally.

Still not observed: AA capture planes, BW snapshot chunks, pixel-cache band, cover extraction
chunk and SD-font page cache. Those tags are present in the ELF (verified by `strings`).

### P0.2 follow-up: the image warm pass stops borrowing too

The third pass showed the warm/decode scratch still taking the secondary framebuffer on a
PSRAM board. `renderContents()` now allocates its own `PSRAM_BUILD_SCRATCH_BYTES` block via
`psramAllocBytes()` and views it with a `BuildArena`, exactly as Background-B/C do. The
storage is separate from `psramScratchStorage_` on purpose: a background build may be holding
that arena, and two arenas must never overlap one region. The borrow and the
release-and-realloc paths both remain as fallbacks — taken when PSRAM is exhausted, and
unconditionally on the C3, where the whole block compiles out.

Why this matters beyond the borrow itself: the release fallback is the path that can reach
`reallocSecondaryEvictingCaches()`, evict font caches, and escalate to a fragmented-heap
restart. Keeping the buffer resident removes that escalation from every image page.

Device-verified on the X4 Pro (`DEBUG_MEMORY_PLACEMENT=1`) after the change:

```
[ 91311] PLACE psram (explicit)      53248 B @ 0x3c5e76fc EXTERNAL
[ 91312] PLACE external build arena  53248 B @ 0x3c5e76fc EXTERNAL
[ 91347] ERS   Image warm scratch: 53248 byte PSRAM arena, framebuffer RESIDENT
[ 93628] PLACE TJpg work pool        12288 B @ 0x3c5e7700 EXTERNAL
```

The TJpgDec pool is now carved from `0x3c5e7700` — inside the dedicated arena — where before
it sat at `0x3c5c2f98`, four bytes into the framebuffer. No `external build arena 48000 B`
line appears, so the framebuffer was never lent during the decode.

Fourth pass: a generated 1.44 MB, eight-spine probe EPUB with dense justified text, large
JPEGs, a PNG, repeated image reuse and a 1200x1600 cover. The book completed end-to-end on
the X4 Pro using the flash-cached `Bitter@16` reader font.

```
[ 93859] PLACE build arena backing    10240 B @ 0x3c5e76fc EXTERNAL
[ 94005] PLACE build arena backing    19502 B @ 0x3c5ea000 EXTERNAL
[168971] PLACE psram (explicit)       53248 B @ 0x3c5e76fc EXTERNAL
[168971] PLACE external build arena   53248 B @ 0x3c5e76fc EXTERNAL
[169688] PLACE TJpg work pool         12288 B @ 0x3c5e7700 EXTERNAL
[391915] PLACE inflate ring           22610 B @ 0x3c5e89a8 EXTERNAL
```

This pass adds three useful negative results:

- `SDMGR Flash cache hit for Bitter@16` followed by `Font loaded from flash: Bitter@16`
  confirms the production font path. The `sd font page cache` hook correctly does not fire:
  `loadFromMmap()` serves the mapped flash image and `SdCardFont::prewarm()` deliberately
  skips `miniBitmap` on that path.
- The PNG path wrote both 456x638 pixel caches (72,738 B each), proving `PixelCache::begin()`
  allocated and used its band. No `pixel cache band` line appeared because the census filters
  allocations below 4,096 B; a successful cache write plus no hook output bounds this
  transient band below that threshold. It is not a PSRAM opportunity.
- The X4 Pro uses the staged AA path (`Inline AA` / `AA replay`); it does not advertise
  `supportsGrayFrame()`. The two full-page `AA capture` allocations are therefore T5S3-only,
  not missing X4 observations.

### P0.3 — ordinary allocations stay internal; cold cache growth spills explicitly

The S3 build wraps `heap_caps_malloc_extmem_enable()`, called by ESP-IDF's early PSRAM system-init
hook (`CONFIG_SPIRAM_BOOT_INIT`) and again by Arduino's `psramAddToHeap()`. PSRAM remains
registered, but neither caller can enable the automatic `malloc()`/`new` fallback. The S3 linker
also wraps application/library calls to raw `heap_caps_malloc/calloc/realloc/aligned_alloc/aligned_calloc`
APIs: requests gain `MALLOC_CAP_INTERNAL` unless they explicitly include `MALLOC_CAP_SPIRAM` or
`MALLOC_CAP_SPIRAM_NO_ENC`, preventing generic `MALLOC_CAP_8BIT`/`MALLOC_CAP_DEFAULT` requests
from spilling into PSRAM. Ordered `heap_caps_*_prefer` APIs remain caller-directed, and ESP-IDF
private allocation calls retain their own capabilities. These wrappers are S3-only, so C3
allocation behavior is unchanged.

`EpubImageManifest::records_` is the cache-overflow path: it grows in internal RAM first. If
internal growth fails, the sorted fixed-record array moves to explicit PSRAM and stays there for
later growth. If PSRAM also cannot grow it, the existing best-effort behavior remains: dimensions
still serve the current build, but the new record is not cached. The cap is 65,535 records ×
12 bytes (~768 KiB).

The 52 KiB background-build and image-warm arenas remain explicit PSRAM allocations, with their
existing borrow/release fallback. Ordinary `malloc`/`new` and generic capability requests still
target internal RAM under P0.3; explicitly placed buffers remain external. The X4 Pro facade
framebuffer now explicitly requests PSRAM by default (§10), so the earlier ordinary-framebuffer
placement check no longer describes its default firmware. Broader P0.3 hardware confirmation
remains pending.

### Still open

- §7.1 now covers every large allocation exercised by the X4 probe except the transient BW
  screenshot chunks and first-open cover-extraction chunk. AA capture is T5S3-only; the X4
  pixel band is below the census threshold; the production flash-mapped font allocates no SD
  page cache. A T5S3 pass is still needed for its AA planes.
- §7.4 (Background-B completion) is **met**: 8 completed background builds, `borrow=0`, four
  of them explicitly inside the PSRAM arena. A before/after count against a pre-P0.2 build
  was not taken, so the improvement is demonstrated rather than quantified.
- §7.2 (internal vs external framebuffer A/B) is **complete**; see §10. §7.3 (font path timing)
  remains open.
- P1 (resident page-plane cache), P2 (one selected font image), P3 (sections resident in PSRAM).

## 10. X4 Pro framebuffer A/B and default

The internal-RAM and PSRAM X4 Pro builds both reported `2.36-x4pro`. Placement markers confirmed
that the framebuffer was internal in the former and external (PSRAM) in the latter. The log
samples were not paired by spine, page, refresh mode, and prerender state, so the timing medians
below are descriptive only.

| Measurement | Internal framebuffer | PSRAM framebuffer |
|---|---:|---:|
| Startup internal free heap, median | 93,120 B (5 readings) | 191,432 B (2 readings) |
| Largest internal free block, median | 51,188 B | 147,444 B |
| Lowest internal watermark in logs | 18,164 B | 62,124 B |
| Timed FAST page render, median | 1,345 ms (9) | 1,329 ms (39) |
| Timed HALF page render, median | 2,342 ms (4) | 2,189 ms (3) |

The PSRAM run retained substantially more internal-heap headroom. Its captured log had no
AutoSync "internal heap too tight" wake-pull skips, compared with 35 in the internal build; the
log windows and workloads differ, so these counts are observations, not a rate comparison.
No matched timed page/mode pair existed, so the small median render-time differences do not
establish a speed change. Following the test, `platformio.ini` opts every X4 Pro build into
`FREEINK_FB_PSRAM=1`.

The placement census used for this comparison has been removed from firmware source; this
record preserves the measured results without shipping its test-only logging.

## 11. KoSync extraction and background-work coordination (2026-09-29)

KoSync's chapter XPath mapper now supplies a short-lived explicit `BuildArena` backed by
`psramAllocBytes()` on PSRAM boards. The reserved region is about 33 KiB: the maximum 32 KiB
inflate ring, the 1 KiB reader chunk, and alignment. The arena is reclaimed when the synchronous
chapter extraction returns.

If PSRAM allocation or arena setup fails, the stream reader uses its existing heap path. If an
explicit arena refuses before `EntryReader` opens, it retries on the heap; failures after output
starts are returned without replaying partial output. The `BOARD_HAS_PSRAM` guard leaves C3's
allocation path unchanged. The KoSync TLS internal-heap admission gate is unchanged.

While the automatic sync worker is busy, the reader pauses Background-A re-arming/dispatch,
image warming, and Background-B slices. It preserves an active B build, visible-page rendering,
deferred grayscale work, and Background-C for the section being read. When sync becomes idle,
speculative work can resume.

Verification: the `StreamReadArenaTest` arena-output and heap-fallback cases passed, and both
`x4pro` and `default` firmware builds succeeded. The host test covers the stream arena/fallback,
not the KoSync PSRAM allocator or reader scheduler. No post-change device placement trace or
reader/sync runtime run was taken; actual PSRAM placement and scheduling remain unverified.
