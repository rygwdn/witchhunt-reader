# Interactive devctl decision-model integration

Status: design resolved; headless native prototype implemented and exercised on 2026-10-21.
Autonomous physical-device operation is not part of this prototype. Manual JSONL USB
transport is available but untested. No firmware changes.

## Goal and resolved architecture

Use inexpensive typed image judgments to navigate the real reader UI toward a
natural-language goal. Preserve `tools/devctl.py` scripted operation and its sole
transport ownership, binary-safe screenshots, completion barriers and shutdown.
Do not introduce an accessibility tree, menu inventory, replay subsystem, image
processing dependency or generalized agent framework.

Jev and Clef are **stateless decision models**, not tool-using agents. Both accept
text/image application state and typed questions through OpenRouter Decisions.
They do not own iteration, history, execution, approval, deadlines or completion.
A small host loop is therefore necessary, not a competing agent runtime.

Contract verified against primary documentation:

- `POST https://openrouter.ai/api/alpha/decisions`, authenticated with an OpenRouter key.
- Request: model, top-level state array (plain text plus image_url parts), questions.
- Each PNG image is a base64 data URL; local filenames and remote HTTP image URLs
  are not usable image inputs on this API.
- A Choice question contains instructions and a map of labels to criteria.
- Response: `answers.<question>.choice`, probabilities/confidence and provider usage.
- Jev 1.14 supports the existing 480×800 portrait PNG dimensions. No resizing needed.
- Clef shares the contract but has different text/image limits; not selected or tested.

Sources:
- https://openrouter.ai/docs/guides/community/jev
- https://openrouter.ai/docs/guides/community/multimodal-decisions

## Implemented boundaries and interfaces

### Persistent controller: tools/devctl_interactive.py

A separate host entrypoint imports the existing Device; this preserves the scripted
CLI rather than mixing JSONL and scripted stdout in one command. `InteractiveSession`
provides observe, press and close while holding one serial connection or native child.

JSONL requests:

```json
{"op":"observe"}
{"op":"press","button":"DOWN","kind":"short"}
{"op":"close"}
```

- Manual button vocabulary: BACK, CONFIRM, LEFT, RIGHT, UP, DOWN, POWER; short/long/double.
- Observe: HELP capability gate, IDLE screen completion, STATE, screenshot.
- Observation: state, screenshot local path/null, raw idle outcome, errors, actions, usable.
- Press: one physical command followed by observation, including press_reply.
- Missing acknowledgement: uncertain outcome, observe without retrying the press.
- Incomplete binary capture: invalidate session; no additional commands on that stream.
- Close: deterministic existing Device lifecycle, including native exit/forced-shutdown status.
- JSONL stdout contains no firmware logs or binary screenshots.

`tools/devctl.py` gains only an optional monotonic Device deadline used by the goal
runner. Existing scripted use leaves it unset.

### Goal runner: tools/devctl_goal.py

- Native `--exec` only; fresh/empty output directory contains a new disposable SD.
- Optional local EPUB fixtures are copied under that SD's `/test`.
- Pinned initial target: `typesafe/jev-1.14`. Model/credential-env/compatible endpoint
  are configurable; no custom chat-completion client or extra package.
- Compact goal, STATE, explicit authorizations/success predicates, recent actions,
  button grammar and actual PNG are supplied to the decision provider.
- Selection Choice: six short navigation buttons, long Up/Down, done, blocked,
  approval_required. No model-selected raw ACT/SET/FS commands or Power.
- Tab-bar Confirm is omitted: long Up/Down already represent tab changes, whereas
  Confirm at sel=-1 does not activate a row. No screen inventory was added.
- A second Choice judges the exact selected action before execution. Independent
  questions in one request cannot safely evaluate an as-yet-unselected action.
- Host-side forbidden-goal and hazardous-Confirm gates cannot be overridden by the model.
- Bounds: action count, monotonic wall deadline, total provider-response deadline,
  repeated UI/frame fingerprints excluding volatile heap/popup age.
- One press at a time, screen-only IDLE, then inspect. No blind retries.
- Result distinguishes verified_done (all explicit STATE predicates match), model_done
  (model claim only), blocked, approval_required and failure.
- Traces contain observations, decisions, provider results and screenshot paths;
  final result includes final observation, counts and child shutdown.

## Authorization and known limits

Navigation and explicitly authorized settings changes on disposable simulator data
only. Native networking can reach real hosts. Do not import production credentials
or seed a real card. The goal runner refuses existing nonempty output directories
and overrides inherited CROSSPOINT_SIM_SD with its own fresh SD.

Model permission judgments and label heuristics are **not a security boundary**.
Ambiguous operations stop; no claim of general safety from BTN-only actions.
Manual JSONL callers remain responsible for their own consequential presses.

STATE is partial. Boolean switches can omit value entirely; a condition such as
`value=On` will not verify them. Use screenshots and persisted simulator settings
for those cases. Enum rows such as Reading Orientation expose values suitable for
explicit STATE predicates. `/test` does not establish persisted per-book settings.

Changing clocks/pixels can evade the repeated-frame heuristic; the action and time
limits remain independent bounds. Shutdown has the existing additional bounded
cleanup grace. Native results do not establish physical panel timing, USB lifecycle,
ESP32 memory usage, peripheral behavior or general model reliability.

## Verification and observed outcomes

Built the real simulator serially with the existing local PlatformIO environment.
The local PlatformIO launcher points at a missing Python 3.13 interpreter; using
its installed packages with the available interpreter succeeded:

```sh
PYTHONPATH="$HOME/.platformio/penv/lib/python3.13/site-packages" \
  python3 -m platformio run -e simulator -j 1
```

The build succeeded with pre-existing enum-switch warnings in simulator/firmware
code; those unrelated files were not edited.

No standalone OpenRouter credential was available to this session. Live runs used
a temporary loopback Decisions-wire bridge to OMP's configured image-capable judge.
The bridge forwarded each real screenshot and typed question, not scripted action
answers. Result metadata identifies `omp-configured-judge`, not an unverified model
or provider. Direct OpenRouter authentication and Clef remain untested.

Observed live runs (counts are provider requests, including separate authorization):

- Reader Settings tab: three successful runs; 7/7/8 presses and 14/14/16 requests.
- Reading Orientation Portrait → Landscape CW: two successful runs; 10 presses,
  20 requests each. UI/STATE and persisted settings verified. Both restored to
  Portrait using the unchanged scripted CLI and verified in persisted settings.
- Open fixture EPUB and reach first chapter page 2: four successful runs;
  4 presses and 8 requests each; final STATE page=2/2 and screenshots captured.
- Unavailable calculator goal: blocked, zero presses, one request.
- Unauthorized delete goal: blocked before starting firmware, zero presses/requests.
- Exploratory toggle runs produced wrong navigation, authorization stops, and a
  repeated-screen stop. One Hyphenation run changed the persisted value to 1 but
  correctly refused a done claim because the supplied value predicate was absent
  from STATE. It was restored to 0. These failures are not hidden as successes.
- Persistent JSONL: observe, invalid-button rejection, one press, observe, close;
  row changed 0→1, remained 1, native exit 0.
- Dropped real BTN acknowledgement injection: exactly one press sent, row changed
  0→1, final observation marked uncertain; no duplicate press; native exit 0.
- Unauthorized orientation change without --authorize: approval_required before the
  setting activation; persisted/default orientation remained Portrait.
- Invalid provider action ACT:HOME: rejected without a button press; native exit 0.
- Stalled provider response: timed out with zero presses and a clean native exit.
- Host wall deadline: stopped with zero presses; cleanup completed within its
  separate shutdown grace.
- Short-frame injection: session invalidated; subsequent Confirm issued zero commands.
- Permanent regressions: all three tests passed.

Full JSONL traces, results, screenshots, fault-injection reports, restorations and
test report: `/tmp/devctl-prototype-20261021-cc_wwek9`. `runs.json` retains exploratory
failures as well as successful paths. Navigation counts include earlier vocabulary
revisions; they are not a controlled benchmark. The repeat sample is small and
exploratory failures demonstrate that inexpensive-model reliability is not
established. No broad success-rate or cost claim is justified.

## Completion and operational handoff

The native prototype and its invocation/observation/action/safety documentation are
in `docs/serial-test-api.md`, under Native simulator over stdio. Permanent regressions
cover progress fingerprints and the deadline's no-write invariant:

```sh
python3 -m unittest discover -s tools -p 'test_devctl_*.py'
```

The earlier 2–4-day estimate is withdrawn: it was based on an unverified agent
interface. The working native path now gives concrete evidence rather than an
estimate. A physical-device rollout requires real USB ownership/lifecycle smoke
runs, a stronger independently enforced approval boundary, and settings restoration
checks before autonomous USB is enabled. Those are deployment gates, not claimed
prototype capabilities.
