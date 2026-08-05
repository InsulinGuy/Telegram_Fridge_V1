# Telegram message information architecture & HTML formatting — TeleFridge V1

> **Status:** design spec (issue **#63**). This is the authoritative spec that the
> `logger/report.h` rework codes against. Values and line references here track
> `logger/report.h`, `logger/ring_buffer.h`, `logger/helpers.h` and
> `telefridge.yaml` as of 2026-07-27; if they drift, the code wins and this doc is
> the bug.
>
> **Implemented** (issue #63, ADR-017). `logger/report.h`, `logger/helpers.h`
> (`html_escape`/`zone_emoji`), `logger/ring_buffer.h` (`Digest` epoch/excursion
> fields), the six `telefridge.yaml` send sites, and `test/test_report.cpp` all
> landed. Remaining: on-hardware render check (verification steps 3–5 below).

---

## Why this change

Issue #63 originally scoped only *formatting* (switch the send to
`parse_mode=HTML`). Auditing the actual message content showed the deeper problem
is **information architecture**, not styling.

### The 4-hour report today

`build_report()` (`logger/report.h:44`) emits, in order:

1. `📋 TeleFridge report (16 samples)`
2. An ASCII pipe table — per sensor: **Cur / Min / Max / Avg / Zone**
3. `📶 −67 dBm  up 42s`
4. `🔋 4.10 V / 95 %`
5. `overrides: …` — only when a setting differs from its compiled default

### What is wrong with it

- **It is a flat dump with no hierarchy.** Every line carries equal weight.
  Nothing answers the only question the message exists to answer: *is the payload
  safe right now?*
- **Sensors A and B get equal visual billing**, contradicting **ADR-011**, where
  `temp_a` (box interior) is the pharma-compliance authority and `temp_b` (fridge
  air) is advisory. The reader must know which column matters.
- **Ordering is roughly inverted by importance** — RSSI and uptime get their own
  top-level lines while the box safety state is a table cell.
- **The ASCII table does not align.** Telegram renders message text in a
  proportional font, so the pipe columns wander. The table only works inside
  `<pre>`.

### Three defects, not preferences

1. **A faulted sensor renders as Zone `OK`.** `build_report()` never calls
   `sensor_faulted()` (`ring_buffer.h:289`), and `alert_state()` returns
   `ALERT_OK` on NaN (`helpers.h:73`). A dead SHT40 — the *primary safety sensor*
   — is reported as healthy. Fault detection already runs every wake
   (`faults_observe()`, `telefridge.yaml:314`); the report simply ignores it.
2. **The predictor never reaches any report.** `report.h` does not even
   `#include "predictor.h"`, despite **CLAUDE.md:530** calling for a predictor
   state line. `predictor_t_min()` (`predictor.h:82`), `box_ema()` and
   `fridge_ema()` all exist and run every wake. The device's best early-warning
   signal is invisible in the digest.
3. **The report has no time.** Every `Sample` carries an `epoch`
   (`ring_buffer.h:17-21`), but `ring_summary()` (`:119-141`) discards it. There
   is no "as of" time and no window bounds, so a fresh report is
   indistinguishable from one describing a window that ended hours ago.

Alerts are separately **context-poor**: `build_crit_alert()` (`report.h:131`)
carries a sensor letter, a label and a temperature — no time, no battery, no
second sensor, no trend.

---

## Decisions taken

| Decision | Choice | Rationale |
|---|---|---|
| Table row labels | **`Box` / `Fridge`** (not `A` / `B`) | Self-explanatory under stress; encodes the ADR-011 asymmetry in the message itself. Code, logs and CLAUDE.md keep `temp_a`/`temp_b`. |
| Verdict line on healthy reports | **Always present** (green) | A constant shape trains the eye; a changed colour/emoji against a familiar green is a stronger signal than an absent line. |
| Issue structure | **#63 expands** to formatting **+** IA | Both edit the same functions in `report.h`; splitting means touching it twice and re-verifying rendering on hardware twice. |
| Alerts | **Enriched**, not merely restyled | Costs bytes only — no extra sensor reads, no extra radio time. |
| Parse mode | **HTML**, not MarkdownV2 | Only `&`, `<`, `>` need escaping vs ~18 characters in MarkdownV2 — materially safer when composing strings in a lambda. |

### Telegram HTML constraints

A fixed tag whitelist: `<b> <strong>`, `<i> <em>`, `<u> <ins>`, `<s> <strike> <del>`,
`<a href>`, `<code>`, `<pre>`, `<blockquote>`, `<blockquote expandable>`,
`<tg-spoiler>`, `<tg-emoji>`. **No CSS, no colours, no tables, no `<br>`, no
images.** Line breaks are literal newlines. Message limit **4096 characters**.

**Malformed HTML returns HTTP 400 — the send fails.** Unsupported or unbalanced
tags are not ignored. This is the single biggest risk in this change and is why
the test plan below mandates a tag-balance check.

---

## The hierarchy

| Tier | Answers | Content | Rendering |
|---|---|---|---|
| **0 — Verdict** | Is the payload safe? | Box state + current temp | `<b>` + status emoji |
| **1 — Evidence** | Why, and heading where? | Predictor trend; Box cur/min/max/avg | italic line + `<pre>` table |
| **2 — Context** | What is driving it? | Fridge row (advisory), excursion count | inside `<pre>` / plain |
| **3 — Housekeeping** | Is the *logger* healthy? | window + epochs, RSSI, uptime, FW, cadence, wake count, settings overrides | `<blockquote expandable>` |

**Battery sits outside the blockquote** (call it tier 2.5). It is the one
housekeeping value that is also *actionable*, and running the cell flat is a
user-facing failure mode that costs the RTC ring buffer (**ADR-010**).

Collapsing tier 3 is what makes this work: a healthy report shows ~4 visible
lines instead of 10, with diagnostics one tap away.

---

## Target renders

### Healthy scheduled report

```html
🟢 <b>Box OK — 4.2 °C</b>
<i>steady · fridge 5.1 °C</i>
<pre>         Cur   Min   Max   Avg
Box      4.2   3.6   4.9   4.3
Fridge   5.1   4.4   6.2   5.2</pre>
🔋 4.10 V / 95 %
<blockquote expandable>16 samples · 12:05–16:05 UTC
📶 −67 dBm · up 42 s · wake 16
fw v1.4.2 · report every 4 h</blockquote>
```

`steady` is the predictor line that never landed (CLAUDE.md:530).

### Box in CRIT

```html
🔴 <b>BOX CRIT HIGH — 8.4 °C</b>
<i>above the <code>8.0 °C</code> limit for ~45 min · fridge 11.2 °C</i>
<pre>         Cur   Min   Max   Avg
Box      8.4   4.1   8.6   6.2
Fridge  11.2   5.0  11.8   8.4</pre>
⚠️ 3 of 16 samples out of range
🔋 4.10 V / 95 %
<blockquote expandable>…</blockquote>
```

The excursion count matters for a pharma payload: a 2-minute excursion and a
45-minute one are different events, and the current report cannot distinguish
them.

### Faulted box sensor

Today this renders as `A | 0.0 | 0.0 | 0.0 | 0.0 | OK`. Proposed:

```html
🔧 <b>BOX SENSOR FAULT — no reading</b>
<i>box state unknown · fridge 5.1 °C (still OK)</i>
<pre>         Cur   Min   Max   Avg
Box        —     —     —     —
Fridge   5.1   4.4   6.2   5.2</pre>
🔋 4.10 V / 95 %
```

"Unknown" is **stated**, never silently rendered as `OK`. The fridge is still
reported because it is the only signal left.

### Predicted-breach alert

```html
⚠️ <b>PREDICTED BREACH — box warming</b>
<code>6.8 °C → 8.0 °C in ~35 min</code>
<i>driver: fridge 11.2 °C and rising</i>
🔋 4.10 V / 95 % · 16:05 UTC
```

`<code>` makes the projection the visual anchor. Battery and time are added
because a 3 am alert should say whether the logger will survive the night.

> While `TAU_BOX_MIN` remains the pre-commissioning placeholder (`helpers.h:64`,
> see CLAUDE.md open items), any predictor figure in a message should be marked
> as uncalibrated.

### CRIT alert

Today: `⚠ TeleFridge ALERT` / `A sensor CRITICAL HIGH: 8.4 C`

```html
🔴 <b>BOX CRIT HIGH — 8.4 °C</b>
<i>limit <code>8.0 °C</code> · crossed ~10 min ago</i>
<i>fridge 11.2 °C — appliance likely failing</i>
🔋 4.10 V / 95 % · 16:05 UTC
```

Same event, but it now names the likely cause (fridge warm too → appliance
failure, not a door left open) and confirms the logger has power to keep
watching.

---

## Implementation

### 1. `logger/helpers.h`

- Add `html_escape(const std::string&)` — `&`→`&amp;`, `<`→`&lt;`, `>`→`&gt;`.
  Lives here rather than `report.h` so `settings.h` / `commands.h` can reuse it.
- Add `zone_emoji(AlertState)` → the directional zone gradient
  `🟦 / 🔷 / 🟢 / 🟡 / 🔴`, plus `⚠️` predicted breach and `🔧` sensor fault
  (issue #2). Cold CRIT is blue by decision and fault is off-scale in shape as
  well as hue — rationale in the `zone_emoji()` comment.
- Reuse the existing `alert_label()` (`:140`) — it already returns
  `"SENSOR FAULT"`, `"TRENDING COLD"` and `"TRENDING WARM"`, three labels no
  message can currently reach.

### 2. `logger/ring_buffer.h` — extend `Digest`

Add to the struct (`:113-117`) and populate in `ring_summary()` (`:119-141`):

- `uint32_t first_epoch, last_epoch;` — window bounds. Skip zero epochs
  (pre-SNTP samples carry `epoch == 0`).
- `uint16_t n_out_a, n_out_b;` — samples outside each sensor's OK band, computed
  with the existing `alert_state(v, is_box)`. Drives the excursion line.

> **Do not copy the existing loop shape for epoch logic.** `ring_summary()`
> iterates `g_ring[0..n)` rather than walking from the tail. That is safe for
> min/max/mean (order-independent), but index order is *not* time order once the
> ring wraps. Derive first/last epoch by min/max over the same pass.

### 3. `logger/report.h` — the bulk of the work

- `#include "predictor.h"` (currently absent) to reach `predictor_t_min()`
  (`predictor.h:82`), `box_ema()` / `fridge_ema()` (`ring_buffer.h:184` / `:159`).
- **Replace the fixed `char buf[512]` / `buf[420]` + single `snprintf` with
  `std::string` append** (small per-fragment `snprintf` into a local scratch).
  Tags plus new content overflow the current buffers, and silent `snprintf`
  truncation would emit an **unclosed tag → HTTP 400 → failed send**. Add a final
  length guard against the 4096-character limit.
- Factor shared pieces so the six builders compose instead of duplicating:
  `verdict_line()`, `trend_line()`, `stats_table()`, `diagnostics_block()`.
- **Fault-aware reporting:** consult `sensor_faulted()` / `fault_reason()`
  (`ring_buffer.h:289` / `:294`) before computing a zone. If faulted, the verdict
  becomes `ALERT_SENSOR_FAULT` and the row renders `—`.
  Do this **in the builder, not by changing `alert_state()`** — that function is
  pure and shared with the alert-firing path; coupling it to fault state would
  change *firing* behaviour, which is out of scope here.
- `build_report()` gains arguments threaded from YAML: `now_epoch`, `wake_count`,
  `button_wake` (so a button-forced report is labelled — today a forced and a
  scheduled report are byte-identical), and `report_interval_min`.

#### Escaping order — strictly

1. `html_escape()` the dynamic **strings**
2. wrap in tags
3. `json_escape()` (`report.h:17`) for the JSON body
4. YAML-side URL-encode

Escape `alert_label()` / `fault_reason()` output and `settings_overrides_line()`.
`snprintf`'d floats are provably free of `& < >` and need no escaping. **`<pre>`
content still needs escaping.**

### 4. `telefridge.yaml`

Add `parse_mode=HTML` to the POST body at every send site:

| Script | Line (script / builder call) |
|---|---|
| `send_report_and_sleep` | `:456` / `:480` |
| `send_boot_msg_and_continue` | `:510` / `:553` |
| `send_crit_alert_and_sleep` | `:585` / `:607` |
| `send_predicted_alert_and_sleep` | `:634` / `:657` |
| `send_fault_alert_and_sleep` | `:687` / `:710` |
| `send_batt_alert_and_sleep` | `:727` / `:750` |

**`poll_commands` reply (`:774` / `:814`) stays plain-text.** `g_cmd_reply` echoes
raw user command text, which can contain `<`/`>`; adding HTML there means
escaping an untrusted path for no benefit. Leave `parse_mode` off that request.

Thread the new `build_report()` arguments (`sntp_time` epoch, `g_wake_count`,
`g_button_wake`, `g_settings.report_every * sample_interval_min`).

The alert scripts never `component.update: wifi_rssi`, which is why alerts omit
RSSI. The enriched alert lines use battery + time only — **no extra sensor reads
and no extra radio time**, preserving the ADR-006 power posture.

### 5. Tests — `test/`

Follow the existing host-test pattern (`test/test_predictor.cpp`,
`test_commands.cpp`, `test_ring_buffer.cpp`, run via `test/run.sh`). Add
`test/test_report.cpp`:

- Snapshot tests over a synthetic ring: healthy, box CRIT, box WARN, faulted
  sensor, empty buffer, pre-SNTP (`epoch == 0`) window.
- **HTML validity check** — a tag-balance scanner asserting every opened tag
  closes and only whitelisted tags appear. This is the guard against the HTTP 400
  that would silently break every send.
- Assert total length < 4096 with a full 32-sample ring.

---

## Out of scope

| Item | Where it belongs |
|---|---|
| Unicode line graph in the report | **#22** |
| Graph *image* via `sendPhoto` / QuickChart | **#55** |
| `<tg-spoiler>` | dropped — no sensitive content in these messages |
| SHT40 humidity (one YAML line away; `telefridge.yaml:126-133` declares temperature only) | separate feature decision |

> **#50 reconciliation.** When this spec was written, `alert_state()` read only
> the compiled `constexpr` bands, so the report could *display* an override it
> wasn't applying — #50 was out of scope and the caveat was to keep
> `settings_overrides_line()` in the diagnostics block and not present overrides
> as the active bands. **#50 has since merged** (`alert_state_live()` in
> `settings.h`), and #63 was rebased onto it: the verdict and boot zone labels now
> classify via `alert_state_live()`, so runtime overrides *are* reflected in the
> report. The `overrides:` line stays in the diagnostics block as a change log, and
> now genuinely matches the active bands.
>
> Two knowingly-deferred spots still use the compile-time `THRESH_*` bounds for a
> *displayed number* (not classification): the predictor trend/`build_predict_alert`
> target bound and `build_crit_alert`'s "limit" line. This matches the predictor
> itself, which #50 did not wire to live thresholds. Making those display the live
> bound is a small follow-up, tracked with the predictor's live-threshold work.

---

## Verification

1. `esphome compile telefridge.yaml`.
2. Run the host tests (`test/run.sh`) — snapshots, tag balance, length guard.
3. On hardware: button-forced report wake → confirm in the real chat that the
   `<pre>` columns align, the blockquote actually collapses, and the verdict line
   reads correctly.
4. Force each alert path (temporarily narrowed thresholds, or a sensor unplugged
   for the fault path) and confirm each renders and returns **HTTP 200** — a 400
   means malformed HTML.
5. **Unplug the SHT40 and confirm the report says `SENSOR FAULT`, not `OK`** —
   this is the regression that motivated the work.

---

## Documentation to update on landing

- **CLAUDE.md § Telegram report** — currently specifies "short and plain-text".
  Update to describe `parse_mode=HTML` and the four-tier hierarchy. The section
  is marked **LOCKED (transport)**; state explicitly that the transport is
  unchanged and this is a body/format change.
- **New ADR-017** recording the HTML switch and the tier model.
- **CLAUDE.md § File structure** — refresh the `logger/report.h` description.
- Tick CLAUDE.md:530 (the predictor state line) once it ships.
