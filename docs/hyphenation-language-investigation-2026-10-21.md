# Book-language / hyphenation investigation — 2026-10-21

## Status

Paused at the user's request. **The cause of the reported device behavior is not established.** No firmware source changes or fixes were made during this review/investigation. The outstanding background layout investigation was cancelled.

User observation: hyphenation is already On, but changing Book language in the reader selector has no apparent effect on hyphenation. Treat that observation as unresolved, not disproved by the host or simulator results below.

## Scope reviewed

The uncommitted work adds a per-book language override through:

- `src/activities/reader/EpubReaderMenuActivity.cpp`: language choices, pending selection, and menu results.
- `src/activities/ActivityResult.h`: `MenuResult::languageOverride`.
- `src/RecentBooksStore.h`, `src/RecentBooksStore.cpp`, and `src/JsonSettingsIO.cpp`: storage in `recent.json` and preservation when reopening a recent book.
- `src/activities/reader/EpubReaderActivity.cpp`: applying the override, resetting layout, and passing language to section builds and sleep-page rendering.
- `lib/Epub/Epub/Section.h` and `Section.cpp`: inline `BuildParams::languageOverride`, variant hashing, and typography/hyphenator selection.
- `test/epub_pipeline/BookLanguageTest.cpp`: direct section-pipeline regression coverage.

The pre-existing hyphenation engine was inspected where needed; this was not a complete audit of all language pattern data or line-breaking algorithms.

## Confirmed behavior and evidence

### Language selection and hyphenation enablement are independent

`EpubReaderActivity.cpp:1464–1465` fills `p.hyphenationEnabled` and `p.languageOverride` independently. `getEffectiveHyphenation()` at lines 3456–3460 resolves the per-book toggle before the global toggle. `ParsedText.cpp:533–540` selects the hyphenating layout only when the enable flag is true.

The compiled global default is Off (`src/CrossPointSettings.h:472`). **This does not explain this report:** the user explicitly confirmed hyphenation is enabled. Do not ask the user to repeat that check as the next diagnostic step.

### The host section tests pass, but bypass the selector

Commands exercised:

```sh
cmake --build build/test --target EpubPipelineTest -j2
build/test/epub_pipeline/EpubPipelineTest \
  --gtest_filter='*BookLanguageTest*' \
  --gtest_output=xml:/tmp/hyphenation-review.xml
```

Build succeeded. Both parameterized cases passed:

- No language metadata.
- Unrecognized metadata value `English`.

The test explicitly sets `metadata.hyphenationEnabled = true` and directly supplies `languageOverride = "en"`. It verifies English hyphenation, cache separation, preservation of EPUB metadata, restoration of the metadata layout, and reopening both warm-cache variants (`BookLanguageTest.cpp:73–105`).

It does **not** exercise menu selection, persistence failures, actual device memory conditions, or a firmware binary installed on the user's device. The initial review's lack of actionable findings should not be read as proof of the device path.

### The existing WASM build applies a language-only change through the UI

Used the existing simulator artifacts under `/tmp/witchhunt-wasm-language` and the fixture `/tmp/witchhunt-language-ui/books/language.epub` in headless Chromium. This was an existing build, not a fresh firmware/simulator rebuild during this investigation; exact installed-device equivalence was not established.

Observed through the actual reader/menu/selector UI:

1. Opened the fixture, whose language metadata is absent.
2. Set Hyphenation to On.
3. Opened Book language and selected English.
4. Left the menu: `recent.json` contained `"hyphenationOverride":1` and `"languageOverride":"en"`.
5. A new section cache was built; reopening the warm layout visibly rendered breaks including `extraordi- / nary`, `demon- / strate`, and `hyphen- / ation`.
6. Reopened the menu and observed English and Hyphenation On still selected.
7. Switched only Book language back to Default, keeping Hyphenation On.
8. `recent.json` retained `"hyphenationOverride":1` and omitted the language override. Reopening that layout restored the non-language-pattern-hyphenated paragraph.

This demonstrates that a language-only change can affect layout through the selector in that simulator build. It does not reproduce or resolve the user's device failure.

#### Simulator limitation during verification

On a cold section rebuild, this simulator reported secondary-framebuffer reallocation failure and entered its fragmented-heap restart path, despite its synthetic large free-heap values. To view the generated pages, the investigation preserved the simulator's filesystem (including newly generated caches and `recent.json`), reloaded the runtime with those files restored, and opened the warm cache. The resulting screenshots therefore prove persisted selection and cached output, **not a clean uninterrupted device-like cold-rebuild/repaint transition**.

The first plain HTTP server also failed to launch the WASM worker because SharedArrayBuffer required cross-origin isolation. A temporary server supplied COOP/COEP headers. Both temporary HTTP servers were stopped and the browser tab was closed after verification.

Temporary evidence, not durable repository assets:

- `/tmp/hyphenation-review.xml`: host test report.
- `/tmp/hyphenation-ui-evidence.json`: captured Default and English recent-book data.
- `/tmp/hyphenation-default.webp`: Default language with Hyphenation On.
- `/tmp/hyphenation-english.webp`: English with Hyphenation On.

These files can disappear with temporary-directory cleanup.

## Concrete code defect found: persistence failure silently prevents live application

At `src/activities/reader/EpubReaderActivity.cpp:7717–7723`, the menu callback gates the entire live language change on a successful store operation:

```cpp
if (menu.languageOverride != bookLanguageOverride &&
    RECENT_BOOKS.setLanguageOverride(epub->getPath(), menu.languageOverride)) {
  RenderLock lock(*this);
  bookLanguageOverride = menu.languageOverride;
  resetBackgroundBuild();
  resetSectionKeepingPosition();
}
```

`RecentBooksStore::setLanguageOverride()` (`src/RecentBooksStore.cpp:333–340`) returns false when the book is absent from the recent list or when saving fails. In either case, the reader keeps the previous language and does not reset layout. There is no failure handling in the callback.

By contrast, `applyBookReaderOverrides()` (`EpubReaderActivity.cpp:3355–3372`) updates the live overrides and attempts persistence without making live application conditional on that write's success.

The store's short-lived `Hold` releases the loaded list when the last hold ends (`RecentBooksStore.cpp:30–40`); an unsuccessful write is not retained as a deferred in-memory language change.

### Conditions that can reach this defect

- Failed write to `/.crosspoint/recent.json`, for example a full/read-only card or transient storage failure.
- A book missing from the recent list.
- Code-inspection reproduction candidate: books under `/test/` are excluded from `RecentBooksStore::addBook()` by `SerialTestApi::isTestBook()` (`RecentBooksStore.cpp:49–51`). With no corresponding recent-book entry, the language setter returns false.

**Evidence level:** the control-flow defect is confirmed by code inspection. Neither a failed-write scenario nor the `/test/` scenario was executed during this investigation. There is no evidence yet that either condition applies to the user's book/device. Do not present this defect as the established root cause.

### Possible correction, not implemented

Separate live application from persistence, consistent with the other per-book layout overrides, and handle a failed save explicitly. Preserve position and invalidate background/current layout when the live language changes.

Before implementing, account for the sleep-page path: it currently reads the language override from the recent-book store (`EpubReaderActivity.cpp:7614`), not the live reader member. Allowing an unsaved live override would make those sources diverge. Decide and verify the intended behavior rather than changing only the conditional and assuming the cutover is complete.

## Selector/result paths inspected without finding a drop

- `EpubReaderMenuActivity.cpp:211–226`: DynamicEnumCtx setter updates `pendingLanguageOverride` in the menu object.
- `EnumSelectionActivity.cpp:48–52` and `SettingInfo.h:518–525`: activation writes the selected index through the setting's setter before finishing.
- `EpubReaderMenuActivity.cpp:700–706`: selector is pushed as a child; its return requests a repaint.
- `ActivityManager.cpp:306–347`: restores the parent activity and invokes its result handler; it does not recreate the menu.
- `EpubReaderMenuActivity.cpp:541–564,589–614`: both action exit and Back carry `pendingLanguageOverride` in the result.
- `EpubReaderActivity.cpp:7717–7728`: language application precedes the cancellation check, so Back is intended to apply pending settings.
- `Section.cpp:392–395`: non-empty language overrides are included in the section property hash.
- `Section.cpp:1252–1255`: setup uses the override instead of metadata when selecting TypographyProfile and the language hyphenator.

These paths work in the simulator scenario above. The separate investigation into resumable/background build interactions was cancelled before publishing a conclusion; that area is not cleared by this write-up.

## Device evidence unavailable at pause

The existing checked-in-tree logs under `logs/` and `more logs/` contained no language/hyphenation matches and did not capture this selector experiment.

A historical device endpoint was found in memory, but neither attempt provided access:

- `http://crosspoint.local/api/status`: hostname lookup failed.
- `http://192.168.0.174/api/status`: connection to port 80 failed.

No device settings, current book, current firmware identity, or current selector/rebuild logs were retrieved. No device files were modified and no firmware was flashed/published.

## Resume checklist

1. Establish the current book, language choices before/after, and whether the selected language remains visible after leaving and reopening the menu. A language already equivalent to metadata can legitimately produce the same layout.
2. Obtain current device logs covering selector exit and the next render/build, plus `recent.json` and firmware/build identity, through an available authorized device connection or user-supplied files.
3. Determine whether the persistence gate rejects the change, whether a new language-specific section cache is built/adopted, and whether the displayed page belongs to that variant.
4. If persistence succeeds, finish tracing resumable/background builds and the global Hyphenator state across setup/parse interleaving; this remains an investigation target, not a finding.
5. Reproduce the actual failure before choosing a fix. Add a focused behavioral regression for the demonstrated failure, then verify the selector-to-render path and persistence/reopen behavior. Hardware repaint and position retention still need device evidence.
