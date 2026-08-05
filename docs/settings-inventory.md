# Runtime settings inventory & guardrails — TeleFridge V1

> **`#NN` references in this document cite the original `telefridge_V1` tracker**
> that this repo was split out of (ADR-025), not this repo's issues — the same
> numbers mean different things here.

> **Status:** Phase 1 spec (issue #32). This is the authoritative inventory that
> **#4** (`logger/settings.h`, the Telegram `/set*` command channel) codes
> against, and the guardrail contract **#31/#48**'s on-device web UI must mirror.
> Values here track `logger/helpers.h` and `telefridge.yaml`; if they drift, the
> compiled defaults in those files win and this doc is the bug.

## Scope & guiding principle

A value **earns a runtime slot only if it would plausibly need to change without
reflashing** — while tuning against the real appliance, or when the device's
situation changes (repurposing, post-incident watch, vacation). Everything else
stays a compile-time `constexpr` in `logger/helpers.h`.

Two delivery phases share this one inventory:

- **Phase 1 (this issue, #32 → #4):** the Telegram `/set*` command channel and
  its guardrails. Tier 1 + Tier 2 below.
- **Phase 2 (#48):** the same settings surfaced through #31's on-device web UI,
  plus UI-only action buttons and power-bank knobs. **The web UI must reuse the
  Phase 1 guardrails** — a portal path that bypasses the Telegram channel's
  validation would defeat the point (see Guardrails §Enforcement locus).

Persistence is NVS (rare writes, acceptable per **ADR-005**); the RTC-memory
ring buffer stays in RTC memory and is *not* moved to NVS.

---

## Tier 1 — required

These are the values a user tuning a real appliance (or repurposing the
hardware) will reach for first. `#4`'s `RuntimeSettings` carries both threshold
sets (per-sensor box/fridge — **ADR-011**).

| Setting | ID (`RuntimeSettings`) | Default | `min` | `max` | `step` | Why runtime-tunable |
|---------|------------------------|---------|-------|-------|--------|---------------------|
| Box CRIT low  | `box_crit_low`   | 2.0 °C | 0.0 | 10.0 | 0.1 | Box (`temp_a`, SHT30) pharma band. Strict 2–8 °C for insulin, but repurposing (drinks fridge, fermentation chamber) moves all four. |
| Box WARN low  | `box_warn_low`   | 3.0 °C | 0.0 | 10.0 | 0.1 | ″ |
| Box WARN high | `box_warn_high`  | 7.0 °C | 0.0 | 10.0 | 0.1 | ″ |
| Box CRIT high | `box_crit_high`  | 8.0 °C | 0.0 | 10.0 | 0.1 | ″ |
| Fridge CRIT low  | `fridge_crit_low`  | 0.0 °C | -20.0 | 30.0 | 0.1 | Fridge (`temp_b`, DHT12) advisory band — advisory in *authority* (the box is the compliance line), but a fridge CRIT still fires an early alert; only WARN is batched. Tunable to the appliance's compressor duty cycle. |
| Fridge WARN low  | `fridge_warn_low`  | 2.0 °C | -20.0 | 30.0 | 0.1 | ″ |
| Fridge WARN high | `fridge_warn_high` | 6.0 °C | -20.0 | 30.0 | 0.1 | ″ |
| Fridge CRIT high | `fridge_crit_high` | 9.0 °C | -20.0 | 30.0 | 0.1 | ″ |
| Samples per report | `report_every` | 16 (= 4 h) | 1 | 32 (`RING_CAPACITY`) | 1 | Bring-up/soak (1–2 → report every wake); post-incident watch (hourly for a day after a CRIT); vacation (stretch the supply). |

The box field range `[0.0, 10.0]` is also the **pharma-envelope hard guardrail**
(below) — for the box set the field bound *is* the safety bound. The `report_every`
`max` is bound to `RING_CAPACITY` (32), not a fixed literal (capacity-cap guardrail).

**`/setbox` / `/setfridge` command shape (informative, #4 owns the exact grammar):**
each command takes the four zone values for one sensor set and is validated as a
unit so ordering can be checked (see guardrails). A single-field convenience form
must still re-validate the whole set after applying the field.

---

## Tier 2 — recommended (same command channel, small extra effort)

| Setting | ID | Default | `min` | `max` | `step` | Notes |
|---------|----|---------|-------|-------|--------|-------|
| Sample interval (min) | `sample_interval_min` | 15 | 1 | 60 | 1 | Deep-sleep duration; `deep_sleep.enter` accepts a templated `sleep_duration`. 1–5 min while watching a compressor cycle during threshold tuning; 30–60 min for max battery life once trusted. Set **together with `report_every`** — they jointly fix report cadence. |
| CRIT re-alert hold-off (min) | `crit_realert_holdoff_min` | 0 (= reminder off, today's behaviour) | 0 | 1440 | 5 | Today a sustained excursion (door ajar overnight) produces one CRIT alert then silence until the sensor leaves the zone. A non-zero value re-sends a reminder every N minutes while still in CRIT; the right value is personal. `0` preserves current behaviour exactly. ("Hold-off", not "cooldown", to avoid clashing with thermal cooling — see issue #3.) |
| Alert debounce (samples) | `alert_debounce_n` | 1 (= today's behaviour) | 1 | 8 | 1 | N consecutive out-of-range samples before an absolute WARN/CRIT is honoured — CLAUDE.md's named fix for compressor-cycle alert chatter. `N=1` = current instant classification. **Touches the LOCKED alert mechanism → gated by ADR-015 (below), independent of its runtime tunability.** |

`sample_interval_min` and `report_every` are the two halves of the report-cadence
knob and should be presented together in both the Telegram help text and the web UI.

---

## Tier 3 — deliberately excluded (stay compile-time)

| Excluded | Reason |
|----------|--------|
| Telegram token / chat id | Stays in `secrets.yaml`. Putting it in NVS exposes it on the LAN during setup windows, and rotation is rare. |
| Quiet hours for CRIT alerts | CRIT is the food-safety boundary; silencing it is the "don't silently loosen CRIT" trap. WARN is already batched; nights are quiet by design. |
| °C/°F units, timezone | Personal constants, not tuning knobs. Compile-time is fine. |
| ~~Per-sensor threshold overrides~~ | **No longer excluded** — superseded by **ADR-011**. Per-sensor box/fridge bands *are* the model (the two Tier-1 threshold groups); `#4`'s `RuntimeSettings` carries both sets. |

---

## Predictor / approach settings — deferred addendum (not Phase 1)

The predictor (**ADR-013**) and approach trigger (**ADR-014**) are *design only*
today — `logger/predictor.h` does not exist and no code consumes these as runtime
values yet. Their compiled defaults already live in `logger/helpers.h`
(`TAU_BOX_MIN`, `PREDICT_HORIZON`, `PREDICT_SMOOTH_N`, `PREDICT_DEBOUNCE_N`,
`PREDICT_HOLDOFF_SAMPLES`). They are recorded here so the inventory is complete,
but a runtime slot for any of them is **out of scope for #4** until the predictor
ships. When it does, the recommended slots/guardrails are:

| Setting | ID | Default | `min` | `max` | Guardrail notes |
|---------|----|---------|-------|-------|-----------------|
| τ_box (min) | `tau_box_min` | commissioning fit (placeholder 120) | 30 | 720 | Must be fitted on the physical box before the predictor is trusted. |
| Predict horizon (min) | `predict_horizon_min` | 90 | 15 | 360 | Sanity-check against τ (comfortably shorter than the 4 h report cadence). |
| Predict smoothing (samples) | `predict_smooth_n` | 4 | 1 | 16 | Larger = slower to respond, better false-positive rejection during compressor cycling. |
| Predict debounce (samples) | `predict_debounce_n` | 2 | 1 | 8 | Kills brief door-open transients. |
| Predict hold-off (min) | `predict_holdoff_min` | 120 | 30 | 480 | Was `PREDICT_HOLDOFF_SAMPLES` (8 × 15 min); express as minutes at runtime so it survives a `sample_interval_min` change. |
| Approach margin (°C) | `approach_margin_c` | 0.5 | 0.1 | 1.0 | Box-only; top of the WARN band adjacent to a box CRIT bound. |
| Approach hold-off (min) | `approach_holdoff_min` | 120 | 30 | 360 | — |
| Escalation delta (°C) | `escalate_delta_c` | 1.5 | 0.5 | 5.0 | °C of worsening beyond the initial alert reading that bypasses the CRIT hold-off (edge case #18). |

---

## Guardrails — as important as the list itself

Every guardrail below is a **hard requirement on #4**. A fat-fingered setting
must never silently disable alerting.

### 1. Ordering validation (threshold sets)
For **both** the box and the fridge set, enforce on every change:

```
crit_low ≤ warn_low < warn_high ≤ crit_high
```

A typo like `warn_high = 50` instead of `5.0` must be **rejected** (preferred) or
clamped — never accepted silently, because it would collapse a zone and disable
alerting. A single-field update must re-validate the whole set. Combine with the
per-field `min`/`max`/`step` bounds in the tables above.

### 2. Pharma envelope (box CRIT hard bound)
`box_crit_low` and `box_crit_high` must fall inside `[0.0, 10.0]`. A `/setbox`
that moves either outside is **rejected with a specific error message** — this is
a safety requirement, not a UX preference. (For the box set this coincides with
the field `min`/`max`, so the field bound enforces it too; keep the explicit
check so the intent is legible and survives a field-range edit.)

### 3. Capacity cap
`report_every ≤ RING_CAPACITY` (32). Above capacity the ring buffer overwrites
the oldest samples before the report fires, silently truncating the digest. The
field `max` is bound to the `RING_CAPACITY` constant, not a literal, so the two
can't drift.

### 4. NaN / unset fallback
Every runtime value **falls back to its compiled default in `logger/helpers.h`**
when NVS is empty or the stored value is NaN/unset. A factory-fresh device (or
one whose NVS was wiped) must behave **exactly like today's firmware**. This is
the invariant that lets the whole feature ship without changing default behaviour.

### 5. Remote visibility
The 4-h Telegram report **echoes any active setting that differs from its
compiled default** (a compact "overrides:" line). A misconfiguration — or a
housemate's experiment — is then spotted remotely, not after a spoiled fridge.
Settings equal to their defaults are omitted to keep the report short.

### Enforcement locus
The guardrails live in `logger/settings.h` (#4) and run on the **write path**,
so they apply no matter who calls the setter. The Phase 2 web UI (#31/#48) must
route writes through the same setter — it must **not** re-implement or bypass the
validation. `number` entities in the web UI still declare matching
`min`/`max`/`step` for good UX, but the `settings.h` write path is the authority.

---

## Companion ADR — required for the debounce knob

`alert_debounce_n` changes *when* an absolute WARN/CRIT is honoured, which touches
the **LOCKED** alert mechanism (§Thresholds & alerts). Runtime tunability does not
exempt it from needing a decision record. See **ADR-015** in CLAUDE.md (added with
this issue). `N=1` — the shipped default — is exactly today's instant
classification, so the ADR is about *allowing* debounce, not changing the default.

---

## Refs

- **#4** — consumes this inventory + guardrails via `logger/settings.h` (the `/set*` parser + `apply_command()` write path).
- **#33 / #8** — delivers those commands remotely: `logger/commands.h` polls Telegram `getUpdates` on radio wakes and routes each authorised line through `apply_command()` (ADR-016).
- **#48** — Phase 2 web-UI delivery of the same settings (split out from #32).
- **#31** — on-device web UI (Phase 2 host).
- **#18** — escalation-delta edge case (deferred addendum).
- CLAUDE.md §Thresholds & alerts (values TUNABLE / mechanism LOCKED), ADR-005, ADR-011, ADR-013, ADR-014, **ADR-015**.
