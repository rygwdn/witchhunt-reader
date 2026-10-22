# Runtime and EPUB-Embedded Font Support — Implementation Plan

> Proposal derived from a read-only feasibility investigation. No implementation changes are included in this plan.

**Goal:** Allow users to select SD-card TTF/OTF fonts on PSRAM-equipped boards, then support EPUB-declared embedded fonts as a book-scoped typography option without destabilizing the existing reader-font override or section-cache behavior.

**Scope decision:** Custom typography and EPUB rendering are explicitly in scope (`SCOPE.md:17-24`). Runtime vector fonts should be PSRAM-board-only initially; retain `.cpfont` for ESP32-C3/no-PSRAM boards. Embedded font support is a separate, later feature and must not be implied by sideloaded-TTF support. Its memory, cache, and shaping costs require a separate acceptance gate before implementation.

## Architecture and constraints

- **Existing renderer contract:** The renderer consumes `EpdFontFamily` / `EpdFontData` glyphs; the active SD font manager loads `.cpfont` (`src/SdCardFontSystem.cpp:104-123`, `lib/EpdFont/SdCardFontManager.cpp`). Reuse that contract with an adapter that faults glyphs from `FtFont`; do not create a second text renderer.
- **SDK is not integration:** FreeInkFont exists under `freeink-sdk/`, but `platformio.ini:111-151` does not link it into the app. Add and configure it only for supported boards.
- **PSRAM gating:** X4 Pro config defines `BOARD_HAS_PSRAM` (`platformio.ini:362-393`). Keep vector code, cache owners, and dependency out of non-PSRAM builds. Do not remove the gate for C3 without a measured alternative memory design.
- **Renderer lifecycle:** Font faces and glyph caches must remain alive as long as renderer registrations reference them. Serialize load, resize, cache mutation, and unload with rendering and background section construction; honor `RenderLock` and existing resource-lending rules.
- **Bounded memory:** Stream large font files from SD using `FtFont` random-access reads; avoid whole-file allocations unless a measured, explicit resident-size budget permits them. Bound each glyph arena, face count, source count, style count, and any working cache. Define graceful cache eviction/fallback behavior on allocation failure. Count FreeType task-stack requirements as well as heap.
- **Typography limits:** Font rasterization does not implement complex-script shaping. The first stage covers the engine's current text model and must not claim new bidi/shaping support.
- **EPUB model:** CSS currently ignores `@font-face` (`lib/Epub/Epub/css/CssParser.h:30-34`); `CssStyle` has no family property (`lib/Epub/Epub/css/CssStyle.h:70-176`); `TextBlock::RenderStyle` has a single block heading font ID and no per-word family (`lib/Epub/Epub/blocks/TextBlock.h:72-76`). Supporting mixed inline families may require a compact, versioned text-block/section-cache format change.
- **Input and resource safety:** EPUB font URLs are untrusted ZIP paths and font bytes are untrusted parser input. Normalize and constrain paths to the EPUB container; bound extraction and parsing; reject malformed resources without failing book opening. Handle IDPF/Adobe font obfuscation; do not treat obfuscated fonts as DRM decryption.

## User-visible behavior

### Stage 1 — SD vector-font families

1. On X4 Pro (and any explicitly enabled PSRAM board), discover `.ttf` and `.otf` files in the existing SD font roots and expose them through the existing font-family setting. Support `.ttc` only if `FtFont` family-index selection is verified; otherwise leave TTC undiscovered with a clear documented limitation.
2. Support a single regular source and optional regular/bold/italic/bold-italic files using existing `.cpfont` family selection semantics. Synthesize missing bold/italic only where the backend supports it.
3. Support current reader size controls; define the available vector size ladder and ensure preview, selected value, and loaded size agree.
4. Preserve existing built-in and `.cpfont` behavior on every board. On non-PSRAM boards, vector families must not appear as selectable fonts.
5. On load/resize failure, retain a usable current or built-in font and report the failure; never leave a renderer ID pointing at a destroyed adapter.

### Stage 2 — EPUB embedded fonts (separately gated)

1. Parse `@font-face` and the supported subset of `font-family`, `src`, `font-weight`, and `font-style`; ignore unsupported URL forms safely.
2. Resolve relative font URLs against the EPUB resource path, extract to a book-scoped cache, and apply EPUB font obfuscation when declared.
3. Permit per-block font-family selection first. Add inline/per-word font-family selection only if real EPUBs require it and the serialized layout design remains bounded.
4. Preserve the user's explicit reader-font override; define whether embedded fonts are opt-in, default-on, or disabled per book before exposing UI.
5. Release book font resources and renderer registrations on book close/change. Invalidate section caches when source font bytes, selected family/style, or effective font identity changes.

## Implementation tasks

### Task 1 — Establish backend and board boundary

- [ ] Confirm scope for SD vector families; keep EPUB embedded fonts explicitly deferred to Stage 2.
- [ ] Verify the FreeInkFont library manifest, source list, allocation hooks, FreeType module configuration, alignment requirements, and target compile compatibility.
- [ ] Make a feature gate derived from PSRAM board capability; ensure C3 builds do not link vector-font objects or retain vector-only state.
- [ ] Measure baseline and post-link firmware size, internal heap, largest free block, PSRAM, and task stack on X4 Pro. Do not set production admission budgets from estimates.
- [ ] Verify C3 and X4 Pro configurations both build after the gate is introduced.

### Task 2 — Add a dynamic glyph adapter

- [ ] Implement an `FtFont`-backed `EpdFontFamily` adapter using the current `EpdFontData` contract and renderer bitmap/coverage/kerning paths.
- [ ] Preserve current glyph dimension, top-offset, advance, baseline, and 1-bit/2-bit bitmap conventions; test boundary glyphs, missing glyphs, fractional advances, and styles against expected geometry.
- [ ] Use streaming read callbacks backed by `HalStorage` for large sources; keep source handles/context alive for every borrowing face. Document and enforce the ownership order.
- [ ] Implement hard-bounded per-style bitmap and lookup caches with cache-clear and memory-pressure release hooks. Avoid allocation during steady-state page turns after warmup where possible.
- [ ] Add renderer registration/unregistration and face ownership APIs. Do not let a renderer map retain callbacks into a destroyed adapter.
- [ ] Serialize all FreeType face access and cache mutations, including background pre-render. Audit every rendering task's stack; do not blindly copy upstream's 24 KiB loop-stack setting to unrelated tasks.

### Task 3 — Integrate SD discovery, settings, and reader lifecycle

- [ ] Extend `SdCardFontRegistry` to identify vector families, supported extensions, style roles, and malformed/unsupported files without confusing them with `.cpfont` families.
- [ ] Extend `SdCardFontSystem` for source open/stream, family load, size change, failure, unload, and board gating while preserving lazy startup and current `.cpfont` behavior.
- [ ] Migrate font-ID resolution, settings option labels, reader-entry loading, font preview, per-book font overrides, size ladders, and UI fallback where applicable.
- [ ] Ensure replacement/deletion of SD source files triggers safe unload and rediscovery; never keep an open source handle to a removed/replaced file across a registry reload.
- [ ] Update user documentation for installation, supported formats, style naming, board support, limitations, and troubleshooting.

### Task 4 — Validate SD vector fonts end to end

- [ ] Add focused tests for source inspection/loading, streamed reads including short reads and seek boundaries, glyph fallback, style resolution, cache ceilings/eviction, and reload/unload ownership.
- [ ] Smoke-run on actual X4 Pro with a small Latin font and a larger supported font; exercise family selection, preview resize, bold/italic text, repeated page turns, memory-pressure transitions, switching back to built-in/`.cpfont`, and SD removal.
- [ ] Record measured load/page-turn latency, peak and steady-state internal heap/largest block, PSRAM, stack high-water, and firmware size. Define acceptable limits before enabling by default.
- [ ] Confirm C3 exposes no vector options and its `.cpfont` and built-in paths remain functional.

### Task 5 — Decide embedded-font data model and policy

- [ ] Inventory representative EPUBs and determine whether block-level selection suffices or mixed inline font families are required.
- [ ] Choose override policy (reader font versus publisher face), supported CSS cascade/weight/style subset, embedded-resource count/size ceilings, cache location/eviction, and behavior on malformed/missing fonts.
- [ ] Specify a compact, versioned serialized layout representation for effective font identity. If per-word font IDs are needed, quantify section-cache growth and memory cost before implementation.
- [ ] Verify EPUB ZIP APIs support bounded extraction to a cache without keeping decompressed font files in RAM. Reuse HAL/storage abstractions.
- [ ] Define font-resource identity and section-cache invalidation, including archive changes and font replacement; do not rely only on CSS family names or a font ID derived from a name.
- [ ] Review IDPF and Adobe obfuscation handling and path normalization. Keep DRM-protected content behavior unchanged.

### Task 6 — Implement EPUB embedded-font support

- [ ] Add a bounded `@font-face` parser/data model and CSS font-family resolution with explicit supported syntax; ignore malformed declarations without aborting the stylesheet.
- [ ] Resolve OPF manifest font resources and CSS-relative URLs inside the EPUB only; reject traversal outside the package namespace and unsupported schemes.
- [ ] Extract/deobfuscate font resources to a book-scoped cache using bounded writes. Add cleanup and interrupted-extraction recovery.
- [ ] Connect effective font family/style to text measurement, line breaking, glyph rendering, and serialized section layout; ensure all these use the same resolved face and metrics.
- [ ] Integrate explicit reader-font override and book lifecycle teardown. Keep old cache versions invalidated safely when the serialized representation changes.
- [ ] Add translations/settings only if the selected override policy requires user-visible controls; avoid a setting if a fixed predictable policy suffices.

### Task 7 — Validate embedded fonts and document limits

- [ ] Add deterministic fixtures covering relative font paths, CSS cascade/fallback, bold/italic matching, missing/malformed fonts, obfuscation, ZIP path traversal, size/count limits, font-cache invalidation, and reader override.
- [ ] Run actual EPUB samples through the device/simulator; compare measured line widths, pagination, section-cache reloads, and resource cleanup.
- [ ] Stress multiple embedded faces under low heap and verify graceful fallback without crashes or renderer dangling references.
- [ ] Document supported CSS syntax, obfuscation formats, device limits, fallback behavior, and explicit non-goals such as complex shaping/DRM.

## Verification status

Plan only. No implementation, build, test, benchmark, or device run performed. The earlier investigation exercised the bundled `FtFont` streamed-rasterization API on the host; that does not verify this firmware's renderer integration or target resource budget.
