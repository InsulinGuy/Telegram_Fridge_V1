# CLAUDE.md — TeleFridge V1

> **Read this file in full at the start of every session.** It is the single
> source of truth for this project. Sections marked **LOCKED** must not change
> without an explicit ADR.
>
> ⚠️ Licensed **GPL-3.0-or-later** ([`LICENSE`](./LICENSE)). Hobbyist project,
> **NO WARRANTY**, **not a medical device** — must not be the sole safeguard for
> insulin, vaccines, or any temperature-sensitive payload. **Read
> [`DISCLAIMER.md`](./DISCLAIMER.md) before use.**

---

## Project overview

**TeleFridge V1** is an ESPHome-based low-power, headless **refrigerated-payload
logger** on the **M5StickC** (ESP32-PICO-D4). It monitors an **insulated box
placed inside a domestic fridge** — the box holds a temperature-sensitive payload
(pharmaceutical, insulin-class, spec range **2–8 °C**); the fridge is the outer
cooling environment. Two I²C sensors, **on two separate buses**, sample on a
fixed interval:

- **SHT30** (M5 ENV II Unit, 0x44, Grove) → **`temp_a`**, **inside the box**. The
  **primary safety sensor**: its reading is what the payload sees, and it is the
  authority for pharma-spec (2–8 °C) compliance.
- **DHT12** (M5 ENV HAT, 0x5C, HAT header) → **`temp_b`**, in the **fridge air,
  outside the box**. Advisory **leading indicator**: it detects fridge/appliance
  failure before the box thermal mass catches up.

Readings accumulate in an RTC-memory ring buffer while the device sleeps; it
**wakes Wi-Fi once every 4 h to push a short Telegram report** and otherwise
lives in deep sleep. Why the roles are asymmetric and locked is in Hardware →
Sensors.

**This project has no always-on display, no SD card, no screen inventory, and no
navigation model** — the onboard TFT lights only for a 3 s status screen on a
Button A press, and is dark on every other wake (ADR-026). It is a sibling to **Tadorna** and **Tadorna
e-Ink** but a separate repo with its own invariants — do **not** port any
always-on `display:`, screen-inventory, or refresh-loop concept from them. It is
also not real-time (readings are batched every 4 h) and not mains-powered
(optimised for lowest average current).

**ESPHome:** 2026.4.5 · **Device name:** telefridge · **Repo:**
`Telegram_Fridge_V1` on GitHub — referred to throughout this document by its
working name **telefridge_StickC_V1** (split from `telefridge_V1` on 2026-08-04,
ADR-025).

---

## Hardware

### Board — M5StickC / ESP32-PICO-D4 (ADR-020)

- MCU: ESP32-PICO-D4 (dual-core Xtensa LX6), 4 MB flash, **no PSRAM** — declaring
  a `psram:` block boot-loops the device.
- USB-C; onboard 0.96" ST7735S TFT (**a 3 s screen on a Button A press only**,
  ADR-026; its AXP192 rails are held off at every other moment to save power).
- **AXP192 PMIC** (0x34) — LiPo charger, rail control, and the **only** source of
  battery telemetry: no ADC divider, no fuel gauge.
- Onboard **~95 mAh** Li-ion cell, the sole supply (ADR-022).
- **Grove** HY2.0-4P port (G32/G33) and an 8-pin **HAT** header
  (G0/G26/G36/BAT/3V3/5V/GND) — two independent I²C-capable pin pairs, which is
  what lets this build run two sensors that would otherwise collide.
- Buttons A (G37) and B (G39), both RTC-capable, input-only.
- Docs: https://docs.m5stack.com/en/core/m5stickc

### Sensors

| Sensor   | Bus (pins)          | Addr | Role                                   |
|----------|---------------------|------|----------------------------------------|
| SHT30 (ENV II Unit) | `bus_box` — Grove, G32/G33 | 0x44 | `temp_a` — **box interior** (PRIMARY safety sensor, ±0.2 °C) |
| DHT12 (ENV HAT)     | `bus_fridge` — HAT, G0/G26 | 0x5C | `temp_b` — **fridge ambient** (advisory leading indicator, ±0.5 °C) |
| AXP192 PMIC         | bit-banged — internal, G21/G22 | 0x34 | Battery voltage (regs 0x78/0x79). No divider, no fuel gauge. Software I²C, not an `i2c:` bus (ADR-023). |

Both temperature parts are M5-native and sit on **separate I²C buses** because
they are on separate physical connectors. Their addresses also don't collide
(0x5C vs 0x44), keeping a rewire-onto-one-bus fallback available. The ENV II
Unit's BMP280 (0x76) and the ENV HAT's BMP280 (0x76) + BMM150 (0x10) are present
but **not configured**. History of the part choices: ADR-021.

**Sensor roles are asymmetric and LOCKED (ADR-011).** `temp_a` (SHT30) lives
*inside* the box; its reading is what the payload experiences and is the
pharma-compliance authority (2–8 °C). `temp_b` (DHT12) reads the fridge air
*outside* the box; it detects appliance failure early (door open, compressor
dead) before the box's thermal mass falls behind. Their IDs and placement
(`temp_a` = box interior, `temp_b` = fridge ambient) must **never** be swapped —
the alert logic and predictor depend on it. Both are logged and threshold-checked
against **different** per-sensor sets (see Thresholds & alerts).

> **On this board the role split is also *forced* by accuracy (ADR-021).** The
> DHT12 is ±0.5 °C and the box WARN shoulder is only 1.0 °C wide, so the DHT12
> **cannot** hold the compliance role. The accurate SHT30 (±0.2 °C) is also the
> one on a cable — the only one that can physically reach inside a sealed box.
> Swapping them is a compliance defect, not a preference.

> **ESPHome platforms.** SHT30 = `platform: sht3xd`, DHT12 = `platform: dht12` —
> both **hub components**, so `component.update:` targets the hub id
> (`sht30_hub` / `dht12_hub`), and the value is read from the sub-sensor
> (`id(temp_a).state`, `id(temp_b).state`). Both conversions also yield humidity,
> not published (temps-only). Grove VCC is **5 V** (AXP192 boost via EXTEN — see
> Pin assignments). **Fallback:** if the DHT12 misbehaves in condensing fridge
> air (a known DHT weakness), the same HAT's **BMP280 (0x76)** is a drop-in
> `temp_b` source via `platform: bmp280_i2c` — a YAML-only swap.
> Refs: [ENV II](https://docs.m5stack.com/en/unit/envII) ·
> [ENV HAT](https://docs.m5stack.com/en/hat/hat-env)

---

## Pin assignments

Against the M5StickC pinout. All bring-up checks have now passed on hardware
(see Open items); nothing here remains pending verification.

### I²C buses — LOCKED (ADR-020, amended by ADR-023)

**Two hardware buses — all the ESP32 has.** The PICO-D4 provides exactly two I²C
peripherals and ESPHome enforces the limit, so a third `i2c:` bus is not
buildable. Both hardware buses go to the temperature sensors (on separate
connectors, so they cannot share one); the PMIC is bit-banged instead (ADR-023).

| Bus id       | SDA / SCL   | Devices                          |
|--------------|-------------|----------------------------------|
| `bus_box`    | G32 / G33   | ENV II Unit SHT30 (0x44) → `temp_a` — Grove port |
| `bus_fridge` | G0 / G26    | ENV HAT DHT12 (0x5C) → `temp_b` — HAT header |
| *(bit-banged)* | G21 / G22 | AXP192 PMIC (0x34) → battery — internal, **not an `i2c:` bus** (ADR-023) |

> **G0 is an ESP32 strapping pin (boot mode).** The ENV HAT wires SDA to it;
> anything holding that line LOW at reset stops the device booting. In practice
> it boots cleanly — I²C idles high via the pull-ups (confirmed on hardware,
> Open items #1). *Fallback:* since 0x5C and 0x44 don't collide, the HAT's
> sensors can be rewired onto `bus_box` and the G0/G26 bus dropped.

### Sensor power — no switched rail (ADR-020)

There is **no switched sensor rail** on this board — sensors are permanently
powered from the AXP192's rails, so there is no cold-start warm-up to schedule
(the retained 100 ms pre-read delay is settling margin, not a warm-up). The Grove
port's VCC is **5 V**, boosted by the AXP192 and gated by the **EXTEN bit in
register 0x12** — `axp192_init()` forces it on so the ENV II Unit is powered.
Grove 5 V is confirmed present on the cell, not just USB (Open items #3).
*Fallback:* power the Unit from the HAT header's 3V3 pin (SHT30 is 3.3 V native).

### Battery / power monitoring

| Signal   | Bus / addr        | Notes                                           |
|----------|-------------------|-------------------------------------------------|
| Cell V   | G21/G22 (bit-banged) 0x34, regs 0x78/0x79 | AXP192 12-bit battery ADC, 1.1 mV/LSB. **No divider, no fuel gauge on this board.** |

Battery voltage is a direct AXP192 register read via `logger/axp192.h` over its
own **bit-banged software I²C** on G21/G22 (ADR-023), feeding a template sensor
`batt_v`; SoC (%) is estimated from voltage by `li_ion_soc()` in
`logger/helpers.h`. Both ride in every Telegram report. **A failed read returns
NaN, never 0** — every downstream path degrades NaN to "battery n/a", whereas a 0
would look like a flat cell and trip the protective floor.

> **The AXP192's ADCs are disabled at power-on** and ESPHome never calls M5's
> `Axp.begin()`, so `axp192_init()` **must write reg 0x82 on every wake** or the
> voltage registers read zero. Confirmed plausible on the *first* wake on
> hardware (4.08 V / 94 %, Open items #4).

### LED / buttons / display

| Signal        | GPIO | Notes                                                     |
|---------------|------|-----------------------------------------------------------|
| Red LED       | 10   | Active LOW. Keep dark in normal operation (power).        |
| Button A      | 37   | ext0 deep-sleep wake source (on-demand report + 3 s screen). |
| Button B      | 39   | Unused.                                                   |
| TFT           | SPI: CLK 13, MOSI 15; CS 5, DC 23, RST 18 | **3 s Button A status screen ONLY (ADR-026, supersedes the display half of ADR-002).** ST7735S, LDO2/LDO3 driven by `axp192_lcd_on/off()`; dark on sampling wakes, report wakes and cold boot alike. |
| RESET         | —    | Hardware reset button (not a GPIO).                       |

Button A (GPIO37) is the **ext0 deep-sleep wake source** ("wake, connect, send
now, sleep"). It is an input-only RTC GPIO (valid for ext0), and the buttons pull
to GND — hence `inverted: true`. GPIO37 is confirmed pulled up (the 15-min sleep
holds untouched — Open items #2), so the floating-ext0 power-budget hazard does
not apply on this board.

---

## Power architecture — the defining constraint

### Supply — LOCKED (ADR-022)

Powered by the M5StickC's **onboard ~95 mAh single-cell Li-ion**, the **sole
supply**, charged by the AXP192 over USB-C. There is no permanent USB source —
recharging is a manual/periodic action, and the firmware **low-battery Telegram
warning is the cue to do it**. *Future option (not scope):* the HAT header's
`BAT` pin accepts a larger parallel cell the AXP192 will charge (ADR-022).

### The defining constraint — battery runtime

**Runtime per charge is the defining constraint, and much tighter on this board.**
Versus the retired Feather, capacity dropped ~3000 → ~95 mAh *and* the AXP192 adds
continuous quiescent draw. So runtime is dominated by **PMIC idle current, not the
wake cadence** — lengthening `SAMPLE_INTERVAL` buys little, which is why the
15 min / 4 h cadence is kept unchanged. **Days, not months, is an accepted trade.**
Measure the real average current on hardware and record it in ADR-022. The power
levers that remain: keeping the TFT rails (LDO2/LDO3) off outside the brief
3 s button screen (enforced by an `on_shutdown` hook, so it holds at every sleep
site — ADR-026), and ADR-006's radio policy (there is no switched-rail lever on
this board).

### Battery monitoring & protection — firmware

- **Voltage** from the AXP192 each wake (`component.update: batt_v`); **SoC (%)**
  estimated from it (`batt_pct`, updated after `batt_v`). Both appear in every
  report and the boot message; NaN degrades to "battery n/a". SoC from voltage is
  approximate (voltage sags under load) — there is no fuel gauge (see Pin
  assignments for the reg-0x82 ADC-enable requirement).
- **Low-battery warning:** below `BATT_WARN_PCT` (**20 %**) the firmware fires an
  early Wi-Fi alert, latched (`g_last_batt_alert`) so it sends **once per
  depletion**, clearing on recharge past `BATT_RECOVER_PCT` (**30 %**). On a
  95 mAh cell this gives far less notice than the 18650 did — treat it as urgent;
  consider raising `BATT_WARN_PCT` once discharge data exists.
- **Protective floor:** below `BATT_CRIT_V` (**3.30 V**) the scheduled 4-h report
  is **skipped** to conserve charge. Temp-**CRIT** alerts still send — food safety
  outranks battery conservation. Thresholds are `constexpr` in `logger/helpers.h`.

---

## Operating cycle — LOCKED

The firmware is a **wake → sample → maybe report → sleep** state machine driven by
deep sleep. There is no continuous loop and no always-on Wi-Fi.

```
        ┌─────────────── deep sleep (Wi-Fi OFF) ───────────────┐
        │                                                      │
   wake (timer every SAMPLE_INTERVAL, or button)               │
        │                                                      │
   settle ─ read SHT30 (temp_a) + DHT12 (temp_b) + AXP192 battery
        │                                                      │
   push sample into RTC ring buffer ; wake_count++             │
        │                                                      │
   evaluate alerts:                                            │
     • box CRIT (absolute, temp_a)   ──► early Wi-Fi alert     │
     • predicted breach (t_to_hit)   ──► early Wi-Fi alert     │
     • approach (temp_a near CRIT)   ──► early scheduled report│  (not implemented)
     • all others (WARN, fridge CRIT) ──► batched in report    │
        │                        (each with own hold-off)      │
   wake_count reached REPORT_EVERY? ──► Wi-Fi ON               │
        │                              connect + Telegram      │
        │                              report + clear buffer   │
        └──────────────────────► sleep again ─────────────────┘
```

- **`SAMPLE_INTERVAL`** — how often to wake and read (default **15 min**).
- **`REPORT_EVERY`** — samples per report; default **16** → a report every **4 h**
  at a 15-min interval. Change both together to keep the 4-h cadence.
- Sampling wakes do **not** enable Wi-Fi. Only report wakes (and CRIT-alert wakes)
  enable the radio — this is where nearly all the energy would otherwise go.

Implementation status for every alert path lives in **Open items** (the single
source of truth); the predictor's output is unvalidated while `TAU_BOX_MIN`
remains the placeholder (ADR-013).

### Wi-Fi policy — LOCKED

`wifi:` is configured with **`enable_on_boot: false`**. The radio is brought up
explicitly with `wifi.enable` only on a report or alert wake, and the device does
not sleep until the send has completed or timed out (`deep_sleep.prevent` around
the send, `deep_sleep.enter` on completion/timeout). Never leave Wi-Fi on across a
sampling wake.

---

## RTC-memory ring buffer — LOCKED

Data accumulated between reports lives in **RTC slow memory**, which survives deep
sleep (but **not** a full power loss / bank shutoff). This was chosen over NVS to
avoid flash wear from frequent small writes.

The full `RTC_DATA_ATTR` block (all wiped by a power loss) holds **more than the
samples** — be aware of the whole footprint:

- The **`Sample` ring** — up to **`RING_CAPACITY`** entries (default 32, headroom
  over the 16 needed per report), each `{ uint32_t epoch; float temp_a;
  float temp_b; }`.
- **`g_wake_count`** (`uint16_t`) — wakes since the last report.
- **`g_last_alert`** (`AlertState`) — for the box-CRIT hold-off.
- **Predictor state (ADR-013):** the last `PREDICT_SMOOTH_N` `temp_b` samples
  (`g_fridge_hist`, fridge EMA) and `temp_a` samples (`g_box_hist`, box EMA), the
  last `PREDICT_DEBOUNCE_N` decisions (`g_predict_hist`), `g_last_predict_alert`,
  and `g_predict_holdoff`. See `logger/predictor.h`.
- **Re-alert / send-ack state (ADR-018/019):** the CRIT episode + reminder clocks
  and `g_last_send_epoch` (`mark_send_ok()`). See `logger/ring_buffer.h`.

Implemented in `logger/ring_buffer.h`, pulled in via `esphome: includes:`.

- On a report wake: summarise the buffer (per-sensor current / min / max / mean /
  count), build + send the Telegram text, then **clear the buffer and reset
  `g_wake_count`**.
- **Power-loss behaviour:** if the bank cuts power, RTC memory is lost and the
  buffer starts empty on next boot. Acceptable — the report is a running digest,
  not an audit log. Gap-proof history would be a new ADR (append digests to NVS,
  or a real logging sink).

Do **not** move the ring buffer to `globals: restore_value: true` — that path
uses NVS/flash and reintroduces the wear this design avoids. (Settings and the
Telegram command cursor *do* live in NVS — they change rarely and must survive a
full power loss; see ADR-012.)

---

## Thresholds & alerts — LOCKED (mechanism) / TUNABLE (values)

> Which values are runtime-tunable (no reflash), and the guardrails that keep a
> bad `/set` from disabling alerting, are specced in `docs/settings-inventory.md`
> — the contract `logger/settings.h` codes against.

The two sensors are **asymmetric** (ADR-011): the box interior is the
payload-safety authority; the fridge sensor is a leading indicator. There are
**four alert sources**, each with its own per-sensor threshold set:

1. **Box absolute thresholds** (`temp_a`, five-zone) — the pharma-compliance
   authority; CRIT is a real "act now" event. **Implemented.**
2. **Fridge absolute thresholds** (`temp_b`, five-zone) — advisory *in authority*
   (it is not the compliance line), but its CRIT **does** fire an early Wi-Fi
   alert: a fridge in CRIT is an appliance failure, and waiting up to 4 h to say
   so wastes the lead time the sensor exists to buy. WARN stays batched.
   **Implemented.**
3. **Predicted breach** — physics-based estimate of when the box will cross a CRIT
   bound. Adds `ALERT_PREDICTED_BREACH_LOW/HIGH`. **Implemented — unvalidated
   until `TAU_BOX_MIN` is calibrated.**
4. **Approach trigger** — box quietly near a CRIT bound with no gradient; brings
   the scheduled report forward. **Design only, not implemented (ADR-014).**

The absolute sources feed the five base states (`ALERT_OK, ALERT_WARN_LOW,
ALERT_WARN_HIGH, ALERT_CRIT_LOW, ALERT_CRIT_HIGH`); the predictor adds
`ALERT_PREDICTED_BREACH_LOW/HIGH` and the sensor-fault detector adds
`ALERT_SENSOR_FAULT`, for an **eight-state** enum. All values are `constexpr` in
`logger/helpers.h`.

### Box interior (`temp_a`, SHT30) — pharma-spec, LOCKED bounds

CRIT boundaries are pinned to the **insulin/pharma refrigerated range 2–8 °C**.
Do not silently loosen these — they are the compliance line.

|      | Zone          | Condition                | Value        | Alert state      |
|------|---------------|--------------------------|--------------|------------------|
| 🟦   | CRITICAL_LOW  | `< THRESH_BOX_CRIT_LOW`   | < 2.0 °C     | ALERT_CRIT_LOW   |
| 🔷   | WARNING_LOW   | crit_low .. warn_low     | 2.0–3.0 °C   | ALERT_WARN_LOW   |
| 🟢   | OK            | warn_low .. warn_high    | 3.0–7.0 °C   | ALERT_OK         |
| 🟡   | WARNING_HIGH  | warn_high .. crit_high   | 7.0–8.0 °C   | ALERT_WARN_HIGH  |
| 🔴   | CRITICAL_HIGH | `> THRESH_BOX_CRIT_HIGH`  | > 8.0 °C     | ALERT_CRIT_HIGH  |

`THRESH_BOX_CRIT_LOW = 2.0`, `THRESH_BOX_WARN_LOW = 3.0`,
`THRESH_BOX_WARN_HIGH = 7.0`, `THRESH_BOX_CRIT_HIGH = 8.0` (°C).

### Fridge ambient (`temp_b`, DHT12) — advisory bounds

Wider bands, shifted slightly lower (a fridge holding 2 °C is fine; a box at 2 °C
is on the edge). These bounds are **advisory in authority** — the box (`temp_a`)
is the compliance line, and nothing here can put the payload out of spec on its
own. But a fridge CRIT still **fires an early Wi-Fi alert** (`temp_b` in a CRIT
zone means the appliance itself has failed), alongside the predictor. Fridge WARN
is batched into the 4-h digest.

|      | Zone          | Condition                   | Value        | Alert state      |
|------|---------------|-----------------------------|--------------|------------------|
| 🟦   | CRITICAL_LOW  | `< THRESH_FRIDGE_CRIT_LOW`   | < 0.0 °C     | ALERT_CRIT_LOW   |
| 🔷   | WARNING_LOW   | crit_low .. warn_low        | 0.0–2.0 °C   | ALERT_WARN_LOW   |
| 🟢   | OK            | warn_low .. warn_high       | 2.0–6.0 °C   | ALERT_OK         |
| 🟡   | WARNING_HIGH  | warn_high .. crit_high      | 6.0–9.0 °C   | ALERT_WARN_HIGH  |
| 🔴   | CRITICAL_HIGH | `> THRESH_FRIDGE_CRIT_HIGH`  | > 9.0 °C     | ALERT_CRIT_HIGH  |

`THRESH_FRIDGE_CRIT_LOW = 0.0`, `THRESH_FRIDGE_WARN_LOW = 2.0`,
`THRESH_FRIDGE_WARN_HIGH = 6.0`, `THRESH_FRIDGE_CRIT_HIGH = 9.0` (°C).

`alert_state(float value, bool is_box)` selects the box set when `is_box` is true,
the fridge set otherwise. Call with `is_box = true` for `temp_a`, `false` for
`temp_b`. The emoji scale above is the zone gradient `zone_emoji()` renders in the
Telegram verdict line — docs and code share one visual language. Note the cold
CRIT zone is 🟦 by decision: severity there is carried by the `TOO COLD` label and
the alert's limit line, **not** by hue, and it is no less a compliance breach than
🔴. Sensor fault renders **🔧** — deliberately off the scale in shape as well as
hue, since it is the absence of a reading rather than a point on it.

### Predictive alert — IMPLEMENTED (`logger/predictor.h`) / TUNABLE (values)

The box is a lumped thermal mass exchanging heat with the fridge air (Newton's law
of cooling): `T_box = temp_a`, `T_fridge = temp_b`. Time until the box crosses a
box CRIT bound `T_thresh`:

```
    t_to_threshold  =  τ_box · ln( (T_fridge − T_box_now) / (T_fridge − T_thresh) )
```

Directional: `T_thresh = THRESH_BOX_CRIT_HIGH` (8 °C) when the box heads up
(`T_fridge > T_box`), `THRESH_BOX_CRIT_LOW` (2 °C) when it heads down. In normal
operation (`T_fridge` between the two bounds) `t` is very large/infinite → no
alert.

**Firing rule** — each sample: EMA-smooth `T_fridge` over `PREDICT_SMOOTH_N`
samples; compute both ETAs, keep the smaller finite one; fire
`ALERT_PREDICTED_BREACH_HIGH`/`_LOW` (early Wi-Fi alert) when **all** hold —
`t < PREDICT_HORIZON`, the same side has held for `PREDICT_DEBOUNCE_N` samples
(kills door-open transients), and not within `PREDICT_HOLDOFF_SAMPLES` of the last
alert. `temp_a` is smoothed alongside `temp_b` for single-sample glitch rejection.

Constants (`constexpr` in `helpers.h`): `TAU_BOX_MIN` (**uncalibrated placeholder
— output unvalidated until the box thermal time constant is fitted; see
ADR-013**),
`PREDICT_HORIZON` **90 min**, `PREDICT_SMOOTH_N` **4** (60 min), `PREDICT_DEBOUNCE_N`
**2** (30 min), `PREDICT_HOLDOFF_SAMPLES` **8** (2 h). Logic (`ema`,
`t_to_threshold`, `predictor_evaluate`, `predictor_step`) is host-tested in
`test/test_predictor.cpp`.

### Approach trigger — DESIGN, not implemented (ADR-014) / TUNABLE (values)

Covers the gap between the predictor (needs a gradient) and the 4-h report: the
box quietly sits close to a CRIT bound with no gradient. If `THRESH_BOX_CRIT_HIGH
− temp_a` or `temp_a − THRESH_BOX_CRIT_LOW` falls in `0 < approach <
APPROACH_MARGIN_C` (**0.5 °C**) and not within hold-off, it brings the scheduled
report forward (normal body prefixed `EARLY:`, clears the ring, resets cadence).
Box-only; box CRIT and predicted breach take precedence on the same wake.

### Alert reporting policy — LOCKED

| Source                        | Behaviour                                              |
|-------------------------------|--------------------------------------------------------|
| Box CRIT (`temp_a`, absolute) | **Early Wi-Fi alert** on entry, hold-off armed on send-ack |
| Box CRIT — still in-zone      | **Reminder** every `crit_realert_holdoff_min` min (`0`=off); **escalation** on ≥1.5 °C worsening — ADR-019 |
| Box WARN (`temp_a`, absolute) | Batched into scheduled 4-h report                      |
| Fridge CRIT (`temp_b`, abs.)  | **Early Wi-Fi alert** on entry (shares the box-CRIT script + `g_last_alert` hold-off); **no** reminder/escalation |
| Fridge WARN (`temp_b`, abs.)  | Batched into scheduled 4-h report                      |
| Predicted breach              | **Early Wi-Fi alert** on entry, hold-off armed on send-ack; **escalation** on ETA halving |
| Box approach (`temp_a`)       | **Early scheduled report** (not alert), resets cadence *(design)* |

- The early **CRIT** path fires when **either** sensor enters a CRIT zone
  (`is_crit(a) || is_crit(b)`) and uses `g_last_alert` as a shared hold-off, so it
  doesn't re-alert every sample while either reading stays critical. The body
  names whichever sensor(s) are in CRIT (`build_crit_alert("Box"/"Fridge", …)`).
  The scheduled report still fires normally.
- **Only the box re-alerts.** While in-zone, a time **reminder** or a worsening
  **escalation** (ADR-019) speaks again only for `temp_a` — a fridge CRIT alerts
  once on entry and then goes quiet until it clears and re-enters. That
  asymmetry *is* the fridge's advisory status: loud enough to interrupt once,
  never loud enough to nag.
- **Hold-offs advance on send-acknowledgement, not on state entry (ADR-018).** A
  failed send leaves the latch untouched, so the next wake re-evaluates and
  retries instead of going silent mid-emergency.
- Threshold checks use each sensor's own reading against its own set — no
  cross-sensor logic in the absolute paths (only the predictor combines them).
- **Separate ESPHome scripts** for CRIT alerts vs. scheduled reports: the CRIT
  path calls `build_crit_alert()` and **leaves the buffer intact**; only the
  report path (`build_report()`) clears the ring and resets `g_wake_count`.

---

## Telegram report — LOCKED (transport)

There is **no native Telegram component** in ESPHome. Reporting is an
`http_request` POST to the Bot API:

```
POST https://api.telegram.org/bot<BOT_TOKEN>/sendMessage
body: chat_id=<CHAT_ID>&parse_mode=HTML&text=<url-encoded message>
```

- **Secrets:** `telegram_bot_token` + `telegram_chat_id` in `secrets.yaml` (kept
  split, not a composite URL, so each rotates independently). Build the URL in the
  lambda.
- **Body:** built in `logger/report.h` from the ring-buffer digest, using
  **`parse_mode=HTML`** with a four-tier hierarchy (transport is LOCKED; the body
  format is not). The tiers: **0 verdict** — one bold line answering *is the box
  safe?* (`temp_a`), with a status emoji following the directional zone gradient
  (🟦/🔷/🟢/🟡/🔴, plus ⚠️ predicted breach and 🔧 sensor fault; a faulted sensor
  reports **FAULT**, never a fabricated "OK"); **1 evidence** — box trend line +
  per-sensor min/max/mean `<pre>` table; **2 context** — fridge advisory + excursion count; **3
  housekeeping** — window/uptime/wake/RSSI/fw/cadence/overrides in an expandable
  blockquote, battery just above. Full spec + escaping/tag-whitelist rules:
  `docs/telegram-message-ia.md` (ADR-017). Alert bodies add an `alert_footer()`
  (battery + wall-clock); the inbound command reply stays **plain-text**.
- **Device label:** every message is prefixed `Stick C` via `with_device_tag()`
  **at the send site** (ADR-024) — because a sibling board posts to the same chat.
- **RSSI** from a `wifi_signal` sensor; **uptime** is `millis()/1000` in the
  lambda (no extra sensor).
- **Buffer clear on 2xx only:** the report path clears the buffer inside the
  `on_response` 2xx branch, so a failed send is retried on the next 15-min wake.
- **TLS:** `verify_ssl: true` with a **pinned CA**
  (`ca_certificate_path: certs/telegram_root_ca.pem`, GoDaddy Root CA G2, valid to
  2037) — pinning the root survives Telegram's yearly leaf renewals. Verified on
  hardware (HTTP 200). Keep this setup. (The old intermittent-send failures were
  network-side — see Known issues — not TLS.)
- **Time before TLS:** every send waits for `sntp_time` to become valid (15 s
  timeout) after Wi-Fi connects, before the POST. Do not remove these waits.

---

## ESPHome configuration skeleton

The authoritative config is **`telefridge.yaml`** — this is a shape reference, not
a copy of it. The **load-bearing invariants** (things that break the build or the
device if changed):

- **`esp32: board: m5stick-c`**, `framework: esp-idf` (needed for deep-sleep +
  RTC-memory control; Arduino is not used). **No `psram:` block** — the PICO-D4
  has none; declaring it boot-loops the device.
- **Exactly two `i2c:` buses** — `bus_box` (G32/G33) and `bus_fridge` (G0/G26).
  The ESP32 has only two I²C peripherals and ESPHome enforces it; the AXP192 is
  bit-banged in `logger/axp192.h`, **not** a third bus (ADR-023).
- **Sensors** `update_interval: never` — readings are taken programmatically via
  `component.update:` each wake, targeting the **hub id** (`sht30_hub`/`dht12_hub`),
  not the sub-sensor. `batt_pct` updates *after* `batt_v`.
- **`wifi: enable_on_boot: false`** (see Wi-Fi policy).
- `on_boot` runs `axp192_init();` once per wake (enable ADCs, Grove 5 V on, TFT
  rails left as found — no bus argument, the PMIC is bit-banged). A higher-priority
  (900) `on_boot` lambda classifies `g_button_wake` and runs `axp192_lcd_on()` on
  button (ext0) wakes only, above the display component's ST7735 init (ADR-026);
  `axp192_lcd_off()` runs from an **`on_shutdown` hook**, which ESPHome fires at
  every `deep_sleep.enter`.
- `deep_sleep: sleep_duration: 15min` (== `SAMPLE_INTERVAL`), wake also on GPIO37
  (Button A) via ext0.

```yaml
esphome:
  name: telefridge
  includes: [logger/axp192.h, logger/helpers.h, logger/ring_buffer.h,
             logger/predictor.h, logger/settings.h, logger/realert.h,
             logger/commands.h, logger/report.h, logger/build_number.h]
  on_boot:
    - priority: 100
      then: [lambda: 'axp192_init();']

esp32:
  board: m5stick-c
  framework: {type: esp-idf}     # NO psram: block

i2c:
  - {id: bus_box,    sda: GPIO32, scl: GPIO33, scan: true}   # Grove → ENV II (0x44)
  - {id: bus_fridge, sda: GPIO0,  scl: GPIO26, scan: true}   # HAT → ENV HAT (0x5C); G0 is a strapping pin
  # NO third bus — AXP192 (G21/G22, 0x34) is bit-banged (ADR-023)

sensor:
  - {platform: sht3xd, i2c_id: bus_box,    address: 0x44, id: sht30_hub,
     temperature: {id: temp_a}, update_interval: never}
  - {platform: dht12,  i2c_id: bus_fridge, id: dht12_hub,
     temperature: {id: temp_b}, update_interval: never}
  - {platform: template, id: batt_v,   update_interval: never, lambda: 'return axp192_batt_voltage();'}
  - {platform: template, id: batt_pct, update_interval: never, lambda: 'return li_ion_soc(id(batt_v).state);'}

wifi:
  enable_on_boot: false          # radio off except on report/alert wakes
  ssid: !secret wifi_ssid
  password: !secret wifi_password

deep_sleep:
  id: deep_sleep_1
  sleep_duration: 15min          # == SAMPLE_INTERVAL; also wakes on GPIO37 ext0
```

Time uses `sntp` (only syncs on report/alert wakes). Sample timestamps between
reports come from the RTC/`millis` offset — do not assume a fresh SNTP time on
every sampling wake. Don't inline large lambdas in YAML; they live in `logger/*.h`.

---

## File structure

```
telefridge_StickC_V1/         ← published on GitHub as Telegram_Fridge_V1
├── CLAUDE.md                 ← this file
├── README.md                 ← the public front door: non-technical, user-facing
├── telefridge.yaml           ← main ESPHome config (authoritative)
├── secrets.yaml.example      ← template for wifi + Telegram + OTA secrets (real secrets.yaml is git-ignored)
├── .gitignore                ← secrets.yaml / secrets_*.yaml / *.bak + build artifacts
├── LICENSE                   ← GPL-3.0-or-later
├── DISCLAIMER.md             ← liability / not-a-medical-device disclaimer
├── SECURITY.md               ← what secrets the device holds, the bot-token-in-logs leak, rotation + reporting
├── CONTRIBUTING.md           ← contributor rules; restates the LOCKED 2–8 °C bounds + sensor roles
├── .claude/settings.json     ← PreToolUse hook: runs scripts/stamp_version.py before any esphome run/compile
├── .github/workflows/        ← release CI (tag push → GitHub Release) + host-test CI
├── certs/
│   └── telegram_root_ca.pem  ← pinned GoDaddy Root CA G2 for api.telegram.org (public)
├── fonts/                    ← OFL Liberation Sans TTFs for the button TFT screen (ADR-026)
├── docs/
│   ├── settings-inventory.md    ← runtime-settings inventory + guardrails (the logger/settings.h contract)
│   ├── telegram-message-ia.md   ← Telegram message info-architecture + HTML spec (ADR-017)
│   └── img/                     ← illustrative Telegram mockups (SVG) used by README.md
├── logger/                   ← C++ headers, pulled in via esphome: includes:
│   ├── helpers.h             ← alert enum (8 states), per-sensor thresholds + predictor constants, alert_state(), zone_emoji + zone_color (TFT, ADR-026)/html_escape
│   ├── axp192.h              ← bit-banged software I²C on G21/G22 + axp192_init() + axp192_batt_voltage() (ADR-023)
│   ├── ring_buffer.h         ← RTC_DATA_ATTR sample ring + wake_count + predictor/realert RTC state + latch-arm helpers
│   ├── predictor.h           ← ema(), t_to_threshold(), predictor firing rule (ADR-013)
│   ├── settings.h            ← RuntimeSettings, NVS load/store, /set* parser + guardrails, apply_command()
│   ├── realert.h             ← in-zone CRIT re-alert: time reminder + worsening escalation (ADR-019)
│   ├── commands.h            ← Telegram getUpdates poll: parse + chat_id auth + dispatch (ADR-016)
│   ├── report.h              ← Telegram HTML body builders + DEVICE_LABEL/with_device_tag() (ADR-017/024)
│   └── build_number.h        ← FW_VERSION string; AUTO-GENERATED by scripts/stamp_version.py (do not edit)
├── scripts/
│   ├── stamp_version.py      ← stamps FW_VERSION into build_number.h from `git describe` (run by a PreToolUse hook before compile)
│   └── capture_boot.py       ← serial-capture bring-up aid (RTS reset + log from t=0)
└── test/                     ← host unit tests (no hardware); run via test/run.sh
    ├── run.sh                ← host-compiles + runs all tests (c++17)
    ├── test_ring_buffer.cpp  ← ring buffer + latch/hold-off semantics
    ├── test_predictor.cpp    ← predictor firing rule + t_to_threshold
    ├── test_commands.cpp     ← getUpdates parse + chat_id auth + dispatch
    ├── test_alert_settings.cpp ← runtime threshold overrides / guardrails
    ├── test_realert.cpp      ← reminder + escalation decisions
    ├── test_report.cpp       ← HTML tag balance, device label, verdict/fault
    └── test_axp192.cpp       ← software-I²C protocol against a simulated AXP192
```

---

## Conventions

- Temperatures in °C. Report to 1 dp; keep 2 dp internally for min/max.
- All GPIO references use the `GPIONN` form, not bare integers.
- Secrets (`wifi_*`, `telegram_bot_token`, `telegram_chat_id`) live only in
  `secrets.yaml` (template: `secrets.yaml.example`).
- `temp_a` = box/primary-safety, `temp_b` = fridge/advisory — never swap (see
  Hardware → Sensors). Alert state is the 8-state `AlertState` enum in `helpers.h`
  (see Thresholds & alerts).
- No display, colour, SD, or navigation model (ADR-002); the LED stays dark in
  normal operation. Do not port display/refresh logic from the Tadorna siblings.

---

## Known issues and gotchas

| Problem | Root cause | Fix |
|---------|-----------|-----|
| **Device won't boot with the ENV HAT fitted** | G0 is a strapping pin and the HAT uses it as SDA; something holding it LOW at reset forces the wrong boot mode | Unplug the HAT to confirm; check its pull-ups. *Fallback:* rewire the HAT's sensors onto `bus_box` (0x5C/0x44 don't collide) and drop the G0/G26 bus |
| `temp_a` missing / `bus_box` scan empty | Grove 5 V absent — the AXP192's EXTEN bit gates that boost | `axp192_init()` forces EXTEN every wake (see Pin assignments). *Fallback:* power the ENV II Unit from the HAT header's 3V3 pin |
| `temp_b` erratic / drifting high | DHT12 in condensing fridge air, and/or HAT self-heating | Add an offset `filter:`; if unreliable, switch `temp_b` to the HAT's BMP280 @0x76 via `platform: bmp280_i2c` (YAML-only) |
| Logger dies / resets randomly | Onboard cell depleted (sole supply, ~95 mAh) | Recharge over USB-C; heed the low-battery warning and treat it as urgent |
| Report never sends | Wi-Fi not enabled (`enable_on_boot:false`) and `wifi.enable` not called on the report wake | Ensure the report/alert path calls `wifi.enable` and waits for connect before send |
| Intermittent TCP send failures (`ESP_ERR_HTTP_CONNECT` / `sock < 0`) | Router-side, not the device — home router was band-steering 2.4/5 GHz under one SSID | Use a dedicated 2.4 GHz SSID (resolved — see Open items). Keep the pinned-CA setup |
| Buffer empty every report | Full power loss (cell depleted/unplugged) wiped RTC memory | Recharge before the cell dies — the low-battery warning + floor exist to prevent this; otherwise accept digest gaps |
| Sensors on the wrong bus / one never appears | Each device must name its `i2c_id` | `bus_box` = G32/G33 (0x44), `bus_fridge` = G0/G26 (0x5C). The AXP192 (0x34) is bit-banged and will **not** appear in any scan (ADR-023) |
| **"BOX SENSOR FAULT" / `temp_a` NaN or frozen in the ring** | `sht3xd` publishes asynchronously (from a `set_timeout(50ms)` callback), and deep sleep wipes RAM, so `temp_a.state` is NaN at the start of each wake — a read sooner than ~50 ms samples NaN into the ring *and* the box CRIT check | `wake_cycle` and the boot script `wait_until !std::isnan(id(temp_a).state)` (1 s timeout) before reading. Do **not** replace with a blind delay — a genuinely dead sensor still times out to NaN, which the fault detector latches |
| **Config fails: "maximum number of i2c interfaces for ESP32 is 2"** | A third `i2c:` bus was declared | Keep only `bus_box` + `bus_fridge`; the PMIC is bit-banged in `logger/axp192.h` (ADR-023) |
| **Battery reads 0.00 V or "n/a"** | AXP192 ADCs are disabled at power-on; ESPHome never calls `Axp.begin()` | `axp192_init()` writes reg 0x82 every wake; confirm it runs before `component.update: batt_v` |
| Device boot-loops after flash | A `psram:` block — the PICO-D4 has none | Remove it entirely |
| Device won't re-sleep after report | `deep_sleep.prevent` left set / no completion path | `deep_sleep.enter` on `on_response` **and** on a send timeout |
| Flash wears out over time | Ring buffer moved to `globals restore_value: true` (NVS) | Keep the buffer in RTC memory (`RTC_DATA_ATTR`), not NVS |
| **Box reads NaN once in a while (`temp_a`), no pattern** | A single failed I²C transaction on `bus_box`. `sht3xd` makes ONE attempt per `update()` and returns without publishing on failure | `wake_cycle` retries once (3 s window) when `temp_a` is NaN **and** the component is not `is_failed()`. Root cause still unidentified — see Open items |
| **Device boot-loops with the box sensor unplugged** | Retrying `component.update:` on a component ESPHome marked FAILED at setup. A failed component never gets `update()` called, so the retry spins the main loop into a watchdog reset — and every cold boot sends a Telegram boot message | The retry is guarded by `!id(sht30_hub)->is_failed()`. **Never retry a sensor read without that guard** |
| **Every message ends in `diag 0x12=... EXTEN=...`** | `DIAG_ENABLED` is true in `helpers.h` | Set it false (the release default). It gates the footer *and* the ERROR-level serial probes together |
| **`temp_a` silently stale — report shows a reading minutes old** | `wait_until !isnan(temp_a)` is vacuous when `temp_a` already holds a value: it passes instantly and the deferred `sht3xd` publish never lands before the blocking send | `id(temp_a).publish_state(NAN);` before every `component.update: sht30_hub`, forcing the guard to bind |
| **Serial attach destroys the state you wanted to inspect** | Opening the port resets the ESP32 on this hardware, even with `HUPCL` cleared — measured, bootloader output at t=0.02 s | Do not attach to a hung/faulted device. Read the RTC counters via the Telegram diag footer instead (`scripts/attach_serial.py` documents this) |
| Predictor fires nuisance alerts during compressor cycles | `PREDICT_SMOOTH_N` too small — the fridge EMA follows the cycle | Increase `PREDICT_SMOOTH_N` (default 4 = 60 min) |
| Predictor never fires, or fires far too early | `TAU_BOX_MIN` mis-set (still the placeholder, or wrong after a box change), or `PREDICT_HORIZON` off | Re-run the τ_box fit and pin `TAU_BOX_MIN`; sanity-check `PREDICT_HORIZON` against τ |

---

## Architecture decisions (ADRs)

> Active decisions are condensed to *decision + the rationale that still governs
> behaviour + a cross-reference*. Superseded/skipped numbers are one-line stubs;
> their full text is recoverable from git history. **Every `#NN` issue reference
> elsewhere in this document cites the `telefridge_V1` tracker, not this repo.**

### ADR-001 — Framework: esp-idf — *in force*
esp-idf, not Arduino: needed for reliable deep-sleep control, RTC-memory handling,
and parity with the sibling builds.

### ADR-002 — Headless: no display, no SD — *in force, display half amended by ADR-026*
No SD card, and no *operational* UI: state is an RTC-memory digest reported over
Telegram, with no local file log and no always-on screen. The blanket "display
unused" invariant is **narrowed by ADR-026** — the onboard TFT now shows a brief
status screen on a button press (3 s), but stays dark on every unattended wake —
sampling, report and cold boot — and in deep sleep, so the
headless-in-normal-operation and power premises hold.

### ADR-003 — Sensors SHT40 + STTS22H (STEMMA QT) — *superseded by ADR-021.* See git history.

### ADR-004 — I²C power gating via GPIO2 in sleep — *moot under ADR-020* (no switched rail). See git history.

### ADR-005 — Storage: RTC-memory ring buffer — *in force*
Between-report samples live in `RTC_DATA_ATTR`, not NVS, to avoid flash wear from
frequent small writes; accepts loss on a full power cut. NVS or an external sink
is a future ADR if gap-proof history is required. (Detail: RTC ring buffer
section.)

### ADR-006 — Wi-Fi only on report/alert wakes — *in force*
`wifi: enable_on_boot: false`; radio enabled explicitly every 4 h (or on CRIT).
Sampling wakes keep the radio off — the core power decision.

### ADR-007 — Telegram via http_request — *in force*
No native Telegram component; report via HTTPS POST to the Bot API `sendMessage`.
Token/chat id in `secrets.yaml`.

### ADR-008 — Alerts: WARN batched, CRIT early — *in force*
WARN transitions ride the scheduled 4-h report; CRIT entry forces an early Wi-Fi
alert with a hold-off. (Full routing: Alert reporting policy.)

### ADR-009 — Power source USB bank — *superseded by ADR-010, then ADR-022.* See git history.

### ADR-010 — Power source JST single-cell Li-ion — *superseded by ADR-022.* See git history.

### ADR-011 — Sensor roles: box = primary safety, fridge = leading indicator — *in force*
Because the payload lives in an insulated box inside the fridge, the two sensors
measure different things: `temp_a` (box interior) is the pharma-compliance
authority (2–8 °C); `temp_b` (fridge air) is a leading indicator whose value is
its *relationship* to the box, not its absolute reading. Consequences: per-sensor
threshold sets; fridge absolute CRIT advisory **in authority** (it never
establishes compliance) though it still alerts early, and it neither reminds nor
escalates — only the box does (ADR-019); the early-warning
alert derived from both sensors (ADR-013). Placement is locked; swapping them is
a compliance defect. The *parts* were later replaced (ADR-021) but the roles and
the `temp_a`/`temp_b` contract are unchanged. All predictor math uses
`T_box = temp_a`, `T_fridge = temp_b`.

### ADR-012 — never used on `main`
The number was skipped when the asymmetric-payload design merged. Its substance —
runtime settings persisted in NVS — landed without a number (`logger/settings.h`,
`docs/settings-inventory.md`), with `constexpr` defaults in `helpers.h` as the
fallback and the getUpdates cursor as the NVS-backed `RuntimeSettings.update_offset`
(ADR-016). The rationale worth keeping: **settings go in NVS, not the RTC ring
(ADR-005)**, because they change rarely (no flash-wear concern) and must survive a
full power loss — the exact event that wipes RTC memory.

### ADR-013 — Predictive alert (Newton's law of cooling) — *in force (implemented)*
A physics-based leading-indicator alert: from the current fridge/box gradient,
predict when the box (`temp_a`) will cross a box CRIT bound. **One mechanism
covers the canonical failure modes** — fast failures shrink `t` toward zero;
door-open transients recover next sample and are eaten by the debounce; slow
drifts shrink `t` monotonically over hours and fire well before CRIT — behind one
user-legible knob (`PREDICT_HORIZON`). Cost: introduces `τ_box` as a calibrated
quantity (V1 = one-off commissioning fit). Implemented and host-tested; output
unvalidated until `TAU_BOX_MIN` is calibrated. Firing rule + constants:
Thresholds & alerts.

### ADR-014 — Approach trigger — *design only, not implemented*
Covers the gap between the predictor (needs a gradient) and the 4-h report: when
`temp_a` sits within `APPROACH_MARGIN_C` (0.5 °C) of a box CRIT bound with no
gradient, bring the scheduled report forward (`EARLY:` prefix, reset cadence)
rather than raise an alert. Box-only; box CRIT and predicted breach take
precedence.

### ADR-015 — Alert debounce (N consecutive out-of-range samples) is permitted — *design only*
Permits an optional debounce: honour an absolute WARN/CRIT only after **N
consecutive** out-of-range samples on the same side, N runtime-tunable. **`N=1` is
the shipped default and reproduces today's behaviour exactly.** The documented fix
for compressor-cycle chatter; keep N low on the box-CRIT path (food safety), the
knob's main use is the fridge advisory band. See `docs/settings-inventory.md`.

### ADR-016 — Remote settings via Telegram command polling (getUpdates) — *in force (implemented, validated on HW)*
Runtime settings are changeable over the same Telegram chat the device already
reports to: at the end of each Wi-Fi-up wake (after the outbound send) the device
issues one `getUpdates` GET, parses pending `/set…` lines, and routes them through
the same `apply_command()` write path (`logger/commands.h`). Rules that constrain
behaviour: **auth is mandatory** — updates whose `chat.id` ≠ the configured
`telegram_chat_id` are ignored but still consumed (offset-advanced) so they can't
re-deliver; **the cursor lives in NVS** (`RuntimeSettings.update_offset`),
persisted before `deep_sleep.enter`, so a power cycle can't replay old commands;
**latency = the next radio wake** (up to the 4-h cadence, or near-immediate as a
reply to a CRIT alert), so there is no `/report_now`.

### ADR-017 — Telegram messages: HTML + four-tier hierarchy — *in force (implemented)*
Message **bodies** (not the LOCKED transport) use **`parse_mode=HTML`** with a
verdict → evidence → context → housekeeping hierarchy. The motivating defect: the
old flat report rendered a **faulted primary sensor as Zone "OK"** because
`build_report()` never consulted `sensor_faulted()`. Two rules that bite: bodies
compose with `std::string` (never a fixed `snprintf` buffer that could truncate a
tag and 400 the send), and **fault-awareness lives in the builder, not in
`alert_state()`** (which stays pure and shared with the alert-firing path). Full
spec: Telegram report section + `docs/telegram-message-ia.md`.

### ADR-018 — Alert hold-offs advance on send-acknowledgement, not state entry — *in force (implemented)*
Every early-alert latch (box-CRIT, predicted breach, sensor-fault, low-battery) is
armed **only inside the send script's `on_response` 2xx branch**, never at
dispatch. **Rationale (safety):** arming at dispatch created a silent-failure hole
— a send that fails mid-emergency (Wi-Fi drop, TLS/HTTP timeout) would still start
the quiet window and suppress the next attempt while the payload is at risk. Under
this rule a failed send leaves the latch untouched, so the next wake re-evaluates
and retries. A shared `g_last_send_epoch` (`mark_send_ok()`, set on every confirmed
2xx) surfaces as a `last send` line in the report so a run of missed sends is
visible. The dispatch sets only the RAM-only `g_wake_handled` double-dispatch
guard. Arming helpers live in `logger/ring_buffer.h`.

### ADR-019 — Re-alert while still in CRIT: reminder + worsening escalation — *in force (implemented)*
Once a CRIT alert has fired, the box speaks again for one of two reasons
(`logger/realert.h`): a **time reminder** every `crit_realert_holdoff_min` minutes
(**`0` = off = the single-shot default**, measured in *wakes* so it works without
SNTP), or a **worsening escalation** — speak now when the box worsens by
≥ `ESCALATE_DELTA_C` (1.5 °C) beyond the last-alerted value (predicted breach: ETA
collapsing to ≤ half). Escalation is always on but bounded by
`ESCALATE_MAX_PER_HOUR` (3) so it can't itself become chatter, and out-ranks the
reminder on the same wake. Both anchor on the **box** (`temp_a`); the fridge
advisory does not remind/escalate. Send-ack discipline per ADR-018 (a reminder
clock must never start on a failed send). On-hardware worsening test pending.

### ADR-020 — Board: M5StickC — *in force*
The target board is the **M5StickC** (ESP32-PICO-D4); the Adafruit Feather V2 is
retired. Principle: **one config, one board** — now enforced by the repo boundary
(ADR-025) rather than by overwriting a shared file. Consequences: **exactly two
I²C buses**, not the three this ADR originally specified (the ESP32 has only two
peripherals — see ADR-023); **no `psram:` block** (boot-loops otherwise); **no
switched sensor rail** (makes ADR-004 moot); wake button is **GPIO37** (ext0,
`inverted: true`); 4 MB flash fits the esp-idf + TLS build (52 %). The port kept
`temp_a`/`temp_b`/`batt_v`/`batt_pct` ids and meanings, so no `logger/` file
changed — worth preserving on any future board move.

### ADR-021 — Sensors: M5 ENV II Unit (SHT30, box) + ENV HAT (DHT12, fridge) — *in force, supersedes ADR-003*
`temp_a` = SHT30 (ENV II Unit, 0x44, Grove/`bus_box`); `temp_b` = DHT12 (ENV HAT,
0x5C, HAT/`bus_fridge`). Both M5-native, so they mate directly with the StickC's
own connectors. The box/fridge role contract, ring buffer, predictor and report
format are unchanged — only the two sensor platforms move (`sht3xd`, `dht12`).
**Why the role split is forced, not a preference:** the DHT12 (±0.5 °C) can't hold
the pharma-compliance role given the 1.0 °C-wide box WARN shoulder, so the ±0.2 °C
SHT30 takes `temp_a` — swapping them is a compliance defect (see Hardware →
Sensors). DHT-family parts are unreliable in condensing air (the fridge-ambient
position); documented fallback is the same HAT's **BMP280 @0x76**
(`platform: bmp280_i2c`, YAML-only). Retiring the STTS22H also removed its
cold-wake poll-until-valid pattern.

### ADR-022 — Power: onboard cell + AXP192 telemetry — *in force, supersedes ADR-010*
The supply is the StickC's **onboard ~95 mAh single-cell Li-ion**, charged by the
AXP192 over USB-C, with battery telemetry from the AXP192's own 12-bit ADC (regs
0x78/0x79). **Runtime regresses hard — days, not months — and that is accepted**
(capacity fell ~3000 → ~95 mAh and the AXP192 adds continuous quiescent draw).
`li_ion_soc()` and the `batt_v`/`batt_pct` ids are unchanged from the JST design.
Operational detail (thresholds, `axp192_init` duties, NaN-not-0) lives in Power
architecture; the AXP192 is bit-banged (ADR-023).

### ADR-023 — AXP192 on bit-banged software I²C — *in force (validated on HW)*
The AXP192 is **not** an ESPHome `i2c:` device — `logger/axp192.h` drives G21/G22
as open-drain GPIOs with its own software I²C master. **Hard constraint, not a
preference:** the ESP32 has exactly two I²C peripherals (ESPHome enforces it), so
the AXP192 *must* come off hardware I²C — and it's the one device that can (read
once per wake, low speed, no timing constraint, no other master). Two
safety-relevant points: any wire-level failure returns **NaN, never 0** (a 0 would
trip the `BATT_CRIT_V` floor); and `axp192_rails_plausible()` rejects any reg-0x12
read with DC-DC1 (the ESP32's own 3.3 V rail) clear, since this code executing
proves that rail is on. Host-tested against a simulated AXP192
(`test/test_axp192.cpp`). Validated on hardware: first wake reported 4.08 V / 94 %
and the LCD stays dark (the 0x12 rail write lands).

### ADR-024 — Every message labelled with the device — *in force*
Every outbound message begins with `DEVICE_LABEL = "Stick C"`, because a sibling
board posts to the same chat and an unlabelled CRIT alert is ambiguous exactly
when ambiguity is most expensive. **The label is applied by `with_device_tag()` at
the send site, not inside builders** — several messages (the CRIT body especially)
are composed from more than one builder, so an in-builder tag would double up or
land mid-message. The plain-text command reply takes the untagged form
(`html=false`).

### ADR-025 — The StickC build lives in its own repo — *in force*
Split out of `telefridge_V1` into its own repo — working name
`telefridge_StickC_V1`, published on GitHub as `Telegram_Fridge_V1` — so each
board's LOCKED invariants and ADR history stay independent (no Feather-era
assumption can leak in). Two consequences a reader needs: **history starts fresh**,
so every `#NN` issue reference throughout this document cites the `telefridge_V1`
tracker, not this repo (treat them as citations into that history); and the
**going-public prerequisites are met** — a GPLv3 `LICENSE` and a liability
`DISCLAIMER.md` are in the tree, which matter here because this monitors
insulin-class refrigerated storage. Secrets were verified never committed in the
source history.

### ADR-026 — Onboard TFT: a 3 s status screen on a button press, and nothing else — *in force, narrows the display half of ADR-002*
The M5StickC's 0.96" ST7735S TFT, previously held dark, now shows a **3 s status
screen on a Button A press** (the button still fires its on-demand Telegram
report; the screen is visual confirmation the press registered). Content is
**verdict-led** — the box-safety verdict from `temp_a` (`alert_state_live` →
`alert_label`, fault-aware per ADR-017: a faulted primary sensor shows **FAULT**,
never a fabricated OK), then both temps and battery.

**A deliberate press is the whole trigger condition, and that is the decision.**
An earlier revision also drew a 5 s screen on cold boot; it was dropped. The
screen's only value is that a person is standing in front of the device *right
now* — which a button press proves and no other wake does. Power-on, the 15-min
sampling wakes and the 4-h report wake are all unattended by definition, so a
screen there is light nobody sees, spent from a 95 mAh cell. Cold boot already
announces itself over Telegram, which reaches the owner wherever they are. **Do
not re-add a screen to any unattended wake** without a new ADR arguing why
someone would be watching.

**The panel can't render the Telegram `zone_emoji()` glyphs, so colour carries the
zone instead — via `zone_color()`, which lives beside `zone_emoji()` in
`helpers.h` and returns a packed `0xRRGGBB`.** Two renderings of one decision, in
one place, because they demonstrably drift when kept apart: the screens were first
written against the pre-issue-#2 collapsed traffic-light scale and went on painting
a sub-2 °C box red after the Telegram side had gone directional. `zone_color()`
returns a plain integer rather than ESPHome's `Color` so `helpers.h` stays free of
framework types and host-testable; `test_report.cpp` asserts the two tables agree
zone-for-zone, so the next divergence fails a test rather than shipping.

**Why this doesn't break ADR-002's premises:** the panel is powered (AXP192
LDO2/LDO3) *only* on a button wake and turned back off as soon as the 3 s screen
ends, so **every unattended wake and all of deep sleep stay dark**. The residual
cost is not zero and should not be described as such: the display *component*
still initialises on every wake, **measured at 901 ms on hardware**, ~96 times a
day, against a panel that is unpowered. Removing that means dropping the ESPHome
`display:` component and driving the ST7735 directly — a new ADR, not a tweak.

**Power-model inversion (the load-bearing change):** "display off" is no longer
enforced at boot by `axp192_init()` (it now leaves the TFT rails as found and only
forces EXTEN). Instead `axp192_lcd_on()` lights the rails from a **priority-900
`on_boot` lambda** — above the display component's priority-400 ST7735 init, so
the panel is powered before init and never rail-bounced between init and the first
draw — and `axp192_lcd_off()` is enforced from an **`esphome: on_shutdown` hook**,
which `DeepSleepComponent::begin_sleep()` fires (via `App.run_safe_shutdown_hooks()`)
at every one of the six `deep_sleep.enter` sites. **That single choke point is
safety-relevant, not tidiness:** since `axp192_init()` no longer clears the rails,
a reset landing between `axp192_lcd_on()` and a screen's own off would otherwise
leave the panel lit through deep sleep on a 95 mAh cell, with nothing to clear it.
For the same reason the wake is classified **once** — `g_button_wake` is set in the
priority-900 lambda and read by both the rail-on decision and the ack screen,
rather than each deriving the wake cause independently.

Rail arithmetic (`axp192_lcd_on_target`/`axp192_lcd_off_target`) is host-tested in
`test/test_axp192.cpp`; the ST7735/SPI/font wiring is ESPHome-only and **still
needs on-hardware verification** (panel offsets, colour order, and the dark-on-
sampling-wake invariant — see Open items). New assets: `spi:`/`display:`/`font:` in
`telefridge.yaml` and an OFL font under `fonts/`.

---

## Open items to finalise on the physical build

### Box sensor NaN — OPEN, root cause unidentified (v0.9.1)

`temp_a` intermittently reads NaN. Two consecutive NaN wakes latch
`BOX SENSOR FAULT — no reading`, which is what the owner sees. **Not solved —
mitigated.** What the evidence rules in and out:

- **Not the battery / 5 V boost.** An overnight fridge run on the cell drained
  to 3.32 V / 5% with `EXTEN=1` and **zero** NaN wakes. The only NaN ever
  captured happened on *external* power, at room temperature.
- **Not the sensor or the cable.** Both were replaced; it recurred.
- **Not a scheduling race.** The `wait_until` guard gives the deferred publish a
  full second, and on a real timer wake `temp_a` starts NaN so the guard binds.
- **Working theory:** a single failed I²C transaction on `bus_box`. `sht3xd`
  makes exactly one attempt per `update()` and returns without publishing, so
  one NACK costs the whole sample.
- **Mitigation shipped:** one guarded retry (3 s) in `wake_cycle`. It has
  **never fired in the wild** — the failure is rare (one occurrence in ~24 h;
  zero in ~100 overnight wakes).

**To close this:** set `DIAG_ENABLED = true` and watch the footer. `retries=`
climbing with `nanwakes=0` means the retry is catching real transient failures
and the mitigation works. `nanwakes=` climbing alongside means they are not
transient and the theory is wrong. `atnan[...]` reports the rail state latched
at the first NaN wake.

### Awake watchdog — a backstop, not a fix

A 120 s ceiling forces `deep_sleep.enter` however a wake is stuck (`wdt=` in the
diag footer counts firings). Verified on hardware by shortening it to 15 s: it
fired mid-wake over a held `deep_sleep.prevent`. **It does not bound a boot
loop** — each reset restarts the timer. Non-zero `wdt=` means a real hang exists
and this is masking it: find the branch, do not raise the timeout.

### Battery runtime is ~half a day, not "days" (ADR-022 needs revising)

Measured overnight at the 15-min cadence: roughly **9 percentage points/hour**,
4.08 V → 3.32 V / 5% in about 13 hours. ADR-022's "days, not months" is
optimistic by an order of magnitude. First suspect is the display component's
**901 ms setup on every wake** against an unpowered panel (ADR-026 item 6).


Single source of truth for implementation + bring-up status.

**Implemented + wired:** the asymmetric-payload design, the **predictive alert**
(`logger/predictor.h`), and the **remote settings channel** (getUpdates polling,
ADR-016) — the last validated on hardware 2026-07-27. **Design only:** the
**approach trigger** (ADR-014). The predictor's numbers remain **unvalidated** —
it ships a placeholder `TAU_BOX_MIN` (`helpers.h`) and box-thermal calibration was
not pursued for V1; treat its ETAs as advisory until τ is fitted (ADR-013).

The original bring-up list is fully closed (below). **ADR-026 opens a second
list:** the TFT screen is host-tested, compiles clean, and has now been **flashed
and partly validated on hardware (2026-08-05)** — see *TFT bring-up* immediately
below. Everything else in the lists is the record of what was validated and what
was decided out-of-scope.

### TFT bring-up — PARTLY VALIDATED (ADR-026)

Flash over **USB, not OTA** (a 6 s button wake can't carry a ~1 MB image). Item 3
is the load-bearing one: it is the invariant the whole power argument rests on.

- [x] **1. Screen renders correctly (2026-08-05)** — right orientation, image not
      shifted, black background, nothing clipped. `col_start: 26` / `row_start: 1`
      were an educated guess and are **correct on this unit**; the `24`/`0`
      alternative and the `use_bgr`/`invert_colors` knobs were not needed.
- [x] **2. Button A → 3 s screen → panel dark → the send still happens
      (2026-08-05)** — confirmed four times, twice before the button-only change
      and twice after. Log shows `wakeup_cause=2 button=1`, a 3.05–3.24 s gap
      between the sensor read and `wifi.enable` (the screen holding), then a
      delivered Telegram report and a clean sleep.
- [ ] **3. An unattended wake NEVER lights the panel.** The 15-min sampling wake
      is the case to watch (`/set sample_interval_min 1` compresses it); the 4-h
      report wake and cold boot take the same path. Cold boot is now easy to check
      directly — power-cycle and confirm the panel stays black.
- [x] **4. Recovers from an interrupted screen (2026-08-05)** — RESET pressed
      during the live 3 s screen; the panel ended up dark. This is the failure the
      `on_shutdown` hook exists for, and the one that would otherwise flatten the
      cell: the AXP192 is a separate chip, so an ESP32 reset leaves LDO2/LDO3
      exactly as the interrupted screen left them — ON — and `axp192_init()` no
      longer clears them. Nothing recovers it until the post-reset wake reaches a
      `deep_sleep.enter` and the hook fires. **Expect the backlight to stay lit for
      the whole post-reset wake (~40 s if it sends), then go out** — the display
      component's own reset blanks the *image* at ~priority 400 but does not remove
      rail power, so an immediately-black panel is the image clearing, not the
      rails dropping. Worth re-checking with that distinction in mind if the power
      budget ever looks wrong.
- [ ] **5. Screen and Telegram agree on a real zone** — chill the box below 2 °C:
      the panel must show **blue** `BOX TOO COLD` while the message shows 🟦. The
      host test pins the tables to each other; only hardware pins them to reality.
- [ ] **6. Average-current comparison, before vs after.** Partly answered: the
      display component's setup is **901 ms on every wake** (measured 2026-08-05),
      against an unpowered panel, ~96 times a day. That is the real residual cost
      and it is not zero. What is still missing is what it costs in mA·h — measure
      and fold into ADR-022, then decide whether it justifies dropping the ESPHome
      `display:` component for direct ST7735 control.
- [ ] **7. `AXP192_LDO23_3V0` (reg 0x28 = `0xCC`, 3.0 V both rails)** is the one
      unvalidated register write in the change — confirm the panel is legible and
      the backlight isn't over-driven.

### M5StickC bring-up

**Passed on hardware (2026-08-04):**
- [x] **1. Clean boot with the ENV HAT fitted** — boots normally across many flash
      cycles; G0's strapping-pin risk did not materialise (I²C idles high). The
      rewire-onto-`bus_box` fallback is not needed.
- [x] **2. GPIO37 does not float** — pulled up; the 15-min sleep holds untouched,
      so the floating-ext0 power-budget hazard does not apply on this board.
- [x] **3. Grove 5 V live on battery, not just USB** — confirmed; `bus_box`
      powers the ENV II Unit (SHT30) off the cell, so the HAT-3V3 fallback is not
      needed.
- [x] **4. `batt_v` plausible on the FIRST wake** — 4.08 V / 94 %, no added delay
      needed. Also validates the bit-banged I²C master (ADR-023) on hardware.
- [x] **5. Build fits 4 MB flash** — Flash 52.3 %, RAM 13.7 %. (This compile also
      exposed the ADR-020 three-bus defect — see ADR-023.)
- [x] **6. Boot-log sanity** — `bus_box` → 0x44 (+ unconfigured 0x76);
      `bus_fridge` → 0x5C (+ unconfigured 0x76/0x10); `temp_a`/`temp_b` plausible
      and distinct. The AXP192 (0x34) does **not** appear in any scan (bit-banged).
- [x] **7. Button A from deep sleep** — a press gives `wakeup_cause=2 button=1`;
      the on-demand path runs to completion (Wi-Fi up ~4 s, send, poll, sleep in
      ~6 s total).

A full wake → sample → alert → send → sleep cycle has run end-to-end on the device
(two Telegram messages delivered). Nothing has been near a fridge yet.

All bring-up checks are now closed; the remaining fridge-tuning and current-draw
characterisations were reviewed and dropped as out-of-scope for V1 (git history
retains their detail). The predictor stays implemented but **uncalibrated** — see
the caveat at the top of this section and ADR-013.

**Done (for the record):**
- [x] **Fridge (advisory, `temp_b`) thresholds accepted as-is.** The current
      advisory bands are deemed adequate; further per-appliance tuning is not
      pursued for V1. The box (`temp_a`) CRIT bounds (2–8 °C) remain pharma-spec
      **LOCKED** regardless — do not widen.
- [x] **Telegram command channel validated on hardware (ADR-016, 2026-07-27).**
      `/status` → getUpdates 200 → reply delivered; `/setreport 8` → applied + NVS
      persist; cursor advances per command and survived a reflash. Poll added
      ~1.8 s to a ~12.7 s report wake. Remaining low-priority: live
      unauthorised-chat rejection (host-tested); offset survival across a full
      power cycle.
- [x] **HTTPS/CA setup confirmed.** `verify_ssl: true` + pinned GoDaddy Root CA G2
      + SNTP wait; HTTP 200 from sendMessage on hardware. (mbedTLS here has
      `MBEDTLS_HAVE_TIME_DATE` off, so cert dates aren't checked — the SNTP wait
      mainly gives real epochs to the ring buffer.)
- [x] **Intermittent send failures resolved (was issue #24).** Root cause was
      router-side band-steering under one SSID; a dedicated 2.4 GHz SSID fixed it,
      confirmed across boot + stress + report cycles with zero TCP errors.
- [x] **PSRAM moot** — the PICO-D4 has none; the `psram:` block is removed (ADR-020).
