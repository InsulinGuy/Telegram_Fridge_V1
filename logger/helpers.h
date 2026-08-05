#pragma once
// TeleFridge V1 — alert state machine + thresholds.
// See CLAUDE.md "Thresholds & alerts". Mechanism is LOCKED; the numeric
// thresholds are food-fridge defaults and are meant to be tuned per appliance.

#include <cmath>
#include <cstdint>
#include <string>

// Alert enum — the five sibling states, two predicted-breach states emitted by
// the predictor (ADR-013), and a sensor-fault state (issue #12). Eight total.
// See CLAUDE.md "Thresholds & alerts".
enum AlertState {
  ALERT_OK = 0,
  ALERT_WARN_LOW,
  ALERT_WARN_HIGH,
  ALERT_CRIT_LOW,
  ALERT_CRIT_HIGH,
  ALERT_PREDICTED_BREACH_LOW,   // predictor: box heading below CRIT_LOW
  ALERT_PREDICTED_BREACH_HIGH,  // predictor: box heading above CRIT_HIGH
  ALERT_SENSOR_FAULT,           // NaN / stuck / stale reading (issue #12)
};

// --- Per-sensor thresholds (°C), constexpr. See CLAUDE.md tables (ADR-011). ---
//
// Sensors are asymmetric: temp_a (SHT30) sits INSIDE the insulated box and is
// the pharma-spec safety authority; temp_b (DHT12) reads the fridge air and is
// an advisory leading indicator. Each is classified against its own set.

// Box interior (temp_a) — pharma-spec, LOCKED. CRIT bounds pinned to 2–8 °C.
constexpr float THRESH_BOX_CRIT_LOW  = 2.0f;
constexpr float THRESH_BOX_WARN_LOW  = 3.0f;
constexpr float THRESH_BOX_WARN_HIGH = 7.0f;
constexpr float THRESH_BOX_CRIT_HIGH = 8.0f;

// Fridge ambient (temp_b) — advisory, wider bands shifted lower. TUNE to the
// real appliance's duty cycle; its absolute CRIT is batched, not an early alert.
constexpr float THRESH_FRIDGE_CRIT_LOW  = 0.0f;
constexpr float THRESH_FRIDGE_WARN_LOW  = 2.0f;
constexpr float THRESH_FRIDGE_WARN_HIGH = 6.0f;
constexpr float THRESH_FRIDGE_CRIT_HIGH = 9.0f;

// --- Sensor-fault detection (issue #12). Consumed by ring_buffer.h. ----------
//
// A silent sensor failure defeats the whole monitor: alert_state() returns
// ALERT_OK on NaN, so a dead sensor would read "OK" and fire nothing. These
// constants drive the per-sensor fault detector (faults_observe / sensor_faulted
// in ring_buffer.h), which runs each wake BEFORE the CRIT/report paths.
//
// FAULT_CONSEC_N — consecutive NaN (either sensor) or stuck-STUCK_TEMP_B (temp_b)
//   reads before a fault latches. STALE_SAMPLES_M — consecutive unchanged-to-3dp
//   reads (either sensor) flagged as stale (8 × 15 min ≈ 2 h). STUCK_TEMP_B = 0.00
//   dates from the retired STTS22H, whose cold-power-up default it was (see #44);
//   it is kept for the DHT12 because a hard 0.00 surviving wake_cycle's
//   poll-until-valid loop is a genuine fault from any part on that bus.
constexpr uint16_t FAULT_CONSEC_N  = 2;
constexpr uint16_t STALE_SAMPLES_M = 8;
constexpr float    STUCK_TEMP_B    = 0.0f;

// --- Predictor defaults (ADR-013). Consumed by predictor.h (a later issue). ---
//
// TAU_BOX_MIN is the box thermal time constant in MINUTES. This is a
// PRE-COMMISSIONING PLACEHOLDER — it must be replaced by a fit to the real box's
// cooldown curve (see CLAUDE.md "τ_box calibration" and the Open items) before
// the predictor's output can be trusted.
constexpr float    TAU_BOX_MIN            = 120.0f;  // placeholder — calibrate!
constexpr uint16_t PREDICT_HORIZON        = 90;      // minutes of warning wanted
constexpr uint16_t PREDICT_SMOOTH_N       = 4;       // temp_b EMA window (samples)
constexpr uint16_t PREDICT_DEBOUNCE_N     = 2;       // consecutive samples to fire
constexpr uint16_t PREDICT_HOLDOFF_SAMPLES = 8;      // 8 × 15 min = 2 h hold-off

// --- Re-alert while still in CRIT (issue #65 reminder, #18 escalation) --------
// Two ways a still-critical box speaks again after its first alert, both riding
// the ADR-018 send-ack discipline (the clocks advance only on a confirmed 2xx):
//   #65 REMINDER  — time-based: re-send every crit_realert_holdoff_min minutes
//     (a runtime setting; 0 = off = today's single-shot behaviour) while a sensor
//     stays in CRIT. See docs/settings-inventory.md.
//   #18 ESCALATION — worsening-based: bypass the reminder wait and speak now when
//     the situation has got materially worse than the value that last alerted.
// Escalation is always on (a safety improvement over today's silence) but bounded
// by a per-hour cap so it can't itself become chatter. Both anchor on the BOX
// (temp_a, the pharma-safety authority, ADR-011); the predicted-breach path
// escalates on its ETA collapsing (see predictor.h / realert.h).
constexpr float    ESCALATE_DELTA_C      = 1.5f;  // box worsened this many °C past the last-alerted value → escalate
constexpr float    ESCALATE_PREDICT_FRAC = 0.5f;  // predicted ETA at/below this fraction of the last-alerted ETA → escalate
constexpr uint8_t  ESCALATE_MAX_PER_HOUR = 3;     // cap on escalation messages per hour (both sources share it)

// Classify a reading into an alert zone against an EXPLICIT band set. This is
// the single zone-ladder; both the compile-time alert_state() below and the
// settings-aware alert_state_live() (settings.h, issue #50) delegate here, so
// there is exactly one place that maps a value + four bounds -> AlertState.
inline AlertState alert_state_bands(float value, float crit_low, float warn_low,
                                    float warn_high, float crit_high) {
  if (std::isnan(value)) return ALERT_OK;   // no reading -> don't alert
  if (value <  crit_low)  return ALERT_CRIT_LOW;
  if (value <  warn_low)  return ALERT_WARN_LOW;
  if (value <= warn_high) return ALERT_OK;
  if (value <= crit_high) return ALERT_WARN_HIGH;
  return ALERT_CRIT_HIGH;
}

// Classify a single temperature reading into an alert zone against the box set
// (is_box = true, use for temp_a) or the fridge set (is_box = false, temp_b).
// Uses the COMPILE-TIME THRESH_* bands — the guardrail-#4 fallback. The live,
// runtime-tunable path is alert_state_live() in settings.h (issue #50); with
// factory-default settings the two agree bit-for-bit.
inline AlertState alert_state(float value, bool is_box) {
  return is_box
    ? alert_state_bands(value, THRESH_BOX_CRIT_LOW, THRESH_BOX_WARN_LOW,
                        THRESH_BOX_WARN_HIGH, THRESH_BOX_CRIT_HIGH)
    : alert_state_bands(value, THRESH_FRIDGE_CRIT_LOW, THRESH_FRIDGE_WARN_LOW,
                        THRESH_FRIDGE_WARN_HIGH, THRESH_FRIDGE_CRIT_HIGH);
}

// True only for the absolute CRIT zones. Predicted-breach states are handled by
// the predictor path (early alert), not by is_crit.
inline bool is_crit(AlertState s) {
  return s == ALERT_CRIT_LOW || s == ALERT_CRIT_HIGH;
}

// --- Battery (M5StickC onboard ~95 mAh Li-ion — the sole supply, ADR-022) ---
// This board has NO fuel-gauge IC and NO ADC divider. Cell voltage comes from
// the AXP192 PMIC's own 12-bit ADC (regs 0x78/0x79, 1.1 mV/LSB), read over the
// bit-banged software I²C in axp192.h (ADR-023); a wire-level failure returns
// NaN, never 0. SoC (%) is then estimated from that voltage by li_ion_soc().
//
// SoC (%) below BATT_WARN_PCT raises a low-battery Telegram warning on an early
// radio wake. The alert is latched (g_last_batt_alert) so it fires once per
// depletion, and clears once SoC recovers past BATT_RECOVER_PCT (a recharge).
constexpr float BATT_WARN_PCT    = 20.0f;
constexpr float BATT_RECOVER_PCT = 30.0f;   // hysteresis: clears the warn latch
// Below this cell voltage the scheduled 4-h report is skipped to conserve
// charge (a protective floor sitting above the protected cell's own ~2.5–3.0 V
// hardware cutoff). By this point the low-battery warning has already fired,
// well above it, at BATT_WARN_PCT. Temp-CRIT alerts still send regardless —
// food safety outranks battery conservation.
constexpr float BATT_CRIT_V      = 3.30f;

// Estimate single-cell Li-ion state-of-charge (%) from terminal voltage.
// Piecewise-linear over a typical discharge curve — rough by nature (voltage
// sags under load), exactly the "rough sense" Adafruit's divider gives. NaN in
// (no reading) → NaN out.
inline float li_ion_soc(float v) {
  if (std::isnan(v)) return v;
  struct P { float v, p; };
  static const P t[] = {
    {3.00f, 0.0f}, {3.30f, 5.0f}, {3.50f, 10.0f}, {3.60f, 20.0f},
    {3.70f, 35.0f}, {3.75f, 50.0f}, {3.80f, 60.0f}, {3.85f, 70.0f},
    {3.90f, 80.0f}, {4.00f, 90.0f}, {4.10f, 95.0f}, {4.20f, 100.0f},
  };
  constexpr int n = sizeof(t) / sizeof(t[0]);
  if (v <= t[0].v)   return 0.0f;
  if (v >= t[n-1].v) return 100.0f;
  for (int i = 1; i < n; i++) {
    if (v < t[i].v) {
      float f = (v - t[i-1].v) / (t[i].v - t[i-1].v);
      return t[i-1].p + f * (t[i].p - t[i-1].p);
    }
  }
  return 100.0f;
}

// Battery glyph for the report line: 🔋 healthy / 🪫 low.
inline const char *batt_glyph(float pct) {
  if (std::isnan(pct))       return "\xF0\x9F\x94\x8B";                 // 🔋 (unknown → neutral)
  if (pct >= BATT_WARN_PCT)  return "\xF0\x9F\x94\x8B";                 // 🔋
  return "\xF0\x9F\xAA\xAB";                                           // 🪫 low battery
}

inline const char *alert_label(AlertState s) {
  switch (s) {
    case ALERT_OK:                     return "OK";
    case ALERT_WARN_LOW:               return "COLD";
    case ALERT_WARN_HIGH:              return "WARM";
    case ALERT_CRIT_LOW:               return "TOO COLD";
    case ALERT_CRIT_HIGH:              return "TOO WARM";
    case ALERT_PREDICTED_BREACH_LOW:   return "TRENDING COLD";
    case ALERT_PREDICTED_BREACH_HIGH:  return "TRENDING WARM";
    case ALERT_SENSOR_FAULT:           return "SENSOR FAULT";
  }
  return "?";
}

// Status emoji for the Telegram verdict line (issue #63, made directional by
// issue #2). Cold zones render blue and warm zones red, matching the thermometer
// gradient documented in the CLAUDE.md threshold tables.
//
// Two mappings here are deliberate and must not be "tidied" back:
//   * ALERT_CRIT_LOW is 🟦, not 🔴, even though a sub-2 °C box is a pharma-spec
//     breach (ADR-011) exactly as serious as a hot one. The gradient is the
//     documented visual language; severity is carried by alert_label()'s
//     "TOO COLD" and the alert's explicit limit line, not by hue alone.
//   * ALERT_SENSOR_FAULT is 🔧 — off the scale in *shape* as well as hue. A
//     fault is not a point on the thermal scale, it is the absence of a
//     trustworthy reading, and the previous 🛑 was a red blob easily mistaken
//     for 🔴 at inline render size. Zero-hue tiles (⬛/⬜) can't be used: each
//     disappears into one of Telegram's two themes.
inline const char *zone_emoji(AlertState s) {
  switch (s) {
    case ALERT_CRIT_LOW:               return "\xF0\x9F\x9F\xA6";          // 🟦
    case ALERT_WARN_LOW:               return "\xF0\x9F\x94\xB7";          // 🔷
    case ALERT_OK:                     return "\xF0\x9F\x9F\xA2";          // 🟢
    case ALERT_WARN_HIGH:              return "\xF0\x9F\x9F\xA1";          // 🟡
    case ALERT_CRIT_HIGH:              return "\xF0\x9F\x94\xB4";          // 🔴
    case ALERT_PREDICTED_BREACH_LOW:
    case ALERT_PREDICTED_BREACH_HIGH:  return "\xE2\x9A\xA0\xEF\xB8\x8F";  // ⚠️
    case ALERT_SENSOR_FAULT:           return "\xF0\x9F\x94\xA7";          // 🔧
  }
  return "\xF0\x9F\x9F\xA1";                                              // 🟡 (unknown)
}

// The same zone gradient as zone_emoji(), for a surface that cannot render
// emoji: the M5StickC TFT status screens (ADR-026). Returns a packed 0xRRGGBB —
// a plain integer rather than ESPHome's Color, so this header stays host-
// testable and free of framework types; the display lambda wraps it in Color().
//
// It lives beside zone_emoji() ON PURPOSE. The two are the same decision
// rendered in two media, and when they lived apart they drifted: the screens
// were written against the old collapsed traffic-light scale and kept painting
// a sub-2 °C box red after the Telegram side had gone directional (issue #2).
// Add a zone here whenever you add one there, and keep the pairs aligned:
//   🟦 blue · 🔷 cyan · 🟢 green · 🟡 amber · 🔴 red · ⚠️ orange · 🔧 violet
// The two off-scale states carry the same reasoning as their glyphs. Predicted
// breach is orange, not amber — it must not read as "WARN, but on a screen".
// Sensor fault is violet: it is the absence of a reading, not a point on the
// thermal scale, and must be unmistakable against every hue on it.
inline uint32_t zone_color(AlertState s) {
  switch (s) {
    case ALERT_CRIT_LOW:               return 0x2962FFu;  // blue    ↔ 🟦
    case ALERT_WARN_LOW:               return 0x00B0FFu;  // cyan    ↔ 🔷
    case ALERT_OK:                     return 0x00C853u;  // green   ↔ 🟢
    case ALERT_WARN_HIGH:              return 0xFFD600u;  // amber   ↔ 🟡
    case ALERT_CRIT_HIGH:              return 0xD50000u;  // red     ↔ 🔴
    case ALERT_PREDICTED_BREACH_LOW:
    case ALERT_PREDICTED_BREACH_HIGH:  return 0xFF6D00u;  // orange  ↔ ⚠️
    case ALERT_SENSOR_FAULT:           return 0xAA00FFu;  // violet  ↔ 🔧
  }
  return 0xFFD600u;                                       // amber (unknown)
}

// Escape a string for Telegram parse_mode=HTML (issue #63). Only these three
// characters are special in Telegram's HTML subset; escaping them lets arbitrary
// dynamic text sit safely between tags without producing a malformed document
// (which Telegram rejects with HTTP 400, failing the send). Apply to any dynamic
// string BEFORE wrapping it in tags; snprintf'd floats are provably free of these.
inline std::string html_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;";  break;
      case '>': out += "&gt;";  break;
      default:  out += c;       break;
    }
  }
  return out;
}

// Five-column Unicode staircase signal-strength bar.
// ▁▃▅▇█ = U+2581/83/85/87/88; ░ = U+2591 (empty column).
inline const char *rssi_bars(int rssi) {
  if (rssi >= -55) return "\xe2\x96\x81\xe2\x96\x83\xe2\x96\x85\xe2\x96\x87\xe2\x96\x88"; // ▁▃▅▇█
  if (rssi >= -65) return "\xe2\x96\x81\xe2\x96\x83\xe2\x96\x85\xe2\x96\x87\xe2\x96\x91"; // ▁▃▅▇░
  if (rssi >= -72) return "\xe2\x96\x81\xe2\x96\x83\xe2\x96\x85\xe2\x96\x91\xe2\x96\x91"; // ▁▃▅░░
  if (rssi >= -80) return "\xe2\x96\x81\xe2\x96\x83\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91"; // ▁▃░░░
  if (rssi >= -90) return "\xe2\x96\x81\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91"; // ▁░░░░
  return             "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91"; // ░░░░░
}
