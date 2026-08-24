# Forgetful — Design Document (v7 — locked for v1 implementation)

**Module ID:** `forgetful`
**Component type:** `audio_fx` (chainable — lives in a Signal Chain fx1/fx2 slot,
uses only the standard knob-turn + jog-page-scroll interaction, no raw MIDI /
custom touch gestures needed)
**One-line pitch:** One live input, routed by hand into whichever of four
tape memories you choose, each one already forgetting itself the moment you
move on — drifting out of tune, losing its top end, getting quieter, until
it's gone. Mix the four together and you're performing with your own recent
past, a little more blurred each time you glance back at it.

## Major structural change from v5

v5 had four loops each passively, independently listening to their own input
feed, one full page each (8 knobs: volume, degrade rate, 5 flavor knobs,
erase). **v6 adds a fifth page — a Master/Mixer page — and reworks the whole
module around a single shared live input that you explicitly route to one
buffer at a time.** This turns Forgetful into something you actively perform
with (build up A, move to B while A decays under it, bring in C, mix the
levels) rather than four loopers that just happen to coexist.

Concretely: **Volume moves off the individual loop pages and onto the Master
page** (since mixing the four is now the point of that page), freeining a
knob on each loop page.

## Concept

One live input. Four small tape memories. You decide, moment to moment,
which one is currently listening — turn the routing knob to point the input
at A, play, turn it to B, keep playing while A quietly starts forgetting
itself underneath, bring in C, glance back and mix in what's left of A. Nothing
you route into stays sharp for long: every loop starts degrading the instant
you move on from it — pitch wobbling, highs dulling, getting quieter — until
it's silent and ready to be filled again.

## Interaction Model

**Five pages, navigated via jog wheel scroll:** Master, then Loop A, B, C, D.

### Page 0 — Master / Mixer

| Knob | Control | Behavior |
| --- | --- | --- |
| 1 | **Input Routing** | Standard `enum` param, options `None`/`A`/`B`/`C`/`D`. Determines which buffer the live input currently feeds. Default `None` — nothing records until you deliberately point it somewhere. Every enum with declared `options` is automatically divable (touch + jog-click opens a scrolling picker) — no custom code needed for that; turning also steps it one option at a time. |
| 2 | Volume — Loop A | Standard continuous param |
| 3 | Volume — Loop B | Standard continuous param |
| 4 | Volume — Loop C | Standard continuous param |
| 5 | Volume — Loop D | Standard continuous param |
| 6–8 | *Reserved (placeholder)* | Declared as inert `chain_params` entries — **structurally required**, not just idle. Flat `chain_params` arrays auto-paginate 8-per-page in declaration order; without real entries here, Loop A's params would spill onto knobs 6–8 of *this* page instead of starting fresh on page 2. Candidates for real future use: master wet/dry, global freeze-all, master saturation. |

This page doubles as the **status overview**: alongside each volume knob's
label, the screen shows that loop's current state (e.g. `B: Looping 74%
(Fading)`), so you can see all four at a glance without leaving the Master
page. This replaces the earlier "always-visible strip on every page" idea
from v5 — turns out the Master page is a more natural home for it, and
resolves that open question without needing custom layout work on the other
four pages.

### Page 1–4 — Loop A / B / C / D

Volume is gone from these pages (moved to Master). Degrade Rate shifts into
knob 1 so the layout stays predictable:

| Knob | Control | Behavior |
| --- | --- | --- |
| 1 | Degrade Rate | Standard continuous param, repeats-until-forgotten (~3–60) |
| 2 | Wow/Flutter | Standard continuous param, 0–1 |
| 3 | Fade to Dark (HF loss rate) | Standard continuous param, 0–1 |
| 4 | Hiss | Standard continuous param, 0–1 |
| 5 | Warmth (saturation) | Standard continuous param, 0–1 |
| 6 | Sudden Forgetting (chaos) | Standard continuous param, 0–1 |
| 7 | *Reserved (placeholder)* | Same page-alignment requirement as Master's 6–8 — see note there. Candidate for later: a "character" preset knob, pitch offset. |
| 8 | **Erase** | Real `chain_params` trigger — see below (changed from v6's "turn-to-unspool") |

### Recording: routed, not per-loop-independent (changed in v6)

There is now **one shared live input**. Only the buffer currently selected by
the Master page's Input Routing knob can receive it:

- Input Routing = `A` (or B/C/D): that buffer starts monitoring its incoming
  level. If it's `IDLE` and input crosses `record_threshold` (settings-schema
  field, default ~-30dBFS) for longer than a short debounce (~50ms), it
  enters `RECORDING`.
- The other three buffers receive no input at all while not selected — they
  simply hold whatever state they're already in (continuing to `LOOP`/decay
  normally if that's what they were doing; sitting `IDLE` if empty).
- Input Routing = `None`: no buffer receives input; all four just continue
  whatever they're already doing.

**A recording closes (moves to `LOOPING`, flavor params randomize) on
whichever of these happens first:**
1. `silence_timeout` (settings-schema field, default ~1.5s) of near-silent
   input, or
2. `buffer_seconds` max length reached, or
3. **(new in v6)** Input Routing is turned away from this buffer — either to
   a different letter or to `None`. Moving on is treated as "I'm done with
   this one," an explicit close in addition to the passive silence-based one.
   This also covers sustained ambient sources (drones, pads) that might never
   go properly silent on their own.

### Erase: real trigger, double-click-confirm (knob 8) — changed in v7

`docs/MODULES.md` documents an `access` field on `chain_params` entries,
independent of `type`:

| value | meaning | host behavior |
| --- | --- | --- |
| `readwrite` (default) | ordinary control | turnable, divable if it has options |
| `read` | readout | never turnable, never opens a picker |
| `write` | **trigger** | never turnable, **a click fires it** |

This is exactly the discrete "button" gesture v1–v6 kept working around with
synthetic turn-based mechanics. Knob 8 is declared:

```
{"key": "loopX_erase", "name": "Erase", "type": "enum",
 "options": ["—", "Erase!"], "access": "write"}
```

Touching knob 8 and clicking the jog wheel **fires** the trigger — the module
receives a `set_param("loopX_erase", ...)` call each time, regardless of
which option string is nominally associated. Because `access: "write"`
disables turning/scrubbing entirely, the value can never drift there by
accident (this is the exact hazard the docs warn about elsewhere: a
turnable trigger-shaped enum can fire from an ordinary knob nudge — declaring
`write` closes that off at the host level, not just in our own code).

**Double-click-confirm, implemented in module state (not host-provided):**
firing a trigger is instantaneous, so the confirm behavior you asked for
(two presses, not one) is built in the module's own C code, not the host:

- First fire on a non-empty loop: don't clear yet. Record the timestamp,
  show "Tap again to erase" in that loop's page state line (screen text we
  already control).
- Second fire within ~600ms: hard-clear the loop, buffer emptied, state →
  `IDLE`.
- No second fire within the window: the armed state simply lapses (nothing
  cleared, no visible LED to reset since we're not managing knob LEDs
  directly — the screen hint just reverts on next redraw).

Works identically regardless of Input Routing state — you can erase a loop
that isn't currently selected for recording.

## State Machine (per loop, ×4 independent instances)

```
IDLE ──(routed here AND input > record_threshold, debounced)──> RECORDING
RECORDING ──(silence_timeout elapsed, OR buffer_seconds reached,
             OR routing moves away from this loop)──> LOOPING
LOOPING ──(memory reaches 0)──> FORGOTTEN ──(auto)──> IDLE
(any non-empty state) ──(knob 8 fired twice within ~600ms)──> IDLE (hard clear)
```

Note: `LOOPING`'s decay progresses regardless of routing — a loop keeps
forgetting itself whether or not it's currently the recording target.

## Screen / Feedback

Same as v5: no raw MIDI/custom LED control in chain-embedded mode, so state
communication is entirely on-screen. Master page carries the 4-loop overview
(see above); each loop's own page shows its state line (`Listening…` /
`Recording` / `Looping — 74% (Fading)` / `Forgotten`) plus its 7 knob
labels/values, with knob 8 showing `Unspool: NN%`.

Memory-percentage-to-word mapping unchanged: 90–100% "Vivid", 40–89%
"Fading", 10–39% "Hazy", 1–9% "Almost gone", 0%/idle "Forgotten"/"Listening".

## DSP Architecture

One plugin instance internally manages **four independent loop engines**
plus one shared input router.

### Input routing (new in v6)
- Single `input_routing` value (enum: None/A/B/C/D), set from the Master
  page's knob 1.
- Each block: the live input signal is passed to exactly one `LoopEngine`
  (whichever is currently routed) for its record/envelope-follower logic;
  the other three receive no input this block.
- Changing `input_routing` away from a `RECORDING` loop triggers that loop's
  close-and-start-looping transition immediately (see State Machine).

### Per-loop buffer
- Circular buffer, fixed max length (`buffer_seconds` setting, default 8s).
- Recorded once; not rewritten during playback. Degradation computed at
  read time as a function of loop age.

### Memory value, degradation model, randomization of flavor params
Unchanged from v5: `memory` decrements per loop-wrap by `1/decay_repeats`;
wow/flutter, HF loss, hiss, saturation, chaos all driven by `(1-memory)`;
flavor params re-randomize on `RECORDING → LOOPING` unless locked to a fixed
value via `settings-schema.json` — except Degrade Rate, which is a live knob
(now knob 1 on each loop page). **Volume is now a Master-page live knob, not
a per-loop settings/randomization concern at all** — it directly scales that
loop's contribution to the summed output, independent of the degrade model.

**Decided (carried over from v5):** flavor knobs (2–6 on each loop page) are
live and override the current randomized value immediately, staying put
until that loop is erased and takes a new recording — randomization only
fires once, at `RECORDING → LOOPING`.

### Erase state (changed in v7)
Per-loop `erase_armed_at` timestamp (or sentinel "not armed"). On each
`set_param` fire of `loopX_erase`: if not armed, arm it (record now, show
screen hint); if armed and within `~600ms` of the recorded time, hard-clear
the loop and reset `erase_armed_at` to "not armed"; if armed but the window
has elapsed, treat this fire as a fresh first press (re-arm) rather than a
second press — an old, stale arm should not be redeemable by a much later
click.

### Signal flow per block
- Route live input to the currently-selected `LoopEngine`'s record path (if
  any is selected).
- For each of the four `LoopEngine`s independently:
  - `IDLE`: no output contribution; monitors input only if currently routed.
  - `RECORDING`: writes routed input into buffer; watches for
    silence-timeout/buffer-full/routing-change closes.
  - `LOOPING`: reads buffer at (wow/flutter-modulated) read-head, applies
    this loop's LP filter + hiss + level scaling + this loop's **Master-page**
    volume, sums into output, detects wrap → decrements memory.
  - `FORGOTTEN`: one-block silence flush → `IDLE`.
- All four loops' outputs sum into a single stereo output.

**Dry input always passes through (clarified during Loop A implementation).**
This module is an inline `audio_fx` sitting between the synth and the
output, so any state that produced silence as *the* output would mute the
whole track whenever no loop happened to be `LOOPING`. The corrected rule:
dry input passes straight through unconditionally in every state (`IDLE`,
`RECORDING`, `LOOPING`, `FORGOTTEN`); each state instead contributes an
additive **wet** layer on top of that dry signal — `IDLE`, `RECORDING`, and
`FORGOTTEN` all contribute zero, only `LOOPING`'s degraded playback
contributes non-zero wet. This resolves the ambiguity in the bullets above:
`RECORDING` was never a special case, it's just one of three states that
contribute no wet. When generalizing to four `LoopEngine`s (Build/Test Plan
step 3), dry must be added to the output exactly once, with all four loops'
wet contributions summed on top of it — not once per engine, which would
quadruple the dry signal.

### Capability flags
- `audio_in`, `audio_out`, `chainable: true`, `component_type: "audio_fx"`,
  `requires_continuous_processing: true`.
- Fully standard chain_params-driven module, no raw MIDI dependency.

## Per-Module Settings (`settings-schema.json`)

Unchanged from v5:

| Key | Label | Type | Notes |
| --- | --- | --- | --- |
| `buffer_seconds` | Buffer Length | float, 2–16, default 8 | Per loop, structural |
| `record_threshold` | Record Threshold | float (dBFS), default -30 | Auto-record trigger level |
| `silence_timeout` | Silence Timeout | float (sec), default 1.5 | Auto-close a recording after this much silence |
| `wow_amount_mode` / `wow_amount` | Wow/Flutter Random/Fixed + value | enum/float | default "random", range 0.1–0.6 when random |
| `hf_loss_rate_mode` / `hf_loss_rate` | Fade to Dark Random/Fixed + value | enum/float | default "random", range 0.2–0.8 |
| `hiss_amount_mode` / `hiss_amount` | Hiss Random/Fixed + value | enum/float | default "random", range 0.0–0.4 |
| `saturation_mode` / `saturation` | Warmth Random/Fixed + value | enum/float | default "random", range 0.0–0.4 |
| `chaos_mode` / `chaos` | Sudden Forgetting Random/Fixed + value | enum/float | default "random", range 0.0–0.15 |
| `erase_confirm_window_ms` | Erase Confirm Window | int (ms), default 600 | Time between double-click fires on the Erase trigger to count as a confirm |

## module.json sketch

```json
{
  "id": "forgetful",
  "name": "Forgetful",
  "version": "0.6.0",
  "abbrev": "FRGT",
  "description": "One input, four tape memories, all of them forgetting.",
  "author": "TBD",
  "ui": "ui.js",
  "dsp": "forgetful.so",
  "api_version": 2,
  "capabilities": {
    "audio_in": true,
    "audio_out": true,
    "chainable": true,
    "component_type": "audio_fx",
    "requires_continuous_processing": true,
    "chain_params": [
      "Page 0 (Master): input_routing (enum), loopA_volume, loopB_volume, loopC_volume, loopD_volume, [reserved placeholder x3]",
      "Page 1-4 (Loop A/B/C/D): loopX_decay_rate, loopX_wow, loopX_hf_loss, loopX_hiss, loopX_saturation, loopX_chaos, [reserved placeholder], loopX_erase (enum, access: write)"
    ]
  }
}
```

Filename note unchanged: shared lib must be `forgetful.so` for Signal Chain
compatibility (`modules/audio_fx/forgetful/forgetful.so`).

## Status

Interaction model (5 pages), Master/Mixer page layout, input routing, the
routing-change-closes-recording rule, per-loop page layout, and the Erase
trigger mechanism are all locked and now grounded directly in `docs/MODULES.md`
(`access: "write"`, divable enums, flat `chain_params` auto-pagination) rather
than assumption. Nothing below should block starting the DSP work.

## Open Questions / Risks (implementation-time, non-blocking)

- **Reserved/placeholder knobs** (Master 6–8, each loop page's 7) — confirmed
  structurally necessary for page alignment (see table notes above), not just
  idle. Exact placeholder param shape (e.g. a harmless `read`-only dummy vs.
  a genuinely inert `readwrite` float) to decide during implementation.
- **`record_threshold` / `silence_timeout` tuning** — first-guess defaults,
  will need ear-tuning once DSP exists.
- **LED behavior in generic chain mode** — the "Chain editor knob feedback is
  a CARD" section of `CLAUDE.md` describes touch raising a value card, not
  per-module custom LED coloring; screen-based feedback (our plan) is the
  correct approach here, not a fallback for something unavailable — this is
  now confirmed, not just assumed.
- **`erase_confirm_window_ms` default (600ms)** — still a first guess, easy
  to retune.
- Overdub layering — still out of scope for v1.

## Build/Test Plan

1. Check whether the Master page's status-overview text fits alongside 5
   knob labels on the standard chain param-page layout; simplify status
   text if space is tight.
2. Implement DSP in C: start with **one** `LoopEngine` in isolation
   (envelope follower, buffer, degradation chain, double-click erase trigger
   handling), bench-test it end-to-end before replicating to four.
3. Generalize to four `LoopEngine`s plus the shared input router inside one
   `audio_fx_api_v2_t` plugin instance, in-place `process_block`.
4. Implement `chain_params` declarations (flat array, 5 pages × 8 including
   placeholders) and the `get_param` text needed for the Master page's
   per-loop status overview and each loop page's state line / erase-armed hint.
5. Bench-test DSP standalone with synthetic input, verifying routed
   recording, all three close conditions (silence/max-length/routing-change),
   decay timing, and double-click erase (including the stale-arm-does-not-
   count-as-confirm case) across all four loops.
6. On-device test in an actual Signal Chain fx slot: verify page scroll,
   verify routing knob switches cleanly between destinations mid-performance,
   verify erase doesn't false-trigger from normal knob nudges.
7. Package per `docs/MODULES.md` tarball structure, `release.json`, catalog PR.
