# CLAUDE.md — TeleFridge V1

> **Read this file in full at the start of every session.**
> It is the single source of truth for this project.
> Sections marked **LOCKED** must not be changed without an explicit ADR.

> ⚠️ **Licence & disclaimer.** Licensed under **GPL-3.0-or-later** (see
> [`LICENSE`](./LICENSE)). This is a hobbyist project provided **"as is", with NO
> WARRANTY** — it is **not a medical device** and must **not** be used as the
> sole safeguard for insulin, vaccines, or any temperature-sensitive payload.
> See [`DISCLAIMER.md`](./DISCLAIMER.md) before use.

---

## Project overview

**TeleFridge V1** is an ESPHome-based low-power, headless **refrigerated-payload
logger** built on the **M5StickC** (ESP32-PICO-D4). It monitors an **insulated
box placed inside a domestic fridge** — the box holds a temperature-sensitive
payload (a pharmaceutical, insulin-class, spec range **2–8 °C**), and the fridge
is the outer cooling environment. Two I²C sensors, **on two separate buses**,
sample on a fixed interval:

- **M5 ENV II Unit — SHT30** (0x44, Grove) → **`temp_a`**, mounted **inside the
  insulated box**. This is the **primary safety sensor**: its reading is what the
  payload actually sees, and it is the authority for pharma-spec (2–8 °C)
  compliance. It is also the accurate part (±0.2 °C), which is why it holds this
  role (ADR-021).
- **M5 ENV HAT — DHT12** (0x5C, HAT header) → **`temp_b`**, in the **fridge air,
  outside the box**. This is the **leading-indicator sensor**: it detects
  fridge/appliance failure long before the box thermal mass catches up. Advisory
  only, and ±0.5 °C — it must never take the compliance role.

Readings are kept in an RTC-memory ring buffer while the device sleeps, and the
device **wakes Wi-Fi once every 4 hours to push a short report to a Telegram
chat**. The device spends the vast majority of its life in deep sleep. The
StickC's TFT is **deliberately unused** and there is **no SD card**.

It is a sibling to **Tadorna** (colour TFT, Adafruit Feather) and **Tadorna
e-Ink** (Waveshare ESP32-S3 e-paper). It is a separate repository with its own
locked invariants. Do **not** merge assumptions from the display builds into
this one — this project has no display, no colour, no screen inventory, no
navigation model, and no display-refresh logic of any kind.

**ESPHome version:** 2026.4.5
**Repo name:** telefridge_StickC_V1 (private; split from `telefridge_V1` on
2026-08-04 — see ADR-025)
**Device name (ESPHome):** telefridge

### What this project is NOT
- Not a real-time monitor — readings are batched and reported every 4 h.
- Not display-driven — remove/ignore any `display:`, screen, or refresh concept.
- Not mains/always-on — it is optimised for the lowest possible average current.

---

## Hardware

### Board

**M5StickC — ESP32-PICO-D4** (ADR-020)

- MCU: ESP32-PICO-D4 (dual-core Xtensa LX6), 4 MB Flash, **no PSRAM**. Declaring
  a `psram:` block boot-loops the device.
- USB-C; onboard 0.96" TFT (**unused** — headless, ADR-002; its rails are held
  off via the AXP192 to save power).
- **AXP192 PMIC** (I²C 0x34 on the internal bus) — LiPo charger, rail control,
  and the **only** source of battery telemetry on this board. There is no ADC
  divider and no fuel gauge.
- Onboard **~95 mAh** Li-ion cell, the sole supply (ADR-022).
- **Grove** HY2.0-4P port (G32/G33) and an 8-pin **HAT** header (G0/G26/G36/BAT/
  3V3/5V/GND) — two independent I²C-capable pin pairs, which is what lets this
  build run two sensors that would otherwise collide.
- Buttons A (G37) and B (G39), both RTC-capable input-only pins.
- Official docs: https://docs.m5stack.com/en/core/m5stickc

> **Retired board:** the Adafruit ESP32 Feather V2 (product 5400) was the V1
> target through 2026-08. Its GPIO2 STEMMA QT power rail, GPIO35/A13 battery
> divider and GPIO38 button are all gone. See ADR-020/021/022 for what replaced
> them, and the git history for the validated Feather configuration.

### Sensors

| Sensor   | Bus (pins)          | Addr | Role                                   |
|----------|---------------------|------|----------------------------------------|
| SHT30 (ENV II Unit) | `bus_box` — Grove, G32/G33 | 0x44 | `temp_a` — **box interior** (PRIMARY safety sensor, ±0.2 °C) |
| DHT12 (ENV HAT)     | `bus_fridge` — HAT, G0/G26 | 0x5C | `temp_b` — **fridge ambient** (advisory leading indicator, ±0.5 °C) |
| AXP192 PMIC         | bit-banged — internal, G21/G22 | 0x34 | Battery voltage (regs 0x78/0x79). No divider, no fuel gauge. Software I²C, not an `i2c:` bus (ADR-023). |

Both temperature points are M5 parts on **separate I²C buses**, because they are
on separate physical connectors. Their addresses also happen not to collide
(0x5C vs 0x44), which keeps a rewire-onto-one-bus fallback available if G0 ever
causes trouble. The ENV II Unit's BMP280 (0x76) and the ENV HAT's BMP280 (0x76)
and BMM150 (0x10) are present on the boards but **not configured**.

> **Sensor swap (2026-08-04, ADR-021):** point A is an **M5 ENV II Unit**
> (SHT30, 0x44, Grove) and point B is an **M5 ENV HAT** (DHT12, 0x5C, HAT
> header). This retires the Adafruit SHT40 and the SparkFun STTS22H, and with
> the STTS22H goes its whole cold-wake poll-until-valid read pattern (issue #44)
> — neither replacement has that pathology. Temps-only: the SHT30 and DHT12 both
> measure humidity in the same conversion; add a `humidity:` sub-sensor to
> publish it at no extra I²C cost.
>
> **History:** point A was STTS22H → SHT30/ENV II (2026-07-08) → SHT40
> (2026-07-21) → SHT30/ENV II (2026-08-04); point B was DS18B20 (1-Wire) →
> STTS22H (2026-07-21) → DHT12/ENV HAT (2026-08-04).

**Sensor roles are asymmetric and LOCKED** (see ADR-011). `temp_a` (SHT30) lives
*inside* the insulated box; its reading is what the payload experiences and is
the authority for pharma-spec compliance (2–8 °C). `temp_b` (DHT12) reads the
fridge air *outside* the box; it detects appliance failure early (door left
open, compressor dead) before the box's thermal mass falls behind. Their IDs and
placement (`temp_a` = SHT30 = box interior; `temp_b` = DHT12 = fridge ambient)
must **never** be swapped — the alert logic and the predictor (ADR-013) depend on
knowing which is which. Both are still logged and both are threshold-checked, but
against **different** per-sensor threshold sets (see Thresholds & alerts).

> **On this board the role assignment is also forced by accuracy** (ADR-021).
> The DHT12 is a ±0.5 °C part and the box WARN shoulder is only 1.0 °C wide, so
> the DHT12 **cannot** hold the compliance role. The accurate SHT30 (±0.2 °C) is
> also the one on a cable, i.e. the only one that can physically reach inside a
> sealed box. Swapping them is a compliance defect, not a preference.

> **SHT30 / ENV II Unit:** 0x44 (fixed). Plugs into the Grove port with the
> stock HY2.0-4P cable. In ESPHome use `platform: sht3xd` — a hub component with
> a `temperature:` sub-sensor, so `component.update:` targets the hub id
> (`sht30_hub`), not `temp_a`. Grove VCC is **5 V** on this board (AXP192 boost,
> gated by the EXTEN bit which `axp192_init()` forces on).
> Ref: https://docs.m5stack.com/en/unit/envII

> **DHT12 / ENV HAT:** 0x5C (fixed), on the HAT header's G0/G26. Same hub shape
> in ESPHome (`platform: dht12`, `component.update: dht12_hub`).
>
> **Two standing caveats (ADR-021).** (1) `temp_b` feeds the predictor's
> `T_fridge` term, and `t_to_threshold` is highly error-sensitive as `T_fridge`
> approaches `T_thresh` — a ±0.5 °C input is materially noisier than the STTS22H
> it replaced, so re-check `PREDICT_SMOOTH_N` when `TAU_BOX_MIN` is calibrated.
> (2) DHT-family parts are unreliable in **condensing** air, and fridge ambient
> is exactly that position. If it misbehaves, the same HAT's **BMP280 (0x76) is
> a drop-in `temp_b` source** via `platform: bmp280_i2c` — a YAML-only swap.
> Ref: https://docs.m5stack.com/en/hat/hat-env

> **G0 is a strapping pin.** The ENV HAT wires SDA to it and M5 ships it that
> way — I²C idles high via the pull-ups, which is the normal-boot level. But
> anything holding that line LOW at reset stops the device booting. Verify a
> clean boot with the HAT fitted.

---

## Pin assignments

Against the M5StickC pinout. Items marked **(verify)** must be confirmed on
hardware before the first fridge deployment.

### I²C buses — LOCKED (ADR-020, amended by ADR-023)

**Two hardware buses — which is all the ESP32 has.** The PICO-D4 provides
exactly two I²C peripherals and ESPHome enforces the limit, so the third bus
ADR-020 originally declared is not buildable. Both hardware buses go to the
temperature sensors, which sit on two different physical connectors and
therefore cannot share one; the PMIC is bit-banged instead (ADR-023).

| Bus id       | SDA / SCL   | Devices                          |
|--------------|-------------|----------------------------------|
| `bus_box`    | G32 / G33   | ENV II Unit SHT30 (0x44) → `temp_a` — Grove port |
| `bus_fridge` | G0 / G26    | ENV HAT DHT12 (0x5C) → `temp_b` — HAT header |
| *(bit-banged)* | G21 / G22 | AXP192 PMIC (0x34) → battery — internal, **not an `i2c:` bus** (ADR-023) |

> **G0 is an ESP32 strapping pin (boot mode).** The ENV HAT wires SDA to it, and
> this works because I²C idles high via the pull-ups — the normal-boot level.
> But anything holding that line LOW at reset stops the device booting.
> **(verify)** a clean boot with the HAT fitted. *Fallback:* since 0x5C and 0x44
> don't collide, the HAT's sensors can be rewired onto `bus_box` with a harness
> and the G0/G26 bus dropped entirely.

### Sensor power — no switched rail (ADR-020)

**There is no GPIO2 equivalent on this board and no power-gating switch in the
config.** Sensors are permanently powered from the AXP192's rails, so ADR-004's
cut-in-sleep tradeoff does not apply here: there is no cold-start warm-up to
schedule, and correspondingly the STTS22H poll-until-valid pattern is gone. The
100 ms delay retained before the first read is settling margin, not a warm-up.

The Grove port's VCC is **5 V**, boosted by the AXP192 and gated by the EXTEN bit
in register 0x12 — `axp192_init()` forces that bit on so the ENV II Unit is
powered. **(verify)** that Grove 5 V is actually present when running on the
cell, not just on USB. *Fallback:* power the Unit from the HAT header's 3V3 pin
(the SHT30 is 3.3 V native) with a custom harness.

### Battery / power monitoring

| Signal   | Bus / addr        | Notes                                           |
|----------|-------------------|-------------------------------------------------|
| Cell V   | G21/G22 (bit-banged) 0x34, regs 0x78/0x79 | AXP192 12-bit battery ADC, 1.1 mV/LSB. **No divider, no fuel gauge on this board.** |

The **onboard ~95 mAh cell is the supply** (ADR-022). Battery monitoring is a
direct AXP192 register read via `logger/axp192.h` — over that header's own
**bit-banged software I²C** on G21/G22, because there is no third hardware I²C
peripheral to give it (ADR-023) — feeding a template sensor that
keeps the id `batt_v`; SoC (%) is still estimated from that voltage by
`li_ion_soc()` in `logger/helpers.h` (same 1S chemistry as the retired JST cell).
Both voltage and the estimated % ride in every Telegram report. A failed I²C read
returns **NaN**, not 0 — every downstream path degrades that to "battery n/a",
whereas a 0 would look like a flat cell and trip the protective floor.

> The AXP192's ADCs are **disabled at power-on** and ESPHome never calls M5's
> `Axp.begin()`, so `axp192_init()` must write reg 0x82 on every wake or the
> voltage registers read zero. **(verify)** that `batt_v` is plausible on the
> *first* wake after init, not only on later ones; add a delay before
> `component.update: batt_v` if the first conversion has not landed.

### LED / buttons / display

| Signal        | GPIO | Notes                                                     |
|---------------|------|-----------------------------------------------------------|
| Red LED       | 10   | Active LOW. Keep dark in normal operation (power).        |
| Button A      | 37   | ext0 deep-sleep wake source (on-demand report).           |
| Button B      | 39   | Unused.                                                   |
| TFT           | —    | **Unused (ADR-002).** LDO2/LDO3 held off by `axp192_init()`. |
| RESET         | —    | Hardware reset button (not a GPIO).                       |

Button A (GPIO37) is the **ext0 deep-sleep wake source** for an on-demand report
("wake, connect, send now, sleep"). It is an input-only RTC GPIO, which is what
makes it valid for ext0, and the buttons pull to GND — hence `inverted: true`.

> **(verify) that GPIO37 is pulled up and does not float.** This is the issue #39
> failure mode, and it is the single cheapest way to destroy the power budget:
> on the Feather the floating ext0 pin made the board self-wake every ~30-50 s
> instead of holding its 15-min sleep. The StickC is believed to fit button
> pull-ups on the board, unlike the bare Feather header — confirm on the
> schematic, then confirm the sleep interval actually holds before trusting any
> runtime measurement.

---

## Power architecture — the defining constraint

### Supply — LOCKED (ADR-022)
Powered by the **M5StickC's onboard ~95 mAh single-cell Li-ion**, as the **sole
supply**, charged by the AXP192 whenever USB-C is connected. ADR-022 supersedes
ADR-010 (JST 18650) which superseded ADR-009 (USB power bank).

- There is no JST connector and no user-fitted cell, so the 1S/polarity hazards
  of ADR-010 are gone with it.
- **Recharge over USB-C:** the AXP192 tops the cell up automatically whenever USB
  is connected. There is no permanent USB source — recharging is a manual/periodic
  action, and the firmware **low-battery Telegram warning** is the cue to do it.
- **Future option (not scope):** the HAT header exposes a `BAT` pin, so a larger
  cell can be fitted in parallel and the AXP192 will charge it. That is the
  documented route back to long runtime if the onboard cell proves too limiting.

### The defining constraint — battery runtime
**Runtime per charge is the defining constraint, and it is much tighter on this
board.** Two things changed at once versus the Feather: capacity dropped from
~3000 mAh to ~95 mAh, and the AXP192 adds a continuous quiescent draw that the
Feather did not have. Consequently runtime is dominated by the **PMIC's idle
current rather than the wake cadence** — lengthening `SAMPLE_INTERVAL` buys much
less here than it would have on the Feather, which is why the 15 min / 4 h cadence
is kept unchanged. A **short runtime (days, not months) is an accepted trade** for
this board. **Measure the real average current on hardware** and record the
resulting runtime and recharge cadence in ADR-022.

The power levers that remain: `axp192_init()` holding LDO2/LDO3 (backlight and
panel) off, and ADR-006's radio policy. The ADR-004 rail-cut lever is gone —
there is no switched sensor rail on this board.

### Battery monitoring & protection — firmware
- **Voltage from the AXP192** (regs 0x78/0x79 via `logger/axp192.h`), read every
  wake (`component.update: batt_v`); **SoC (%) estimated** from that voltage by a
  template sensor calling `li_ion_soc()` (`component.update: batt_pct`, updated
  after `batt_v`). Both voltage and % appear in every Telegram report and the
  boot message; NaN degrades to "battery n/a".
- **Low-battery warning:** below `BATT_WARN_PCT` (default **20 %**) the firmware
  fires an early Wi-Fi Telegram alert, latched (`g_last_batt_alert`) so it sends
  **once per depletion** and clears on recharge past `BATT_RECOVER_PCT` (30 %).
  On a 95 mAh cell this gives **far less notice than the 18650 did** — treat it
  as urgent, and consider retuning `BATT_WARN_PCT` upward once real discharge
  data exists.
- **Protective floor:** below `BATT_CRIT_V` (default **3.30 V**) the scheduled
  4-h report is **skipped** to conserve charge. Temp-**CRIT** alerts still send —
  food safety outranks battery conservation. Thresholds are `constexpr` in
  `logger/helpers.h`.

### No fuel gauge on this board either
The M5StickC has **no fuel-gauge IC and no ADC battery divider**. Battery state
comes from the AXP192's own 12-bit battery-voltage ADC over I²C. Two consequences
carried over from the Feather: SoC from voltage is approximate (voltage sags under
load), and the AXP192's ADCs are **off at power-on** — `axp192_init()` must enable
them (reg 0x82) on every wake, because ESPHome never runs M5's `Axp.begin()`.

---

## Operating cycle — LOCKED

The firmware is a **wake → sample → maybe report → sleep** state machine driven
by deep sleep. There is no continuous loop and no always-on Wi-Fi.

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
     • approach (temp_a near CRIT)   ──► early scheduled report│
     • all others (WARN, fridge CRIT) ──► batched in report    │
        │                        (each with own hold-off)      │
   wake_count reached REPORT_EVERY? ──► Wi-Fi ON               │
        │                              connect + Telegram      │
        │                              report + clear buffer   │
        └──────────────────────► sleep again ─────────────────┘
```

- **`SAMPLE_INTERVAL`** — how often to wake and read (default **15 min**).
- **`REPORT_EVERY`** — samples per report; default **16** → a report every
  **4 h** at a 15-min sample interval. Change both together to keep the 4-h cadence.
- Sampling wakes do **not** enable Wi-Fi. Only report wakes (and CRIT-alert
  wakes) enable the radio — this is where nearly all the energy would otherwise go.

> **Implementation status:** the box-CRIT/WARN absolute alerts, the scheduled
> report, and the **predicted-breach** path are implemented today. The predictor
> lives in `logger/predictor.h` (ADR-013, issue #3), wired into `wake_cycle` as an
> early Wi-Fi alert between the box-CRIT check and the report; its state (box +
> fridge EMA rings, debounce ring, send-ack hold-off) is in `logger/ring_buffer.h`
> and it is host-tested in `test/test_predictor.cpp`. **Caveat:** `TAU_BOX_MIN` is
> still the pre-commissioning placeholder, so the predictor's output is unvalidated
> until the τ_box fit is done (see Open items). The **approach-trigger** path
> (ADR-014) remains documented design, not yet implemented.

### Wi-Fi policy — LOCKED
`wifi:` is configured with **`enable_on_boot: false`**. The radio is brought up
explicitly with `wifi.enable` only on a report or alert wake, and the device
does not sleep until the send has completed or timed out (`deep_sleep.prevent`
around the send, `deep_sleep.enter` on completion/timeout). Never leave Wi-Fi on
across a sampling wake.

---

## RTC-memory ring buffer — LOCKED

Readings accumulated between reports live in **RTC slow memory**, which survives
deep sleep (but **not** a full power loss / bank shutoff without a backup cell).
This was chosen over NVS to avoid flash wear from frequent small writes.

- Implemented in `logger/ring_buffer.h` as an `RTC_DATA_ATTR` struct, included
  into the build via `esphome: includes:` (same include pattern the e-Ink
  sibling uses for `display/*.h`).
- The buffer holds up to **`RING_CAPACITY`** samples (default 32 — headroom over
  the 16 needed per report). Each sample: `{ uint32_t epoch; float temp_a;
  float temp_b; }`.
- A separate `RTC_DATA_ATTR uint16_t g_wake_count` counts wakes since the last
  report; `RTC_DATA_ATTR AlertState g_last_alert` holds the last alert state for
  the CRIT-alert hold-off.
- **Predictor neighbours (implemented, ADR-013 / issue #3):** the ring buffer now
  also holds the predictor's RTC-memory state — the last `PREDICT_SMOOTH_N` `temp_b`
  samples (`g_fridge_hist`, fridge EMA) and `temp_a` samples (`g_box_hist`, box
  EMA), the last `PREDICT_DEBOUNCE_N` predictor decisions (`g_predict_hist`), the
  `g_last_predict_alert` last-emitted side, and `g_predict_holdoff` (the send-ack
  re-alert hold-off, in wakes). See `logger/predictor.h` for the firing rule.
- On a report wake: summarise the buffer (per-sensor current / min / max / mean
  / sample count), build the Telegram text, send, then **clear the buffer and
  reset `g_wake_count`**.
- **Power-loss behaviour:** if the bank cuts power, RTC memory is lost and the
  buffer starts empty on next boot. This is acceptable — the report is a running
  digest, not an audit log. If gap-proof history is ever required, that is a new
  ADR (candidate: append each report's digest to NVS, or a real logging sink).

Do **not** move the ring buffer to `globals: restore_value: true` — that path
uses NVS/flash and reintroduces the wear this design avoids.

---

## Thresholds & alerts — LOCKED (mechanism) / TUNABLE (values)

> **Which of these values are runtime-tunable (no reflash), and the guardrails
> that keep a bad `/set` from disabling alerting, are specced in
> `docs/settings-inventory.md` (issue #32) — the contract `logger/settings.h`
> (#4) codes against.**

The two sensors are **asymmetric** (see ADR-011): the box interior is the
payload-safety authority; the fridge sensor is a leading indicator whose
predictive value comes from what it says about where the box is heading.
Accordingly there are **four alert sources**, each with its own per-sensor
threshold set:

1. **Box absolute thresholds** (per-sensor five-zone, `temp_a`) — the pharma
   compliance authority. CRIT here is a real "act now" event and the ultimate
   safety net. **Implemented.**
2. **Fridge absolute thresholds** (per-sensor five-zone, `temp_b`) — advisory
   only. Its CRIT is batched into the 4-h report, not sent as an early alert
   (the predictive alert below is the actionable fridge-failure signal).
   **Implemented.**
3. **Predicted breach** — a physics-based estimate of when the box will cross a
   CRIT threshold given the current fridge/box gradient. Emits its own alert
   states `ALERT_PREDICTED_BREACH_LOW` / `..._HIGH`. See "Predictive alert"
   (ADR-013). **Implemented (`logger/predictor.h`, issue #3) — output unvalidated
   until `TAU_BOX_MIN` is calibrated.**
4. **Approach trigger** — the box is quietly sitting *close to* a CRIT bound
   with no strong gradient (so the predictor doesn't fire). Brings the scheduled
   report forward. See "Approach trigger" (ADR-014). **Design only.**

The absolute per-sensor sources feed the five base states
(`ALERT_OK, ALERT_WARN_LOW, ALERT_WARN_HIGH, ALERT_CRIT_LOW, ALERT_CRIT_HIGH`);
the predictor adds `ALERT_PREDICTED_BREACH_LOW` / `ALERT_PREDICTED_BREACH_HIGH`
and the sensor-fault detector (issue #12) adds `ALERT_SENSOR_FAULT`, for an
**eight-state** enum. All values are `constexpr` in `logger/helpers.h`.

### Box interior (`temp_a`, SHT30) — pharma-spec, LOCKED bounds

CRIT boundaries are pinned to the **insulin/pharma refrigerated range 2–8 °C**.
Do not silently loosen these — they are the compliance line.

| Zone          | Condition                | Value        | Alert state      |
|---------------|--------------------------|--------------|------------------|
| CRITICAL_LOW  | `< THRESH_BOX_CRIT_LOW`   | < 2.0 °C     | ALERT_CRIT_LOW   |
| WARNING_LOW   | crit_low .. warn_low     | 2.0–3.0 °C   | ALERT_WARN_LOW   |
| OK            | warn_low .. warn_high    | 3.0–7.0 °C   | ALERT_OK         |
| WARNING_HIGH  | warn_high .. crit_high   | 7.0–8.0 °C   | ALERT_WARN_HIGH  |
| CRITICAL_HIGH | `> THRESH_BOX_CRIT_HIGH`  | > 8.0 °C     | ALERT_CRIT_HIGH  |

`THRESH_BOX_CRIT_LOW = 2.0`, `THRESH_BOX_WARN_LOW = 3.0`,
`THRESH_BOX_WARN_HIGH = 7.0`, `THRESH_BOX_CRIT_HIGH = 8.0` (°C).

### Fridge ambient (`temp_b`, DHT12) — advisory bounds

Wider bands, shifted slightly lower (a fridge holding 2 °C is fine; a box at
2 °C is on the edge). Absolute CRIT here is **advisory only** — reported in the
4-h digest, not an early Wi-Fi alert. The predictor is the early-alert path for
fridge problems.

| Zone          | Condition                   | Value        | Alert state      |
|---------------|-----------------------------|--------------|------------------|
| CRITICAL_LOW  | `< THRESH_FRIDGE_CRIT_LOW`   | < 0.0 °C     | ALERT_CRIT_LOW   |
| WARNING_LOW   | crit_low .. warn_low        | 0.0–2.0 °C   | ALERT_WARN_LOW   |
| OK            | warn_low .. warn_high       | 2.0–6.0 °C   | ALERT_OK         |
| WARNING_HIGH  | warn_high .. crit_high      | 6.0–9.0 °C   | ALERT_WARN_HIGH  |
| CRITICAL_HIGH | `> THRESH_FRIDGE_CRIT_HIGH`  | > 9.0 °C     | ALERT_CRIT_HIGH  |

`THRESH_FRIDGE_CRIT_LOW = 0.0`, `THRESH_FRIDGE_WARN_LOW = 2.0`,
`THRESH_FRIDGE_WARN_HIGH = 6.0`, `THRESH_FRIDGE_CRIT_HIGH = 9.0` (°C).

`alert_state(float value, bool is_box)` selects the box set when `is_box` is
true, the fridge set otherwise. Call it with `is_box = true` for `temp_a`,
`false` for `temp_b`.

### Predictive alert — IMPLEMENTED (`logger/predictor.h`, issue #3) / TUNABLE (values)

The box is a lumped thermal mass exchanging heat with the fridge air, so
Newton's law of cooling applies. With `T_box = temp_a` (box interior) and
`T_fridge = temp_b` (fridge air), `delta = temp_b − temp_a`:

```
    dT_box/dt = (T_fridge − T_box) / τ_box = delta / τ_box
    T_box(t)  = T_fridge − delta_now · exp(−t / τ_box)
```

Solving for the time until `T_box` crosses a box CRIT threshold `T_thresh`:

```
                              ⎛  T_fridge − T_box_now  ⎞
    t_to_threshold  =  τ_box · ln ⎜ ───────────────────── ⎟
                              ⎝  T_fridge − T_thresh   ⎠
```

Directional: `T_thresh = THRESH_BOX_CRIT_HIGH` (8 °C) when `T_fridge > T_box`
(box heading up), `T_thresh = THRESH_BOX_CRIT_LOW` (2 °C) when `T_fridge < T_box`
(box heading down). If `T_fridge` sits between the two box CRIT bounds — normal
operation — `t_to_threshold` is very large or infinite; no alert.

**Firing rule.** On every sample: (1) smooth `T_fridge` (`temp_b`) with an EMA
over the last `PREDICT_SMOOTH_N` samples (default **4** = 60 min) to average out
compressor cycling; (2) compute both `t_to_hit_high` and `t_to_hit_low`, keep
whichever is finite and smaller; (3) fire `ALERT_PREDICTED_BREACH_HIGH` /
`..._LOW` (early Wi-Fi alert) when **all** hold — `t_to_threshold <
PREDICT_HORIZON` (default **90 min**); the same side has satisfied the rule for
the last `PREDICT_DEBOUNCE_N` samples (default **2** = 30 min, killing brief
door-open transients); and not within hold-off (`PREDICT_HOLDOFF_SAMPLES`,
default **8** samples = 2 h at 15-min sampling).

**τ_box calibration.** V1 ships with a one-off commissioning fit: warm the box
to room temp, log its cooldown curve in the running fridge, fit
`T_box(t) = T_fridge + (T_room − T_fridge) · exp(−t/τ)`, and pin the extracted τ
as `TAU_BOX_MIN` in `logger/helpers.h`. Until that fit is done, `TAU_BOX_MIN`
holds a **documented placeholder** and the predictor's output must be treated as
unvalidated. Online τ estimation is a future ADR.

`TAU_BOX_MIN`, `PREDICT_HORIZON`, `PREDICT_SMOOTH_N`, `PREDICT_DEBOUNCE_N`, and
`PREDICT_HOLDOFF_SAMPLES` are `constexpr` defaults in `logger/helpers.h`
(landed by issue #1). The firing logic lives in `logger/predictor.h`
(`ema`, `t_to_threshold`, `predictor_evaluate`, `predictor_step`), landed by
issue #3 and host-tested in `test/test_predictor.cpp`.

### Approach trigger — DESIGN (not yet implemented) / TUNABLE (values)

Covers the gap between the predictor (needs a gradient) and the scheduled 4 h
report (waits for cadence): the box is *quietly sitting close to* a CRIT bound
with no strong gradient. Compute `approach_high = THRESH_BOX_CRIT_HIGH − temp_a`
and `approach_low = temp_a − THRESH_BOX_CRIT_LOW`; if either falls in
`0 < approach < APPROACH_MARGIN_C` (default **0.5 °C**) and not within hold-off,
bring the scheduled report forward (send the normal report body prefixed
`EARLY:`, clear the ring, reset `g_wake_count` and the 4 h cadence). **Box-only**
(`temp_a`); fridge trajectory is the predictor's problem. Box CRIT and predicted
breach take precedence over an approach send on the same wake.

### Alert reporting policy — LOCKED

| Source                        | Behaviour                                              |
|-------------------------------|--------------------------------------------------------|
| Box CRIT (`temp_a`, absolute) | **Early Wi-Fi alert** on entry, hold-off armed on send-ack |
| Box CRIT — still in-zone      | **Reminder** every `crit_realert_holdoff_min` min (#65, `0`=off); **escalation** on ≥1.5 °C worsening (#18) — ADR-019 |
| Box WARN (`temp_a`, absolute) | Batched into scheduled 4-h report                      |
| Fridge CRIT (`temp_b`, abs.)  | Batched into scheduled 4-h report (advisory)           |
| Fridge WARN (`temp_b`, abs.)  | Batched into scheduled 4-h report                      |
| Predicted breach              | **Early Wi-Fi alert** on entry, hold-off armed on send-ack; **escalation** on ETA halving (#18) |
| Box approach (`temp_a`)       | **Early scheduled report** (not alert), resets cadence *(design)* |

- The early **box-CRIT** path uses `g_last_alert` (RTC memory) as a hold-off so
  it doesn't re-alert every sample while `temp_a` stays in the same CRIT zone.
  While in-zone it may still speak again — a time **reminder** (#65) or a
  worsening **escalation** (#18); see ADR-019. The scheduled report still fires
  normally.
- **Hold-offs advance on send-acknowledgement, not on state entry (ADR-018,
  issue #14).** Every early-alert latch — box-CRIT (`g_last_alert`), predicted
  breach (`g_predict_holdoff`), sensor-fault (`g_fault_latch_*`) and low-battery
  (`g_last_batt_alert`) — is armed **only inside its send script's
  `http_request` `on_response` 2xx branch**, never at dispatch. The wake_cycle
  dispatch sets only the RAM-only `g_wake_handled` double-dispatch guard. A send
  that fails mid-emergency (Wi-Fi drop, TLS/HTTP timeout) therefore leaves the
  latch untouched, so the next wake re-evaluates and **retries** instead of going
  silent for the full quiet window. The arming helpers (`crit_latch_arm`,
  `fault_latch_arm`, `batt_latch_arm`, `predict_holdoff_arm`) live in
  `logger/ring_buffer.h` and are host-tested. The report's diagnostics block
  shows a **`last send`** wall-clock line (`g_last_send_epoch`, set via
  `mark_send_ok()` on every confirmed 2xx) so a run of missed messages is visible.
- Threshold checks use each sensor's own reading against its own set; there is no
  cross-sensor logic in the absolute paths (only the predictor combines them).
- **Implementation note:** CRIT alert sends and scheduled report sends use
  **separate ESPHome scripts**. They differ in two ways: (1) the CRIT path calls
  `build_crit_alert()` while the report path calls `build_report()`; (2) only the
  report path clears the ring buffer and resets `g_wake_count` after sending —
  the CRIT path must leave the buffer intact so the next scheduled report
  includes the full window.

---

## Telegram report — LOCKED (transport) 

There is **no native Telegram component** in ESPHome. Reporting is an
`http_request` POST to the Telegram Bot API:

```
POST https://api.telegram.org/bot<BOT_TOKEN>/sendMessage
body: chat_id=<CHAT_ID>&parse_mode=HTML&text=<url-encoded message>
```

- Secrets: use `telegram_bot_token` and `telegram_chat_id` in `secrets.yaml`
  (matching the template in `secrets.yaml.example`). Build the request URL in
  the lambda: `"https://api.telegram.org/bot" + bot_token + "/sendMessage"`. Do
  **not** store a composite `telegram_send_url` secret — splitting token and
  chat id keeps them independently rotatable.
- The message body is built in a lambda (`logger/report.h`) from the ring-buffer
  digest. **Transport is LOCKED and unchanged; the body/format is not** — as of
  issue #63 the bodies use **`parse_mode=HTML`** and a four-tier information
  hierarchy (see `docs/telegram-message-ia.md` and ADR-017), *not* the earlier
  flat plain-text dump. Telegram HTML is a fixed tag whitelist (`<b> <i> <code>
  <pre> <blockquote>`; no CSS/colour/tables); only `& < >` need escaping via
  `html_escape()`, and malformed HTML is rejected HTTP 400 (a failed send), so the
  builders compose with `std::string` (never a fixed `snprintf` buffer that could
  truncate a tag) and a host test asserts tag balance. The tiers:
  - **0 verdict** — one bold line answering *is the box safe?* (`temp_a`, the
    payload-safety authority per ADR-011), with a 🟢/🟡/🔴/🛑 status emoji. A
    **faulted sensor reports FAULT, never a fabricated "OK"** (`sensor_faulted()`).
  - **1 evidence** — the box trend line (predictor: `t_to_hit`/"steady"; the ETA
    is marked "(est)" while `TAU_BOX_MIN` is uncalibrated) + the per-sensor
    current / min / max / mean `<pre>` table (columns labelled `Box`/`Fridge`).
  - **2 context** — fridge advisory reading; an out-of-range excursion count.
  - **3 housekeeping** — window (from `Sample.epoch`), uptime, wake count, RSSI,
    fw version, cadence, and the `overrides:` line — collapsed into a
    `<blockquote expandable>`. **Battery voltage + estimated SoC** (AXP192,
    ADR-022; "battery n/a" if unread) sits just above it, actionable.
  Alert bodies (CRIT / predicted breach / fault / low-battery) share the same
  HTML treatment plus a `alert_footer()` context line (battery + wall-clock). The
  inbound command reply (`g_cmd_reply`) stays **plain-text** (no `parse_mode`) —
  it echoes raw user text that could contain `<`/`>`.
- **Every message is prefixed with the device label** `Stick C` (ADR-024), because
  a sibling board reports to the same chat. Applied by `with_device_tag()` at the
  **send site**, never inside a builder — the CRIT body is composed of several
  builders, so an in-builder tag would double up. Use `html=false` for the
  plain-text command reply.
- **RSSI** is read from a `wifi_signal` sensor (`platform: wifi_signal` in
  `sensor:`); pass its `.state` to `build_report()` as the `rssi` argument.
  **Uptime** (`uptime_s`) is `(uint32_t)(millis() / 1000)` in the lambda — no
  extra ESPHome sensor needed.
- The send happens inside a `deep_sleep.prevent` guard; the report path clears
  the buffer only on a confirmed 2xx (`on_response`), so a failed send is
  retried on the next 15-min wake, then `deep_sleep.enter`.
- **TLS:** `verify_ssl: true` with a **pinned CA**:
  `ca_certificate_path: certs/telegram_root_ca.pem` — the GoDaddy Root CA G2
  (valid to 2037), which api.telegram.org chains to. Pinning the root survives
  Telegram's yearly leaf renewals. Verified end-to-end on hardware (HTTP 200).
  Note: the issue #24 `ESP_ERR_HTTP_CONNECT` failures turned out to be
  **network-side** (see Known issues), not a TLS/cert problem — but keep the
  pinned-CA setup; it is the correct configuration and closes the old
  `verify_ssl: false` bring-up workaround.
- **Time before TLS:** cert validation needs a valid clock and the device
  cold-boots at epoch 1970 with Wi-Fi off. Every send script waits for
  `sntp_time` to become valid (15 s timeout) after Wi-Fi connects, before the
  POST. Do not remove these waits.

---

## ESPHome configuration skeleton

> Illustrative, not exhaustive. The authoritative config lives in
> `telefridge.yaml`; keep this section in sync when structural things change.

### Board and framework — LOCKED

```yaml
esphome:
  name: telefridge
  includes:
    - logger/axp192.h
    - logger/helpers.h
    - logger/ring_buffer.h
    - logger/report.h
  on_boot:
    - priority: 100
      then:
        # Enable the AXP192 ADCs, hold the LCD rails off, keep Grove 5 V on.
        # No bus argument: the PMIC is bit-banged on G21/G22 (ADR-023).
        - lambda: 'axp192_init();'

esp32:
  board: m5stick-c    # M5StickC, ESP32-PICO-D4
  framework:
    type: esp-idf

# NO psram: block — the PICO-D4 has none; declaring it boot-loops the device.
```

Arduino framework is not used (esp-idf for deep-sleep + RTC-memory control and
parity with the sibling builds).

### Sensor power — nothing to configure

There is **no switched sensor rail on this board** and therefore no power switch
in the config (contrast the Feather V2's GPIO2). Rail control that does matter —
LCD off, Grove 5 V boost on — happens once per wake inside `axp192_init()`.

### I²C + sensors

```yaml
i2c:
  - id: bus_box               # Grove port  → ENV II Unit
    sda: GPIO32
    scl: GPIO33
    scan: true
  - id: bus_fridge            # HAT header  → ENV HAT (G0 is a strapping pin)
    sda: GPIO0
    scl: GPIO26
    scan: true
# NO third bus. The ESP32 has exactly two I²C peripherals and ESPHome enforces
# it; the AXP192 PMIC (G21/G22, 0x34) is bit-banged in logger/axp192.h — ADR-023.

sensor:
  - platform: sht3xd           # M5 ENV II Unit, box interior
    i2c_id: bus_box
    address: 0x44
    id: sht30_hub              # hub component id for explicit component.update
    temperature:
      name: "Temp A (ENV II SHT30)"
      id: temp_a
    # No humidity sub-sensor: temps-only. The SHT30 still measures RH in the
    # same conversion; add a `humidity:` block here to publish it (no extra I²C).
    update_interval: never     # read explicitly on wake

  - platform: dht12            # M5 ENV HAT, fridge ambient
    i2c_id: bus_fridge
    id: dht12_hub              # hub component id for explicit component.update
    temperature:
      name: "Temp B (ENV HAT DHT12)"
      id: temp_b
    update_interval: never

  # Battery voltage from the AXP192 (regs 0x78/0x79) — no divider, no fuel gauge
  # on this board (ADR-022). Read each wake via `component.update: batt_v`.
  - platform: template
    id: batt_v
    name: "LiPo Voltage"
    unit_of_measurement: "V"
    update_interval: never
    lambda: 'return axp192_batt_voltage();'
  # Estimated SoC (%) from that voltage; update AFTER batt_v.
  - platform: template
    id: batt_pct
    name: "LiPo %"
    unit_of_measurement: "%"
    update_interval: never
    lambda: 'return li_ion_soc(id(batt_v).state);'
```

`update_interval: never` on both temperature sensors is intentional — like the
display sibling's `never` refresh, readings are taken programmatically
(`component.update:` on each wake), not on a free-running interval. Both sensors
are **hub components**, so `component.update:` targets the hub id (`sht30_hub`,
`dht12_hub`) and the value is then read from the sub-sensor
(`id(temp_a).state`, `id(temp_b).state`). Both conversions also yield humidity,
not published (temps-only).

Neither part needs a cold-start poll loop: sensors are permanently powered here,
and the STTS22H's ~600 ms first-conversion latency (issue #44) left with it.

### Deep sleep + Wi-Fi

```yaml
deep_sleep:
  id: deep_sleep_1
  sleep_duration: 15min       # == SAMPLE_INTERVAL
  # wake also on GPIO37 (Button A) via ext0 — verify the pin is pulled up

wifi:
  enable_on_boot: false       # radio stays off except on report/alert wakes
  ssid: !secret wifi_ssid
  password: !secret wifi_password
```

Time: use `sntp` (only syncs on report/alert wakes when Wi-Fi is up). Sample
timestamps between reports come from the RTC/`millis` offset — do not assume a
fresh SNTP time on every sampling wake.

---

## File structure

```
telefridge_V1/
├── CLAUDE.md                 ← this file
├── telefridge.yaml       ← main ESPHome config
├── secrets.yaml              ← wifi creds, Telegram bot token + chat id (gitignored)
├── certs/
│   └── telegram_root_ca.pem  ← pinned GoDaddy Root CA G2 for api.telegram.org (public)
├── docs/
│   ├── settings-inventory.md ← Phase 1 runtime-settings inventory + guardrails (issue #32; the spec #4's logger/settings.h codes against)
│   └── telegram-message-ia.md ← Telegram message info-architecture + HTML formatting spec (issue #63 / ADR-017)
└── logger/
    ├── helpers.h             ← alert enum (7 states), per-sensor box/fridge thresholds + predictor constants (constexpr), alert_state(value, is_box), formatting
    ├── axp192.h              ← M5StickC PMIC support: bit-banged software I²C on G21/G22 (ADR-023) + axp192_init() (enable ADCs, LCD rails off, Grove 5 V on) + axp192_batt_voltage() — see ADR-020 / ADR-022
    ├── ring_buffer.h         ← RTC_DATA_ATTR sample ring buffer, wake_count, push/summary
    ├── predictor.h           ← ema(), t_to_threshold() closed form, predictor_evaluate/predictor_step firing rule (ADR-013, issue #3)
    ├── settings.h            ← RuntimeSettings, NVS load/store, /set* command parser + guardrails, apply_command() write path (issue #4)
    ├── realert.h             ← in-zone CRIT re-alert: #65 time reminder + #18 worsening escalation decision/arm helpers, send-ack-armed (ADR-018/019, issues #65/#18)
    ├── commands.h            ← Telegram getUpdates poll: parse + chat_id auth + dispatch to apply_command + cursor advance (ADR-016, issue #33/#8)
    └── report.h              ← build Telegram HTML message bodies (parse_mode=HTML, four-tier hierarchy, fault-aware, predictor trend) from the buffer digest — html_escape/zone_emoji in helpers.h (ADR-017, issue #63); DEVICE_LABEL + with_device_tag(), applied at every send site (ADR-024)
```

`.h` files are pulled in via `esphome: includes:`. Do not inline large lambdas
directly in the YAML.

---

## Conventions

- Temperatures in °C. Report values to 1 dp; keep 2 dp internally for min/max.
- All GPIO references use the `GPIONN` form, not bare integers.
- `temp_a` = ENV II SHT30 (0x44, `bus_box`) = **box interior / primary safety**;
  `temp_b` = ENV HAT DHT12 (0x5C, `bus_fridge`) = **fridge ambient / advisory
  leading indicator**. These roles and placement must never be swapped (ADR-011)
  — alert logic depends on it, and on this board the accuracy gap between the two
  parts makes a swap a compliance defect (ADR-021).
- Alert state is a C++ enum in `helpers.h` — the five sibling states, two
  predicted-breach states (`ALERT_PREDICTED_BREACH_LOW/HIGH`), and the
  sensor-fault state (`ALERT_SENSOR_FAULT`, issue #12), eight total.
- Secrets (`wifi_*`, `telegram_bot_token`, `telegram_chat_id`) live only in `secrets.yaml`.
- **This project has no display, no colour, no SD card, no screen inventory, and
  no navigation model.** The M5StickC physically has a TFT; it is deliberately
  unused and its rails are held off by `axp192_init()` (ADR-002). The LED stays
  dark in normal operation to save power; any use is a deliberate
  keep-alive/debug aid, not a UI. Do not port display, screen, or refresh logic
  from Tadorna e-Ink.

---

## Known issues and gotchas

| Problem | Root cause | Fix |
|---------|-----------|-----|
| **Device won't boot at all with the ENV HAT fitted** | G0 is an ESP32 strapping pin (boot mode select) and the HAT uses it as SDA. Anything holding that line LOW at reset forces the wrong boot mode | Unplug the HAT to confirm the cause. Check the HAT's pull-ups. *Fallback:* 0x5C and 0x44 don't collide, so the HAT's sensors can be rewired onto `bus_box` and the G0/G26 bus dropped |
| `temp_a` missing / `bus_box` scan empty | Grove 5 V absent — the AXP192's EXTEN bit gates that boost, and it is not on by default | `axp192_init()` forces EXTEN on every wake; confirm it runs before the first read. *Fallback:* power the ENV II Unit from the HAT header's 3V3 pin (the SHT30 is 3.3 V native) |
| **Board self-wakes every ~30-50 s instead of sleeping** (issue #39 pattern) | ext0 wake pin floating — no pull-up on GPIO37 | Confirm Button A is pulled up on the schematic; fit a 10 kΩ pull-up to 3V3 if not. This silently destroys the power budget, so verify the sleep interval holds before any runtime measurement |
| `temp_b` erratic / drifting high | (a) DHT12 in condensing fridge air — DHT-family parts are unreliable there; (b) HAT self-heating from the ESP32/AXP192 | Add an offset `filter:`; if unreliable, switch `temp_b` to the same HAT's BMP280 @0x76 via `platform: bmp280_i2c` (YAML-only swap) |
| Logger dies / resets randomly | Onboard cell depleted (sole supply, ADR-022) — only ~95 mAh, so this happens far sooner than it did on the 18650 | Recharge over USB-C; heed the low-battery Telegram warning, and treat it as urgent on this board |
| Report never sends | Wi-Fi never enabled (enable_on_boot:false) and `wifi.enable` not called on report wake | Ensure report/alert path calls `wifi.enable` and waits for connect before send |
| `ESP_ERR_HTTP_CONNECT` / `sock < 0` / esp-tls `select() timeout` (issue #24) | **Hotspot A/B test (2026-07-07) confirmed device + firmware are fine**: flashed with phone-hotspot credentials, WiFi connected at full signal, HTTP POST succeeded, Telegram message received — no TCP errors. Failure is 100% router/ISP-side on the home line. Leading suspects: (1) DS-Lite/CGNAT on the ISP dropping outbound TCP from small devices — `enable_ipv6: true` already in config; check boot log for SLAAC address on home router; (2) 2.4 GHz channel congestion at −70 dBm; (3) per-device ACL on the home router (by the ESP32's MAC). | Check router: IPv6 SLAAC assigned? 2.4 GHz channel (use 1/6/11)? Per-device firewall rules? Then soak ≥3 report wakes on home router. |
| Mac-side network tests mislead | The Mac runs a VPN whose kill-switch firewall blocks non-tunnel traffic — `curl --interface en0` failures measure the Mac's firewall, not the router; successes with VPN on exercise the tunnel path | Disable the VPN before using the Mac as a network reference; prefer on-device probes or a hotspot A/B |
| Buffer empty every report | Full power loss (cell fully depleted or unplugged) wiped RTC memory | Recharge before the cell dies — the low-battery warning + protective floor exist to prevent this; otherwise accept digest gaps |
| Sensors on the wrong bus / one never appears | Each device must name its `i2c_id` — two buses exist and the two sensors are on separate connectors | `bus_box` = G32/G33 (SHT30 0x44), `bus_fridge` = G0/G26 (DHT12 0x5C); confirm both in the boot scan. The AXP192 (G21/G22, 0x34) is bit-banged and will **not** appear in any scan (ADR-023) |
| **"BOX SENSOR FAULT" every ~30 min / `temp_a` NaN or frozen in the ring** | `sht3xd` does NOT publish synchronously: `update()` starts the measurement and publishes from a `set_timeout(50ms)` callback (unlike `dht12`, which publishes inline). Deep sleep wipes RAM, so `temp_a.state` is NaN at the start of every wake — any read sooner than ~50 ms after `component.update: sht30_hub` samples NaN. This put NaN into the ring AND into the box CRIT check, i.e. the compliance sensor was not being monitored at all on timer wakes | `wake_cycle` and the boot script `wait_until` `!std::isnan(id(temp_a).state)` (1 s timeout) before reading it. Do **not** replace this with a longer blind delay — the wait is robust to the platform's timing changing. A genuinely dead sensor still times out to NaN, which is what the fault detector should latch |
| **Config fails: "The maximum number of i2c interfaces for ESP32 is 2"** | A third `i2c:` bus was declared. The PICO-D4 has exactly two I²C peripherals and ESPHome enforces it — this is what ADR-020's original three-bus design hit | Keep only `bus_box` + `bus_fridge`. The PMIC belongs on the bit-banged path in `logger/axp192.h`, not on an `i2c:` bus (ADR-023) |
| **Battery reads 0.00 V or "n/a"** | The AXP192's ADCs are **disabled at power-on** and ESPHome never calls M5's `Axp.begin()` — so 0x78/0x79 read zero until reg 0x82 is written | `axp192_init()` writes 0x82 every wake; confirm it runs and that the first conversion has landed before `component.update: batt_v` (add a delay if not) |
| Device boot-loops immediately after flash | A `psram:` block in the config — the PICO-D4 has no PSRAM | Remove it entirely; there is no valid mode string for this MCU |
| Device won't re-sleep after report | `deep_sleep.prevent` left set / no completion path | `deep_sleep.enter` on `on_response` AND on a send timeout |
| Flash wears out over time | Ring buffer moved to `globals restore_value: true` (NVS) | Keep the buffer in RTC memory (`RTC_DATA_ATTR`), not NVS |
| Predictor fires nuisance alerts during compressor cycles *(once predictor ships)* | `PREDICT_SMOOTH_N` too small — fridge (`temp_b`) EMA follows the cycle | Increase `PREDICT_SMOOTH_N` (default 4 samples = 60 min) |
| Predictor never fires but box CRIT does, or fires far too early *(once predictor ships)* | `TAU_BOX_MIN` mis-set (still the placeholder, or wrong after a box change), or `PREDICT_HORIZON` off | Re-run the commissioning τ_box fit and pin `TAU_BOX_MIN`; sanity-check `PREDICT_HORIZON` against τ |

---

## Architecture decisions (ADRs)

### ADR-001 — Framework: esp-idf
esp-idf, not Arduino: needed for reliable deep-sleep control, RTC-memory
handling, and parity with the sibling builds.

### ADR-002 — Headless: no display, no SD
No display and no SD card on this build. State is an RTC-memory digest reported
over Telegram; there is no local UI or local file log.

### ADR-003 — Sensors: SHT40 (I²C) + STTS22H (I²C), both on STEMMA QT — SUPERSEDED (2026-08-04)
**Superseded by ADR-021** (M5 ENV II Unit SHT30 + ENV HAT DHT12, split buses),
which retired both parts along with the Feather V2 itself. Historical text below.

**Superseded 2026-07-21 (was SHT30/ENV II + DS18B20).** Both temperature points
are now **I²C on the shared STEMMA QT bus**: point A is an **Adafruit SHT40**
(0x44, ±0.2 °C typ.), point B is a **SparkFun Micro STTS22H** (0x3C, ±0.5 °C).
This retires the 1-Wire DS18B20 (GPIO26 freed) and the M5Stack ENV II/SHT30.

Rationale: an all-I²C, all-STEMMA-QT topology removes the 1-Wire bus, its 4.7 kΩ
pull-up, and the Grove→STEMMA adapter the ENV II needed — both parts now plug
straight into the Feather's STEMMA QT jack (daisy-chained). The `temp_a`/`temp_b`
contract, ring buffer, and report format are **unchanged** — only the two sensor
platforms in `sensor:` change (`sht4x` for A, `stts22h` for B). Trade-off vs the
DS18B20: point B loses the waterproof immersion probe form factor; if an immersed
probe is needed again that is a new ADR. Both parts are 3V3-native and both now
sit on the GPIO2-switched rail, so **both** need the post-wake warm-up delay
(previously only point A did). Temps-only: SHT40 humidity is not published
(available at no extra I²C cost); STTS22H is temperature-only.

> **Roles updated by ADR-011:** this ADR fixed the *parts* (SHT40 + STTS22H,
> all-I²C). Their *roles* are now **asymmetric**, not equal-weight — SHT40
> (`temp_a`) is the box-interior safety sensor and STTS22H (`temp_b`) is the
> fridge-ambient leading indicator. See ADR-011.

### ADR-004 — I²C power gating via GPIO2 in sleep — MOOT (2026-08-04)
**Moot under ADR-020:** the M5StickC has no switched sensor rail, so there is
nothing to gate and no warm-up to schedule. The decision has no force on the
current board; it is kept for the history of the STTS22H cold-wake saga
(issue #44), which was a direct consequence of it.

Historical: cut the STEMMA QT rail (GPIO2 LOW) during deep sleep to remove the
external-sensor load, re-enable + warm up on each wake. Traded a ~30 ms warm-up
for lower sleep current.

### ADR-005 — Storage: RTC-memory ring buffer
Between-report samples live in `RTC_DATA_ATTR` RTC slow memory, not NVS, to
avoid flash wear from frequent small writes. Accepts loss on full power cut (see
Power / bank shutoff). NVS or an external sink is a future ADR if gap-proof
history is required.

### ADR-006 — Wi-Fi only on report/alert wakes
`wifi: enable_on_boot: false`; radio enabled explicitly every 4 h (or on CRIT
entry). Sampling wakes keep the radio off — this is the core power decision.

### ADR-007 — Telegram via http_request
No native Telegram component exists; report via HTTPS POST to the Bot API
`sendMessage` endpoint. Token/chat id in `secrets.yaml`.

### ADR-008 — Alerts: WARN batched, CRIT early
WARN transitions ride the scheduled 4-h report; CRIT entry forces an early
Wi-Fi alert with a hold-off. Same five-state enum as the siblings, no colour,
no display encoding.

### ADR-009 — Power source: USB bank, no required LiPo — SUPERSEDED (2026-07-22)
**Superseded by ADR-010.** (Historical: powered from a USB bank; the bank
auto-shutoff problem was the defining risk, resolved with the IKEA VARMFRONT in
issue #11. An optional JST LiPo was the future enhancement — which ADR-010 now
makes the sole supply.)

### ADR-010 — Power source: JST single-cell Li-ion, sole supply — SUPERSEDED (2026-08-04)
**Superseded by ADR-022** (M5StickC onboard cell + AXP192), which retired the
JST connector along with the Feather V2. Historical text below.

Power is a **single-cell (1S, 3.7 V) Li-ion on the Feather's JST connector, as
the only supply** — the USB power bank is retired (issue #42). Recharge over
USB-C via the onboard charger.

Rationale: a far simpler topology that eliminates the bank auto-shutoff problem
entirely and enables genuine battery telemetry from the cell. The new defining
constraint becomes **battery runtime** (measure average current, size the cell,
set a recharge cadence). Consequences, all implemented in firmware:
- Battery becomes a **primary, always-on metric** — voltage from the **A13 /
  GPIO35 divider** (this board has **no fuel gauge**; see "No fuel gauge on this
  board"), SoC estimated from that voltage — in every report and the boot message.
- A **low-battery Telegram warning** (latched, once per depletion) at
  `BATT_WARN_PCT`, and a **protective floor** at `BATT_CRIT_V` that skips the
  scheduled report to conserve charge (temp-CRIT alerts still send).
- Trade-off: no bank means RTC memory is lost if the cell is ever fully depleted
  or unplugged — the warning + floor exist to keep that from happening; a
  protected cell is required so the hardware cutoff backs them up.

### ADR-011 — Sensor roles: box interior is primary safety, fridge is leading indicator
**Still in force.** The *parts* named below were replaced by ADR-021 (SHT40 →
ENV II SHT30 for `temp_a`; STTS22H → ENV HAT DHT12 for `temp_b`), but the
box/fridge **roles**, the `temp_a`/`temp_b` contract and every consequence of it
are unchanged. On the current board the role split is additionally forced by the
accuracy gap between the two parts — see ADR-021.

Supersedes the earlier "both sensors equal weight" stance. Because the payload
lives inside an **insulated box inside the fridge**, the two sensors measure
different things: **`temp_a` (SHT40), placed inside the box**, is the
pharma-compliance authority (spec range 2–8 °C); **`temp_b` (STTS22H), in the
fridge air**, is a leading indicator whose value lies in its relationship to the
box, not its absolute reading. Consequences: per-sensor threshold sets (box vs
fridge); fridge absolute CRIT downgraded to advisory (batched in report); an
actionable early-warning alert derived from both sensors together (see ADR-013).
Physical placement is now locked (SHT40 in box, STTS22H in fridge air); swapping
them silently is a compliance defect.

> **Note on sensor identity vs the design branch.** The predictive design
> originated on the unmerged `claude/device-settings-telegram-gkhyq4` branch,
> where the box sensor was a DS18B20 (1-Wire) and the fridge sensor an MCP9808.
> `main`'s validated hardware is all-I²C (ADR-003): here the **SHT40 (`temp_a`)
> is the box sensor** and the **STTS22H (`temp_b`) is the fridge sensor** — the
> sensor *identities* are flipped relative to that branch, but the box/fridge
> *roles* are the same. All predictor math uses `T_box = temp_a`,
> `T_fridge = temp_b`.

### ADR-013 — Predictive alert (Newton's law of cooling)
A physics-based leading-indicator alert: from the current fridge/box gradient,
predict when the box (`temp_a`) will cross a box CRIT bound —
`t = τ_box · ln((T_fridge − T_box) / (T_fridge − T_thresh))`. One mechanism
handles the canonical failure modes: fast failures shrink `t` toward zero;
door-open transients recover on the next sample and are eaten by a
`PREDICT_DEBOUNCE_N`-sample debounce; slow drifts shrink `t` monotonically over
hours and fire well before CRIT. One user-legible knob (`PREDICT_HORIZON` =
"how much warning do you want?"). Cost: introduces `τ_box` as a calibrated
quantity (V1 = one-off commissioning fit; online estimation is a future ADR).
**Status:** implemented. Constants + the two `ALERT_PREDICTED_BREACH_*` states
landed in `helpers.h` (issue #1); the firing logic is in `logger/predictor.h`
with its RTC state in `ring_buffer.h`, wired into `wake_cycle` and host-tested
(issue #3). The re-alert hold-off is armed on send-ack (a failed send retries
next wake). Output stays unvalidated until `TAU_BOX_MIN` is calibrated on the
physical box. `temp_a` (box) is now smoothed alongside `temp_b` (fridge) for
single-sample glitch rejection.

### ADR-014 — Approach trigger: bring the report forward when the box quietly nears CRIT
Covers a residual gap between the predictor (needs a gradient) and the scheduled
4 h report. When `temp_a` enters the top `APPROACH_MARGIN_C` (default 0.5 °C) of
the WARN band adjacent to a box CRIT bound with no strong gradient, it does *not*
raise an alert — it brings the scheduled report forward (normal report body
prefixed `EARLY:`, clears the ring, resets the 4 h cadence). Box-only (`temp_a`);
fridge trajectory is the predictor's job. Box CRIT and predicted breach take
precedence on the same wake. **Status: design only, not yet implemented.**

### ADR-015 — Alert debounce (N consecutive out-of-range samples) is permitted
The absolute WARN/CRIT paths classify each sample instantly today (effectively
`alert_debounce_n = 1`). This ADR **permits** an optional debounce: honour an
absolute WARN/CRIT only after **N consecutive** out-of-range samples on the same
side, N runtime-tunable (see `docs/settings-inventory.md`, Tier 2). This is the
documented fix for compressor-cycle alert chatter, and it touches the LOCKED
alert mechanism, so it needs this record even though it is runtime-configurable.

Constraints: **`N=1` is the shipped default** and reproduces today's behaviour
exactly, so default alerting is unchanged. Debounce delays an alert by at most
`(N−1) × sample_interval` — with the field cap `N ≤ 8` and a 15-min interval that
is ≤105 min, which is why the box CRIT path (food safety) should keep a low N;
the knob's main use is the fridge advisory band. Debounce is orthogonal to the
CRIT re-alert hold-off (that governs *repeat* alerts once in-zone; debounce
governs *entry*). **Status: design only — the settings inventory (#32) records
the knob and this ADR; the firing-side logic lands with #4.**

### ADR-016 — Remote settings via Telegram command polling (getUpdates)
The runtime settings from #4 (`logger/settings.h`) are changeable **from anywhere**
over the same Telegram chat the device already reports to — no proximity, no setup
mode. Telegram bots receive messages by webhook (impossible for a sleeping device
behind CGNAT/DS-Lite, see the issue #24 history) or by **`getUpdates` polling**;
the device already opens the radio on every report/alert wake, so it issues one
extra GET to `…/getUpdates` **at the end of each Wi-Fi-up wake, after the outbound
sendMessage**, parses pending `/set…` lines, and routes them through the *same*
`apply_command()` write path (all #32 guardrails already there). Implemented in
`logger/commands.h` (issue #33 / #8), wired as the shared `poll_commands` script
into every Wi-Fi-up path — the boot wake and all five terminal send paths (report,
CRIT, predicted-breach, sensor-fault, low-battery); the boot wake makes a button
press an on-demand command check. Host-tested in `test/test_commands.cpp`.

Consequences / rules:
- **Latency is the defining trade-off.** Commands apply on the *next* radio wake —
  up to the report cadence (4 h default), or near-immediately when sent as a reply
  to a CRIT alert (that alert wake polls too). Polling on every 15-min sampling
  wake is **off the table** — it would enable Wi-Fi every wake and demolish
  ADR-006. So the grammar stays limited to values that make sense applied-with-
  delay; there is no `/report_now`.
- **Auth is mandatory:** every update whose `chat.id` ≠ the configured
  `telegram_chat_id` is ignored (never dispatched) but still consumed (offset-
  advanced) so it can't re-deliver. Only the owner's chat can issue commands, over
  TLS (the existing pinned GoDaddy Root CA G2), from anywhere.
- **Cursor in NVS, not RTC:** the `getUpdates` offset lives in
  `RuntimeSettings.update_offset` (NVS-backed, #4) and is persisted *before*
  `deep_sleep.enter`, so a full power cycle (cell depleted/unplugged) can't replay
  old commands. One small write per command batch — fine under ADR-005.
- Cost: one extra HTTPS round trip per report/alert wake (a few seconds of radio
  already-up time), negligible against the send itself. Malformed/unknown commands
  get a short usage/rejection reply rather than silence.
- This is the **pull** half of remote control; it does **not** replace on-device
  Wi-Fi provisioning (#31's captive portal) — a bot channel only works once the
  device already has internet, so first-start provisioning stays portal-only.

> **Terminology:** the device **polls** Telegram for commands (never "drains" —
> that collides with battery drain, this project's constraint).

### ADR-017 — Telegram messages: HTML formatting + four-tier information hierarchy
The message **bodies** (not the transport, which stays LOCKED) move from a flat
plain-text dump to **`parse_mode=HTML`** with a deliberate information hierarchy:
verdict → evidence → context → housekeeping (see the Telegram-report section and
`docs/telegram-message-ia.md`, issue #63). Telegram HTML is a fixed tag whitelist
(`<b> <i> <code> <pre> <blockquote>`; no CSS/colour/tables), only `& < >` need
escaping, and malformed HTML is rejected HTTP 400 — a *failed send*.

Rationale: the old report was a flat list that (a) gave `temp_a` and `temp_b`
equal weight despite the ADR-011 asymmetry, (b) had no glanceable "is the payload
safe?" verdict, (c) rendered its ASCII table ragged in Telegram's proportional
font, and — the motivating defect — (d) rendered a **faulted primary sensor as
Zone "OK"** because `build_report()` never consulted `sensor_faulted()`.

Consequences, all implemented:
- Bodies compose with `std::string` (never a fixed `snprintf` buffer that could
  truncate a tag mid-way and 400 the send); a host test (`test/test_report.cpp`)
  asserts **balanced, whitelisted tags** and a < 4096-char bound.
- Escaping order is strict: `html_escape()` dynamic strings → wrap in tags →
  `json_escape()` → YAML URL-encode. `<pre>`/table content is fixed labels +
  `snprintf`'d floats (provably free of `& < >`), so only pass-through strings
  (`fault_reason`, `settings_overrides_line`, `FW_VERSION`) are escaped.
- The report now surfaces data that already existed but never reached a message:
  the **predictor** trend line (CLAUDE.md had called for it), the sample **window**
  from `Sample.epoch` (previously discarded by `ring_summary()`), a per-sensor
  **excursion count**, **fault-aware zones**, and a button-wake "on-demand" marker.
  `Digest` gained `first_epoch`/`last_epoch`/`n_out_a`/`n_out_b` for this.
- Fault-awareness lives **in the builder**, not in `alert_state()` — that function
  is pure and shared with the alert-firing path, and must not gain fault coupling.
- No extra radio time: alert enrichment (battery + wall-clock footer) uses values
  already in hand; ADR-006's power posture is unchanged.
- Runtime thresholds: #63 was rebased onto **#50** (now merged), so the verdict and
  boot zone labels classify via `alert_state_live()` — runtime `/setbox`/`/setfridge`
  overrides are reflected in the report, and the `overrides:` line in the diagnostics
  block now matches the active bands. (Two *displayed numbers* — the predictor target
  and `build_crit_alert`'s "limit" — still use the compile-time `THRESH_*`, matching
  the predictor, which #50 didn't wire to live bands; a small tracked follow-up.)
- **Not in scope** (kept separate): the Unicode trend graph (#22) and graph image
  (#55); on-device provisioning (#31).

### ADR-018 — Alert hold-offs advance on send-acknowledgement, not on state entry
The early-alert hold-off/latch for **every** Wi-Fi alert path — box-CRIT
(`g_last_alert`), predicted breach (`g_predict_holdoff`), sensor-fault
(`g_fault_latch_*`) and low-battery (`g_last_batt_alert`) — is armed **only on a
confirmed 2xx send**, inside the send script's `http_request` `on_response`
handler, never at the moment the alert *state* is entered.

Rationale: arming the hold-off at dispatch created a silent-failure hole — a send
that fails mid-emergency (Wi-Fi drop, TLS/HTTP timeout) would still start the
quiet window, suppressing the next attempt for its full duration while the payload
is actually at risk. Under this ADR a failed send leaves the latch untouched, so
the next wake re-evaluates and retries. The `wake_cycle` dispatch now sets only
the RAM-only `g_wake_handled` guard (double-dispatch protection within one wake);
the persistent latches move to `crit_latch_arm()` / `fault_latch_arm()` /
`batt_latch_arm()` / `predict_holdoff_arm()` in `logger/ring_buffer.h`, called
from the 2xx branch.

Consequences: the predictor already followed this rule (ADR-013); ADR-018
generalises it to CRIT, fault and battery. A shared `g_last_send_epoch` marker
(`mark_send_ok()`, set on every confirmed 2xx of any message) is surfaced as a
`last send` line in the report diagnostics so a run of missed sends is visible.
Host-tested in `test/test_ring_buffer.cpp` (latch semantics) and
`test/test_report.cpp` (the `last send` line). **Status: implemented (issue #14).**
This is the foundation the CRIT re-alert reminder (#65) and escalation-on-worsening
(#18) build on — a reminder clock must never start on a failed send. **Not yet
done:** a "> K consecutive failures → escalate retry cadence, never silence" guard
(issue #14 stretch) remains a follow-up.

### ADR-019 — Re-alert while still in CRIT: time reminder (#65) + worsening escalation (#18)
Once a CRIT alert has fired, the box speaks again for one of two reasons, decided
at a single in-zone point in `wake_cycle` and implemented in `logger/realert.h`:

- **Reminder (#65, time-based).** While a sensor stays in CRIT, re-send the alert
  every `crit_realert_holdoff_min` minutes (a runtime setting; **`0` = off = the
  single-shot default**, bit-for-bit today's behaviour). The interval is measured
  in **wakes** (`realert_wakes_for_min()`, using `sample_interval_min`), not a
  wall clock, so it works on sampling wakes that never sync SNTP.
- **Escalation (#18, worsening-based).** Bypass the reminder wait and speak *now*
  when the box has worsened by **≥ `ESCALATE_DELTA_C`** (1.5 °C) beyond the value
  that last alerted — `CRIT_HIGH` higher, `CRIT_LOW` lower. The predicted-breach
  path escalates on its **ETA collapsing to ≤ `ESCALATE_PREDICT_FRAC`** (half) of
  the last-alerted ETA. Escalation is **always on** (a safety improvement over
  today's silence) but bounded by **`ESCALATE_MAX_PER_HOUR`** (3), a single hourly
  budget shared by both escalation sources so it can't itself become chatter. On
  the same wake **escalation out-ranks the reminder**, and it re-arms the reminder
  clock so a further worsening still has room to escalate.

Both anchor on the **box** (`temp_a`, the ADR-011 safety authority); the fridge
advisory does not remind/escalate. Messages carry a `realert_prefix()` line
(`ESCALATION — getting worse` / `still critical (reminder)`); entry is unprefixed.

Discipline (ADR-018): the wake **decides** (a pure read of the RTC episode state
in `ring_buffer.h`); the send script's `on_response` 2xx branch **arms** the
episode (`crit_episode_begin` / `crit_realert_arm` / `predict_escalation_arm` /
`predict_alert_record`). A failed send leaves the clocks untouched → retry next
wake. `realert_tick()` (the once-per-wake elapsed-time advance) is the only thing
that moves without a send. Host-tested in `test/test_realert.cpp` and the HTML
prefix balance in `test/test_report.cpp`. Constants are `constexpr` in
`helpers.h` (fixed in firmware; not runtime-tunable in V1). **Status: implemented
(issues #65, #18) — reminder/escalation firing validated on host; on-hardware
worsening test pending, same as #14's negative-path field test.**

### ADR-020 — Board: M5StickC (2026-08-04)
The target board becomes the **M5StickC** (ESP32-PICO-D4). The Adafruit ESP32
Feather V2 is **retired**, and `telefridge.yaml` was ported in place rather than
forked — there is one config, for one board.

> **Amended by ADR-025 (2026-08-04):** "ported in place rather than forked" was
> true within `telefridge_V1`, but the StickC build now lives in its **own repo**
> (`telefridge_StickC_V1`). The *principle* is unchanged and still binding —
> **one config, one board** — it is now enforced by repo boundary rather than by
> overwriting. `telefridge_V1` keeps the Feather build on its `main`.

Rationale: the StickC is a self-contained, enclosed unit with an integrated cell,
charger and buttons, in a far smaller package than the Feather plus a separate
18650 holder. The trade is accepted deliberately (see ADR-022 on runtime).

Consequences:
- **Three I²C buses** instead of one: `bus_box` (Grove, G32/G33), `bus_fridge`
  (HAT header, G0/G26) and `bus_axp` (internal, G21/G22). The two temperature
  sensors are on separate physical connectors, so separate buses are structural,
  not a workaround. `bus_fridge` puts SDA on **G0, a strapping pin** — it works
  because I²C idles high, but it is a documented boot risk.
  > **Amended by ADR-023 (2026-08-04):** the third bus does not exist. The ESP32
  > has only **two** I²C peripherals, so `bus_axp` was never buildable — the
  > config failed validation the first time it was compiled. The two hardware
  > buses go to the sensors; the AXP192 is bit-banged. Everything else in this
  > ADR stands.
- **No `psram:` block.** The PICO-D4 has none; declaring it boot-loops the device.
- **No switched sensor rail.** GPIO2 and `logger/i2c_power.h` are gone, which
  makes ADR-004 moot and removes the cold-start class of bug entirely.
- Wake button moves GPIO38 → **GPIO37 (Button A)**, still ext0, still
  `inverted: true`. The issue #39 floating-pin hazard transfers with it and must
  be re-verified on this board.
- 4 MB flash instead of 8 MB — verify the esp-idf + TLS build fits its OTA slot.
- The board has a TFT. It stays **unused** (ADR-002 unchanged) and its rails are
  held off in firmware.
- **The port is board-local by construction:** `temp_a`, `temp_b`, `batt_v` and
  `batt_pct` kept their ids and meanings, so **no file in `logger/` changed** and
  the host test suite passes untouched. That property is worth preserving on any
  future board move.

### ADR-021 — Sensors: M5 ENV II Unit (SHT30, box) + ENV HAT (DHT12, fridge) (2026-08-04)
Supersedes ADR-003. Point A is an **M5 ENV II Unit** (SHT30, 0x44, Grove →
`bus_box`); point B is an **M5 ENV HAT** (DHT12, 0x5C, HAT header →
`bus_fridge`). The Adafruit SHT40 and SparkFun STTS22H are retired.

Rationale: both parts are M5-native, so they mate directly with the StickC's own
connectors — the Unit on the stock Grove cable (the only one that can reach
inside a sealed box), the HAT seated on the device itself. The ADR-011 box/fridge
role contract, the ring buffer, the predictor and the report format are all
**unchanged**; only the two sensor platforms move (`sht3xd` and `dht12`, both
core ESPHome).

Consequences and constraints:
- **The role assignment is forced, not preferred.** The DHT12 is ±0.5 °C and the
  box WARN shoulder is 1.0 °C wide, so the DHT12 cannot hold the pharma-compliance
  role; the SHT30 (±0.2 °C) takes `temp_a`. Swapping them is a compliance defect.
  This also puts the HAT's self-heating on the advisory sensor rather than the
  compliance one.
- **The compliance sensor's accuracy is unchanged** in practice (SHT40 ±0.2 °C →
  SHT30 ±0.2 °C typ), so the 2–8 °C bounds stand as-is.
- **The advisory sensor got noisier** (STTS22H ±0.5 °C → DHT12 ±0.5 °C, but with
  worse repeatability). `temp_b` feeds the predictor's `T_fridge` term, and
  `t_to_threshold` is highly error-sensitive as `T_fridge` approaches `T_thresh`
  — re-check `PREDICT_SMOOTH_N` when `TAU_BOX_MIN` is calibrated.
- **DHT-family parts are unreliable in condensing air**, which is exactly the
  fridge-ambient position. Documented fallback: the same HAT's **BMP280 (0x76)**
  via `platform: bmp280_i2c`, a YAML-only swap with no hardware change.
- **The STTS22H cold-wake read pattern is retired** with the part. The
  poll-until-valid loop, the `stts_poll_deadline` global and the LOCKED read
  pattern in this document are all gone; neither replacement has that pathology,
  and sensors are permanently powered on this board anyway.
- Temps-only is unchanged: both parts measure humidity in the same conversion and
  neither publishes it.

### ADR-022 — Power: M5StickC onboard cell + AXP192 telemetry (2026-08-04)
Supersedes ADR-010. The supply is the StickC's **onboard ~95 mAh single-cell
Li-ion**, charged by the **AXP192** over USB-C. Battery telemetry comes from the
AXP192's own 12-bit ADC (regs 0x78/0x79, 1.1 mV/LSB), read over G21/G22 —
originally as the `bus_axp` hardware bus, and since ADR-023 as bit-banged
software I²C, because the ESP32 has no third I²C peripheral to spare.

Rationale: it follows from ADR-020 — the board has an integrated cell and charger,
and no ADC divider or fuel gauge to read instead.

Consequences:
- **Runtime regresses hard and this is accepted.** Capacity falls ~3000 mAh →
  ~95 mAh *and* the AXP192 adds continuous quiescent draw the Feather lacked.
  Runtime is now dominated by **PMIC idle current rather than wake cadence**,
  which is why the 15 min / 4 h cadence is deliberately left unchanged —
  lengthening it buys little here. Expect days, not months. Measure the real
  average current and record it against this ADR.
- **`logger/axp192.h` is a hand-rolled register shim, on purpose.** There is no
  core ESPHome `axp192` component, and pinning an unversioned third-party fork
  into a safety-critical build was rejected. It does three things per wake:
  enable the ADCs (reg 0x82 — **required**, since ESPHome never calls M5's
  `Axp.begin()` and the registers otherwise read zero), hold LDO2/LDO3 (backlight
  and panel) off for ADR-002, and force EXTEN on so the Grove port's 5 V boost
  powers the ENV II Unit.
- **`li_ion_soc()` and every downstream battery path are unchanged** — same 1S
  chemistry, same `batt_v`/`batt_pct` ids, same NaN → "battery n/a" degradation.
  A failed I²C read must return NaN, never 0: a 0 would read as a flat cell and
  trip the `BATT_CRIT_V` protective floor.
- **The low-battery warning gives much less notice** on a 95 mAh cell. Consider
  raising `BATT_WARN_PCT` once real discharge data exists.
- **Future option, not scope:** the HAT header's `BAT` pin accepts a larger cell
  in parallel, which the AXP192 will charge — the documented route back to long
  runtime without changing firmware.

> **Amended by ADR-023:** the AXP192 is no longer reached over an ESPHome `i2c:`
> bus (there is no third peripheral for it). `logger/axp192.h` now bit-bangs
> G21/G22 itself. The register map, the NaN contract and every downstream
> battery path are unchanged.

### ADR-023 — AXP192 on bit-banged software I²C (2026-08-04)
Amends ADR-020 and ADR-022. The AXP192 PMIC is **not** an ESPHome `i2c:` bus
device. `logger/axp192.h` drives G21/G22 as plain open-drain GPIOs with its own
software I²C master.

Rationale — this is a hard constraint, not a preference. The ESP32 (PICO-D4) has
exactly **two** I²C peripherals, and ESPHome enforces it
(`components/i2c/__init__.py`, `VARIANT_ESP32: {"NUM": 2}`); there is no
software-I²C fallback in the component. ADR-020 specified three buses, so the
ported config **never validated** — this was found on its first compile, after
the port was committed. Something had to come off hardware I²C, and both
temperature sensors are ESPHome components that require a real bus *and* sit on
two different physical connectors, so neither can move or share. The PMIC is the
one device that can: it is read **once per wake**, at low speed, with no timing
constraint, no other master on the line, and we already own its access code.

Consequences:
- **No hardware change, no ADR-021 reversal.** Both sensors keep their native
  connectors and the `temp_a`/`temp_b` contract is untouched. The documented
  rewire-onto-one-bus fallback stays available but is *not* needed for this.
- `axp192_init()` and `axp192_batt_voltage()` **lose their bus argument** and
  configure their own GPIOs, so they have no component-ordering constraint —
  only the requirement to run before the first `batt_v` read.
- The master runs at **~100 kHz**, honours clock stretching with a ~1 ms timeout,
  and does a **9-clock bus recovery** at init in case a reset left a slave
  holding SDA. Any wire-level failure returns **NaN**, preserving ADR-022's
  contract that a dead bus must never read as 0 V (which would look like a flat
  cell and trip the `BATT_CRIT_V` floor).
- **Host-tested** (`test/test_axp192.cpp`): the header exposes its six pin
  primitives as a swappable HAL (`AXP192_TEST_HAL`), so the full protocol —
  START/STOP framing, MSB-first bytes, ACK/NACK, the repeated start of a register
  read, rail arithmetic, bus recovery, NaN-on-dead-bus — runs against a simulated
  AXP192 written to the I²C spec rather than to our master. Timing, edge rates
  and pull-up adequacy remain hardware items.
- Same posture ADR-022 already took: hand-rolled register access in a file we
  own, rather than pinning an unversioned third-party component into a
  safety-critical build.
- **Hardening:** register 0x12 is the only register this file read-modify-writes,
  and its bit0 is DC-DC1 — the ESP32's own 3.3 V supply. Acting on a corrupted
  read could write that bit low and power the board off mid-operation, so
  `axp192_rails_plausible()` rejects any 0x12 read with DC-DC1 clear: this code
  is *executing*, so that rail is provably on and such a read is provably wrong.
  Skipping costs one wake of stale rail state and self-corrects on the next.
  Nothing else in this file can brick anything — a bad ADC read is just a NaN.

**Status: implemented and VALIDATED ON HARDWARE (2026-08-04).** The boot Telegram
message reported **4.08 V / 94 %** on the first wake — correct for a 1S cell on
USB — so the software I²C master works at ~100 kHz against the real PMIC, with
the board's own pull-ups. The LCD also stays dark, confirming the 0x12 rail write
lands as well as the ADC read.

### ADR-024 — Every Telegram message is labelled with the device (2026-08-04)
This chat receives messages from **more than one device** (a sibling board posts
to the same channel), so every outbound message begins with a device label —
`DEVICE_LABEL = "Stick C"` — as the first line.

Rationale: an unlabelled alert is ambiguous exactly when ambiguity is most
expensive. A CRIT alert that doesn't say which box it came from is close to
useless when two boxes report to one chat.

The label is applied by `with_device_tag()` **at the send site**, not inside the
individual builders. That placement is load-bearing: several messages are
composed from more than one builder — the CRIT body is `realert_prefix()` + up to
*two* `build_crit_alert()` calls + `alert_footer()` — so tagging inside a builder
would emit the label twice on some paths and mid-message on others. One wrap at
the point a body becomes a message gives exactly one label, always first.

Consequences:
- Applied at **all seven send sites**: report, boot, CRIT, predicted-breach,
  sensor-fault, low-battery, and the inbound-command reply.
- The command reply is **plain text** (no `parse_mode`, ADR-017 — it echoes raw
  user input that could contain `<`/`>`), so it takes the untagged label form.
  `with_device_tag(body, html=false)`; passing `true` there would show the user a
  literal `<b>`.
- Host-tested in `test/test_report.cpp`: the label leads every message type, is
  present exactly once, leaves the HTML balanced, and keeps the body intact.

### ADR-025 — The StickC build lives in its own repo (2026-08-04)
This project is split out of `telefridge_V1` into its own **private** repo,
`telefridge_StickC_V1`. `telefridge_V1` keeps the retired Feather V2 build on its
`main`; this repo is the M5StickC build and the only place it is developed.

Rationale: two physical devices are now in service, reporting to the same
Telegram channel (which is what ADR-024's device label exists for). One repo per
board keeps each board's LOCKED invariants, bring-up state and ADR history
independent, and removes the risk of a Feather-era assumption silently leaking
into the StickC config — the exact failure ADR-020 warned about when it insisted
on "one config, for one board". That principle is unchanged; it is now enforced
by the repo boundary instead of by overwriting a shared file.

Consequences:
- **History starts fresh.** This repo begins at a single initial commit
  containing the finished StickC tree, rather than importing `telefridge_V1`'s 64
  commits. Consequence to be aware of: `git blame` and `git log` here begin at
  the split, and **every issue number referenced throughout this document
  (#3, #12, #14, #18, #24, #32, #39, #42, #63, #65 …) belongs to the
  `telefridge_V1` issue tracker**, not to this repo. Treat them as citations into
  that history. The Feather-era debugging record (the issue #24 router saga, the
  #39 floating-pin lesson, the #42 power-source work) is preserved there and is
  still the authority for *why* several invariants here are locked.
- **Private for now.** Going public needs `telefridge_V1` issue #69's
  prerequisites first — a GPLv3 licence and a liability disclaimer — which matter
  more here than usual: this monitors insulin-class refrigerated storage. Note
  this document also carries network SSIDs and a device MAC.
- Secrets were verified never to have been committed in the source history, so
  nothing needed scrubbing in the split.
- `enclosure/` (battery-holder STLs) was **not** carried over; it was untracked
  scratch work in `telefridge_V1` and unrelated to this firmware.

---

## Open items to finalise on the physical build

> **Status note:** the asymmetric-payload design (box interior = `temp_a`/SHT30 =
> primary safety; fridge ambient = `temp_b`/DHT12 = leading indicator) and the
> **predictive alert** (`logger/predictor.h`, issue #3) are implemented and wired.
> The **remote settings channel over Telegram** (`getUpdates` polling, ADR-016,
> `logger/commands.h`, issue #33 / #8) is implemented, host-tested, and
> **validated on hardware (2026-07-27)** — a live `/status` and `/setreport 8`
> round-tripped with the reply delivered and the setting persisted to NVS (see
> open items for the measured awake cost). Still **documented design, not code:**
> the **approach trigger** (ADR-014). The predictor's numbers are unvalidated
> until `TAU_BOX_MIN` is calibrated.

### M5StickC bring-up (2026-08-04, ADR-020/021/022/023)

**Bring-up is well advanced (2026-08-04): items 1, 4, 5, 6 and 7 PASSED on
hardware** — clean boot with the HAT, working battery telemetry over the
bit-banged PMIC bus, a build that fits flash, correct bus scans, and a working
button wake. A **full wake → sample → alert → send → sleep cycle has run
end-to-end on the device**, with Wi-Fi associated, SNTP synced to real
wall-clock, and **two Telegram messages delivered** (boot + a genuine CRIT alert,
since room temperature is `CRIT_HIGH` for a fridge payload).

**Still open: items 2, 3, 8, 9, 10** — and item 2 (floating ext0 pin) gates
item 9, so do it first. Nothing here has been near a fridge yet.

> **The port as first committed did not compile.** ADR-020's three I²C buses
> exceed the ESP32's two peripherals; it was never caught because the config was
> never built. Fixed by ADR-023 (AXP192 bit-banged). Lesson worth keeping: on
> this project a board port is not "done" until `esphome compile` succeeds —
> host tests pass without touching the board layer at all.

- [x] **1. ~~Clean boot with the ENV HAT fitted~~ — PASSED (2026-08-04).** Boots
      normally with the HAT seated, repeatedly, across many flash cycles. G0's
      strapping-pin risk did not materialise: I²C idles high via the pull-ups, as
      predicted. The rewire-onto-`bus_box` fallback is not needed.
- [ ] **2. Confirm GPIO37 (Button A) is pulled up and does not float.** This is
      the issue #39 failure — on the Feather a floating ext0 pin made the board
      self-wake every ~30-50 s instead of holding its 15-min sleep. Leave it
      sleeping ~1 h untouched and confirm the interval actually holds **before**
      trusting any runtime measurement. Fit a 10 kΩ pull-up to 3V3 if needed.
      > **Still open, and NOT yet disproven.** Every `wakeup_cause=2` seen so far
      > was a deliberate button press, so no spurious wake has been *observed* —
      > but no untouched long-sleep run has been done either. The device has
      > never been left alone longer than ~1 min. This needs a genuine
      > leave-it-alone hour before any current measurement (item 9) is meaningful.
- [ ] **3. Confirm Grove 5 V is live on battery, not just USB.** The rail comes
      from the AXP192 boost gated by EXTEN, which `axp192_init()` forces on. If
      `bus_box` scans empty on battery, power the ENV II Unit from the HAT
      header's 3V3 pin instead (the SHT30 is 3.3 V native).
- [x] **4. ~~Confirm `batt_v` is plausible on the FIRST wake~~ — PASSED
      (2026-08-04).** The boot Telegram message reported **4.08 V / 94 %** — a
      correct 1S Li-ion reading for a cell on USB, on the *first* wake after
      `axp192_init()`. No added delay was needed: the existing settling delay plus
      both temperature reads already cover the AXP192 ADC's first conversion.
      **This also validates the bit-banged I²C master (ADR-023) on hardware** —
      timing, edge rates and the board's pull-ups all work at ~100 kHz. Note the
      value is only visible in the Telegram message: this ESPHome build emits no
      `[D][sensor]` state lines, so serial logs show nothing either way.
      *Remaining sub-item:* confirm the voltage **trends** down on discharge and
      up on USB (folds naturally into item 9's runtime measurement).
- [x] **5. ~~Confirm the build fits 4 MB flash~~ — DONE (2026-08-04, on host).**
      First successful M5StickC compile: **Flash 52.3 %** (958,931 B of the
      1,835,008 B OTA slot), **RAM 13.7 %** (44,736 B). Comfortable margin; the
      4 MB flash and its two OTA slots are not a constraint. *(This compile is
      also what exposed the ADR-020 three-bus defect — see ADR-023.)*
- [x] **6. ~~Boot log sanity~~ — PASSED (2026-08-04).** Scans: `bus_box` → 0x44
      (SHT30) + 0x76 (ENV II's unconfigured BMP280); `bus_fridge` → 0x5C (DHT12)
      + 0x76 (BMP280) + 0x10 (BMM150), the last three all present-but-unconfigured
      as designed. `temp_a`/`temp_b` plausible and distinct. Boot Telegram message
      delivered with a real battery voltage and percentage. **The AXP192 (0x34)
      does NOT appear in any bus scan** — it is bit-banged, not an `i2c:` device
      (ADR-023); a plausible `batt_v` is its only evidence of life.
- [x] **7. ~~Button A from deep sleep~~ — PASSED (2026-08-04).** A press during
      deep sleep gave `wakeup_cause=2 first_boot=0 button=1`, and the on-demand
      path ran to completion: Wi-Fi up in ~4 s, send, command poll, back to sleep
      in ~6 s total.
- [ ] **8. Characterise `temp_b` (DHT12).** Log it against a reference thermometer
      after a report wake (Wi-Fi on ~12 s) to quantify HAT self-heating, and add an
      offset `filter:` if biased. Watch for erratic readings in condensing fridge
      air — if it misbehaves, switch `temp_b` to the HAT's BMP280 @0x76 via
      `platform: bmp280_i2c`.
      > **First data point (2026-08-04, on the bench, both sensors in still room
      > air):** `temp_b` reads consistently **~0.3–2.3 °C above** `temp_a` across
      > several boots (e.g. 29.90 / 29.64, 30.10 / 29.68, 31.60 / 29.33). Direction
      > and magnitude match the predicted HAT self-heating, but this is *not* a
      > calibration — it conflates self-heating with the two parts' ±0.2/±0.5 °C
      > tolerances and any real air gradient. Redo against a reference thermometer.
- [ ] **9. Measure average current → runtime + recharge cadence,** and record it
      against ADR-022. Expect days, not months, and expect PMIC quiescent draw to
      dominate rather than the wake cadence.
- [ ] **10. Re-tune `BATT_WARN_PCT`** once real discharge data exists — 20 % of
      95 mAh gives far less notice than it did on the 18650.

### Design items (board-independent)

- [ ] Tune the fridge (advisory, `temp_b`) thresholds against the real
      appliance's duty cycle. The box (`temp_a`) CRIT bounds (2–8 °C) are
      pharma-spec and should not be widened.
- [ ] **Calibrate `TAU_BOX_MIN` on the real box** (needed before the predictor
      is trusted). Warm the box to room temp, place it in the running fridge, log
      `temp_a` for at least 3×τ, fit
      `T_box(t) = T_fridge + (T_room − T_fridge)·exp(−t/τ)`, and pin the extracted
      τ as `TAU_BOX_MIN` in `helpers.h` (replacing the placeholder).
- [ ] Sanity-check `PREDICT_HORIZON` against the calibrated τ (a few sample
      intervals, comfortably shorter than the 4 h report cadence).
- [ ] Verify the predictor on a controlled door-open transient (should NOT fire)
      and a simulated failure (fridge unplugged → should fire within one horizon).
      *(`predictor.h` is implemented + host-tested (issue #3); this item is the
      remaining on-hardware validation, and depends on a calibrated `TAU_BOX_MIN`.)*
- [ ] Confirm the low-battery warning and the `BATT_CRIT_V` protective floor fire
      at their thresholds, and tune `li_ion_soc()` if the SoC estimate reads off
      under load. *(Carried over from issue #42; the mechanism is unchanged by the
      board move, only its voltage source is — see M5StickC item 4.)*

> **Retired with the Feather V2 (2026-08-04, ADR-020):** the GPIO38 pull-up fix
> (issue #39), the JST 18650 supply and its A13 divider validation (issue #42 /
> ADR-010), and the IKEA VARMFRONT power bank (issue #11 / ADR-009). Their
> successors are in the M5StickC bring-up list above — note that issue #39's
> floating-ext0-pin lesson transfers directly to GPIO37 and is still open there.
- [x] **Telegram command channel validated on hardware (ADR-016, issue #33 / #8,
      2026-07-27).** Live round trips confirmed on a button-forced report wake
      (`wakeup_cause=2 button=1`): `/status` → getUpdates HTTP 200 → parse →
      **reply delivered HTTP 200** (Telegram `message_id` returned); `/setreport 8`
      → `OK: report set to 8`, applied + `preferences: Writing 1 items` (NVS
      persist). Cursor advances correctly per command and **survived a reflash**
      (NVS-backed `update_offset` reused post-flash). **Awake-time cost: the poll
      added ~1.8 s** (getUpdates + reply) to a ~12.7 s report wake — negligible.
      Remaining, low-priority: exercise the unauthorised-chat rejection live with a
      second account (host-tested in `test/test_commands.cpp`); confirm offset
      survival across a *full power cycle* (reflash-survival already shown, and the
      cursor is NVS not RTC by design).
- [x] ~~Verify PSRAM mode string for the PICO-MINI-02 against a real build.~~
      **Moot (2026-08-04, ADR-020):** the PICO-D4 on the M5StickC has no PSRAM and
      the `psram:` block is removed entirely.
- [x] Confirm `http_request` HTTPS/CA setup for the Telegram endpoint on 2026.4.x.
      → Done: `verify_ssl: true` + pinned GoDaddy Root CA G2
      (`certs/telegram_root_ca.pem`) + SNTP wait before each send. Verified on
      hardware 2026-07-07 (HTTP 200 from sendMessage). Note mbedTLS here has
      `MBEDTLS_HAVE_TIME_DATE` off, so cert validity dates are not checked —
      the SNTP wait mainly gives real epochs to the ring buffer.
- [x] Issue #24 closed (2026-07-07): intermittent TCP failures were router-side.
      Root cause: the home router was band-steering 2.4/5 GHz under a single SSID;
      separating them into a dedicated 2.4 GHz SSID resolved the issue.
      Confirmed: boot message, boot-then-report cycle (3-min stress test at 10s
      interval), and second report all succeeded with zero TCP errors on the home
      router. Network: a dedicated 2.4 GHz SSID, credentials in `secrets.yaml`.
