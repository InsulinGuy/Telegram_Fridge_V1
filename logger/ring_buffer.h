// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
#pragma once
// TeleFridge V1 — RTC-memory sample ring buffer.
// See CLAUDE.md "RTC-memory ring buffer" (LOCKED). Lives in RTC slow memory so
// it survives deep sleep but NOT a full power loss (JST Li-ion fully depleted or
// unplugged — issue #42). Chosen over NVS to avoid flash wear from frequent
// small writes.
//
// NOTE: RTC_DATA_ATTR requires the esp-idf framework (see ADR-001). These
// symbols persist across deep sleep; they reset to zero on cold boot / power loss.

#include <cstdint>
#include "helpers.h"

// Capacity: headroom over the samples needed per report (16 @ 15-min / 4-h).
constexpr uint16_t RING_CAPACITY = 32;

struct Sample {
  uint32_t epoch;    // unix time of the reading (0 if no valid clock yet)
  float temp_a;      // SHT30 (M5 ENV II Unit, Grove, I²C 0x44) — box interior
  float temp_b;      // DHT12 (M5 ENV HAT, HAT header, I²C 0x5C) — fridge air
};

// --- Persistent (RTC slow memory) state ---------------------------------
// Defined here (not extern): ESPHome compiles a single translation unit that
// includes this header exactly once, so a header-level definition is safe and
// avoids needing a separate .cpp. Do NOT include this header from a second TU.
RTC_DATA_ATTR Sample   g_ring[RING_CAPACITY];
RTC_DATA_ATTR uint16_t g_ring_count = 0;   // valid samples currently held
RTC_DATA_ATTR uint16_t g_ring_head = 0;    // next write index (wraps)
RTC_DATA_ATTR uint16_t g_wake_count = 0;   // wakes since last report
RTC_DATA_ATTR AlertState g_last_alert = ALERT_OK; // for CRIT-alert hold-off
RTC_DATA_ATTR bool g_first_boot = true;    // cleared after boot message is sent
RTC_DATA_ATTR bool g_last_batt_alert = false; // low-battery warn latch (issue #42)

// --- Send-acknowledgement bookkeeping (issue #14) -----------------------
// The hold-off/latch fields above (g_last_alert, g_last_predict_alert,
// g_fault_latch_*, g_last_batt_alert) must advance ONLY on a confirmed 2xx
// send, never at the moment an alert state is entered. Otherwise a send that
// fails mid-emergency (Wi-Fi drop, TLS timeout) would silently arm the hold-off
// and suppress the next attempt for the full quiet window — the silent-failure
// this issue closes. The wake_cycle dispatch now only sets the RAM-only
// g_wake_handled guard (double-dispatch protection); the persistent latches are
// armed by the arm_*_latch() helpers below, called from each send script's
// http_request on_response 2xx branch. On any non-2xx outcome the latch is left
// untouched, so the next wake re-evaluates and retries.
//
// Wall-clock (SNTP epoch) of the last confirmed 2xx send of ANY message. 0 until
// the first success. Surfaced in the report diagnostics so the owner can see at a
// glance whether messages are getting through. Set only via mark_send_ok().
RTC_DATA_ATTR uint32_t g_last_send_epoch = 0;

// --- Re-alert-while-in-CRIT episode state (issue #65 reminder, #18 escalation) --
// The firing logic lives in logger/realert.h (it needs g_settings); only the
// RTC-backed state lives here, alongside the other g_ globals. All of it advances
// ONLY on a confirmed 2xx send (ADR-018) except the once-per-wake ticks. Reset to
// zero on cold boot; the box-episode fields also reset on recovery (box leaves
// CRIT) via crit_episode_reset().
//
// Box-CRIT episode: countdown (in wakes) to the next #65 reminder, and the box
// reading (temp_a) that last set the #18 escalation baseline (NaN = no active box
// episode / baseline not yet established).
RTC_DATA_ATTR uint16_t g_crit_realert_ticks = 0;
RTC_DATA_ATTR float    g_crit_alert_value   = std::nanf("");
// Predicted-breach episode: the ETA (minutes) that last alerted, for the #18
// "ETA halved" escalation test. INFINITY = no active predicted-breach baseline.
RTC_DATA_ATTR float    g_predict_alert_tmin = INFINITY;
// Escalation rate limit — one fixed hourly budget shared by both escalation
// sources: g_escal_window_ticks counts the wakes left in the current hour,
// g_escal_count the escalations already spent in it.
RTC_DATA_ATTR uint16_t g_escal_window_ticks = 0;
RTC_DATA_ATTR uint8_t  g_escal_count        = 0;

// --- Predictor state (RTC slow memory) ----------------------------------
// Storage only — the firing rule (Newton-cooling projection + debounce) lives in
// the not-yet-implemented predictor.h (ADR-013, a later issue). Issue #2 lands
// just the state these buffers hold so predictor.h can read/advance them.
//
// Hold-off latch for the early predicted-breach alert, mirroring g_last_alert's
// role for the absolute-CRIT path. Holds the last predicted-breach state emitted.
RTC_DATA_ATTR AlertState g_last_predict_alert = ALERT_OK;

// Fridge EMA source: the last PREDICT_SMOOTH_N temp_b (DHT12, fridge-air)
// samples, kept to average out compressor cycling before the projection
// (ADR-011: the predictor smooths the *fridge* signal, temp_b — issue #2 comment).
RTC_DATA_ATTR float    g_fridge_hist[PREDICT_SMOOTH_N];
RTC_DATA_ATTR uint16_t g_fridge_hist_count = 0;   // valid entries held
RTC_DATA_ATTR uint16_t g_fridge_hist_head  = 0;   // next write index (wraps)

// Box EMA source: the last PREDICT_SMOOTH_N temp_a (SHT30, box-interior) samples.
// The box moves slowly, so this mainly buys single-sample glitch rejection at the
// cost of a small lag (an alert may arrive a sample or two later — accepted; see
// issue #3). Mirrors the fridge ring; both smoothed values feed the predictor.
RTC_DATA_ATTR float    g_box_hist[PREDICT_SMOOTH_N];
RTC_DATA_ATTR uint16_t g_box_hist_count = 0;      // valid entries held
RTC_DATA_ATTR uint16_t g_box_hist_head  = 0;      // next write index (wraps)

// Predicted-breach hold-off, in wakes remaining before the predictor may fire
// again. Set to PREDICT_HOLDOFF_SAMPLES on a confirmed send-ack (NOT at dispatch
// — see issue #3: a failed send must not silence the predictor), decremented once
// per wake. A dedicated counter is required because g_wake_count resets on every
// report and so cannot measure a 2 h window that spans reports.
RTC_DATA_ATTR uint16_t g_predict_holdoff = 0;

// Debounce source: the last PREDICT_DEBOUNCE_N raw predictor decisions, so
// predictor.h can require the same side to persist before firing (kills
// door-open transients). ALERT_OK means "no breach predicted that sample".
RTC_DATA_ATTR AlertState g_predict_hist[PREDICT_DEBOUNCE_N];
RTC_DATA_ATTR uint16_t   g_predict_hist_count = 0; // valid entries held
RTC_DATA_ATTR uint16_t   g_predict_hist_head  = 0; // next write index (wraps)

// --- Sensor-fault state (RTC slow memory) — issue #12 -------------------
// Per-sensor detection of silent failures: NaN reads (both), stuck-0.00 (temp_b —
// inherited from the retired STTS22H's cold-power-up default, kept because a
// hard 0.00 from any temp_b part is a fault), and stale readings (both). These
// counters persist across deep sleep so a fault is caught across consecutive
// wakes, and reset to zero on cold boot (a fresh RTC). See helpers.h constants.
//
// Consecutive NaN/stuck run per sensor (temp_a = A, temp_b = B).
RTC_DATA_ATTR uint16_t g_fault_run_a = 0;
RTC_DATA_ATTR uint16_t g_fault_run_b = 0;
// Stale detection: last value (compared to 3 dp) and its unchanged run length.
RTC_DATA_ATTR float    g_stale_last_a = 0.0f;
RTC_DATA_ATTR float    g_stale_last_b = 0.0f;
RTC_DATA_ATTR uint16_t g_stale_run_a  = 0;
RTC_DATA_ATTR uint16_t g_stale_run_b  = 0;
// Hold-off latch: alert fires once on entry into fault, clears on a healthy read
// (mirrors g_last_batt_alert). Prevents re-alerting every wake while faulted.
RTC_DATA_ATTR bool g_fault_latch_a = false;
RTC_DATA_ATTR bool g_fault_latch_b = false;

// --- TEMPORARY DIAGNOSTIC STATE (BOX SENSOR FAULT "no reading") -------------
// Captured in wake_cycle at the moment of the read, carried in RTC memory to
// whichever send happens later (a sampling wake has no radio, so the wake that
// observes a fault is usually NOT the wake that reports it), and rendered by
// with_device_tag(). Remove with the rest of the diagnostic scaffolding.
//
// Discriminator: nan_a=1 with EXTEN=0 means the Grove 5 V boost — temp_a's only
// supply — was down, which is the ADR-026 rail-ownership theory and the one
// mechanism that explains a replacement sensor and a new cable both failing.
// nan_a=1 with EXTEN=1 exonerates the rail and points at the bus or at the
// sht3xd deferred-publish path instead.
RTC_DATA_ATTR int16_t  g_diag_rails    = -1;  // raw reg 0x12, or -1 = read failed
RTC_DATA_ATTR uint8_t  g_diag_nan_a    = 0;   // was temp_a NaN at ring_push time
RTC_DATA_ATTR uint16_t g_diag_nan_wakes = 0;  // cumulative wakes seen with NaN temp_a
RTC_DATA_ATTR int16_t  g_diag_pwr      = -1;  // raw reg 0x00 (input power status), or -1
// Box-read retries (see wake_cycle). Cumulative: a non-zero count means the
// first attempt failed and the retry path ran — the number that says whether
// transient I2C failures are happening at all, and how often, even when the
// retry succeeded and nothing else in the report looks unusual.
RTC_DATA_ATTR uint16_t g_diag_retries  = 0;
// Conditions LATCHED AT THE FIRST NaN WAKE, never overwritten afterwards.
// The g_diag_rails/g_diag_pwr above are rewritten every wake, so a report only
// ever showed the rail state of the SENDING wake — which for a sampling-wake
// failure is minutes later and a different power state entirely. That ambiguity
// is what made the first captured NaN suggestive instead of conclusive.
// -2 = "no NaN wake recorded yet", distinct from -1 = "read failed".
// Awake-watchdog firings. Non-zero means a wake overran its ceiling and had to
// be forced to sleep — i.e. a real hang exists somewhere in the wake paths.
RTC_DATA_ATTR uint16_t g_diag_wdt      = 0;
RTC_DATA_ATTR int16_t  g_nanw_rails    = -2;
RTC_DATA_ATTR int16_t  g_nanw_pwr      = -2;

// Append a sample (overwrites oldest when full).
inline void ring_push(uint32_t epoch, float a, float b) {
  g_ring[g_ring_head] = Sample{epoch, a, b};
  g_ring_head = (g_ring_head + 1) % RING_CAPACITY;
  if (g_ring_count < RING_CAPACITY) g_ring_count++;
}

// Reset after a report has been sent.
inline void ring_clear() {
  g_ring_count = 0;
  g_ring_head = 0;
  g_wake_count = 0;
}

// Most recent sample (the one written last). Only valid when g_ring_count > 0.
inline const Sample &ring_latest() {
  return g_ring[(g_ring_head + RING_CAPACITY - 1) % RING_CAPACITY];
}

// Per-sensor summary over the buffered window: current (most recent) / min /
// max / mean, plus the sample count. `cur_*` are NaN when the buffer is empty.
//
// Also carries (issue #63): the window's epoch bounds for the report's "as of" /
// window line, and per-sensor out-of-range counts for the excursion line. All
// zero when the buffer is empty.
struct Digest {
  float cur_a, min_a, max_a, mean_a;
  float cur_b, min_b, max_b, mean_b;
  uint16_t n;
  uint32_t first_epoch, last_epoch;   // window bounds (0 if no sample had a clock)
  uint16_t n_out_a, n_out_b;          // samples outside each sensor's OK band
};

inline Digest ring_summary() {
  Digest d{};
  d.n = g_ring_count;
  if (g_ring_count == 0) {
    d.cur_a = d.cur_b = std::nanf("");
    return d;
  }
  // SKIP NaN SAMPLES IN THE AGGREGATES, per sensor and independently.
  // Previously min/max seeded from g_ring[0] and compared with </>, which are
  // ALWAYS false against NaN, and the mean summed NaN outright — so a SINGLE
  // failed read turned min/max/mean into NaN for the whole window and the report
  // rendered "-  -  -". Observed in the field: 15 good samples discarded by one
  // bad one, on the sensor that is the compliance authority. The count of good
  // samples is tracked per sensor so one sensor's gap cannot skew the other's
  // mean. `n` still reports samples TAKEN; a NaN remains visible as a failed
  // read via the fault detector, which is where that signal belongs.
  double sa = 0, sb = 0;
  uint16_t na = 0, nb = 0;
  d.min_a = d.max_a = d.mean_a = std::nanf("");
  d.min_b = d.max_b = d.mean_b = std::nanf("");
  for (uint16_t i = 0; i < g_ring_count; i++) {
    float a = g_ring[i].temp_a, b = g_ring[i].temp_b;
    if (!std::isnan(a)) {
      sa += a; na++;
      if (na == 1 || a < d.min_a) d.min_a = a;
      if (na == 1 || a > d.max_a) d.max_a = a;
    }
    if (!std::isnan(b)) {
      sb += b; nb++;
      if (nb == 1 || b < d.min_b) d.min_b = b;
      if (nb == 1 || b > d.max_b) d.max_b = b;
    }
    if (alert_state(a, /*is_box=*/true)  != ALERT_OK) d.n_out_a++;
    if (alert_state(b, /*is_box=*/false) != ALERT_OK) d.n_out_b++;
    // Window bounds by min/max over valid epochs. Index order is NOT time order
    // once the ring wraps, so do not assume g_ring[0] is oldest — compare epochs.
    uint32_t e = g_ring[i].epoch;
    if (e != 0) {
      if (d.first_epoch == 0 || e < d.first_epoch) d.first_epoch = e;
      if (e > d.last_epoch) d.last_epoch = e;
    }
  }
  // Divide by the GOOD-sample count, not g_ring_count: dividing a NaN-free sum
  // by a total that includes failed reads would silently bias the mean low.
  // Stays NaN when a sensor produced no valid sample at all, which is correct —
  // there is no average of nothing, and the fault detector reports the why.
  if (na > 0) d.mean_a = (float)(sa / na);
  if (nb > 0) d.mean_b = (float)(sb / nb);
  const Sample &last = ring_latest();
  d.cur_a = last.temp_a;
  d.cur_b = last.temp_b;
  return d;
}

// Back-compat alias — report.h and existing callers use ring_digest().
inline Digest ring_digest() { return ring_summary(); }

// --- Predictor state accessors ------------------------------------------
// Storage + read helpers only; the firing rule stays in predictor.h (ADR-013).

// Record this wake's fridge reading (temp_b) into the EMA history ring.
inline void fridge_hist_push(float temp_b) {
  g_fridge_hist[g_fridge_hist_head] = temp_b;
  g_fridge_hist_head = (g_fridge_hist_head + 1) % PREDICT_SMOOTH_N;
  if (g_fridge_hist_count < PREDICT_SMOOTH_N) g_fridge_hist_count++;
}

// Exponential moving average of the stored fridge samples, oldest→newest, so the
// most recent reading carries the most weight. NaN entries are skipped; returns
// NaN if nothing valid is held. alpha = 2/(N+1) over the samples on hand.
inline float fridge_ema() {
  if (g_fridge_hist_count == 0) return std::nanf("");
  const float alpha = 2.0f / (PREDICT_SMOOTH_N + 1);
  float ema = std::nanf("");
  // Walk oldest→newest: the oldest valid entry seeds the EMA.
  uint16_t start = (g_fridge_hist_head + PREDICT_SMOOTH_N - g_fridge_hist_count)
                   % PREDICT_SMOOTH_N;
  for (uint16_t i = 0; i < g_fridge_hist_count; i++) {
    float v = g_fridge_hist[(start + i) % PREDICT_SMOOTH_N];
    if (std::isnan(v)) continue;
    ema = std::isnan(ema) ? v : (alpha * v + (1.0f - alpha) * ema);
  }
  return ema;
}

// Record this wake's box reading (temp_a) into the EMA history ring.
inline void box_hist_push(float temp_a) {
  g_box_hist[g_box_hist_head] = temp_a;
  g_box_hist_head = (g_box_hist_head + 1) % PREDICT_SMOOTH_N;
  if (g_box_hist_count < PREDICT_SMOOTH_N) g_box_hist_count++;
}

// Exponential moving average of the stored box samples. Same shape as
// fridge_ema() (oldest→newest, most recent weighted most, NaN skipped, NaN when
// empty); kept separate so the two rings stay independent.
inline float box_ema() {
  if (g_box_hist_count == 0) return std::nanf("");
  const float alpha = 2.0f / (PREDICT_SMOOTH_N + 1);
  float ema = std::nanf("");
  uint16_t start = (g_box_hist_head + PREDICT_SMOOTH_N - g_box_hist_count)
                   % PREDICT_SMOOTH_N;
  for (uint16_t i = 0; i < g_box_hist_count; i++) {
    float v = g_box_hist[(start + i) % PREDICT_SMOOTH_N];
    if (std::isnan(v)) continue;
    ema = std::isnan(ema) ? v : (alpha * v + (1.0f - alpha) * ema);
  }
  return ema;
}

// --- Predicted-breach hold-off ------------------------------------------
// True while the predictor is still within its post-alert quiet window.
inline bool predict_in_holdoff() { return g_predict_holdoff > 0; }

// Decrement the hold-off one step. Call once per wake (a wake == one sample).
inline void predict_holdoff_tick() {
  if (g_predict_holdoff > 0) g_predict_holdoff--;
}

// Arm the hold-off window. Call ONLY on a confirmed send-ack (2xx), so a failed
// send never silences the predictor — see issue #3 send-ack semantics. With the
// once-per-wake tick, arming to N spaces consecutive fires exactly N wakes apart:
// the firing wake is sample 0, the next N−1 wakes are suppressed, and the N-th
// wake (== N × sample_interval later) may fire again.
inline void predict_holdoff_arm() { g_predict_holdoff = PREDICT_HOLDOFF_SAMPLES; }

// --- Send-ack latch arming (issue #14) ----------------------------------
// Each of these is called ONLY from its send script's on_response 2xx branch,
// mirroring predict_holdoff_arm(). They take the alert state as evaluated during
// the wake (the sensor readings do not change between dispatch and the send-ack
// within one wake), so recomputing at ack time is equivalent to latching at
// dispatch — minus the silent-failure hole.

// Record a confirmed 2xx send. Skips epoch 0 (no valid clock) so the report's
// "last send" line never regresses to a bogus 1970 timestamp.
inline void mark_send_ok(uint32_t epoch) { if (epoch != 0) g_last_send_epoch = epoch; }

// CRIT re-alert hold-off (mirrors the old dispatch-time latch). Box CRIT takes
// precedence over fridge CRIT for the stored state, matching the send body's
// box-first ordering. Call only on a confirmed CRIT-alert send.
inline void crit_latch_arm(AlertState box_state, AlertState fridge_state) {
  g_last_alert = is_crit(box_state) ? box_state : fridge_state;
}

// Sensor-fault entry latch, per sensor. Latch only the sensor(s) actually
// faulted at send time, so a sensor that recovers before the send still re-alerts
// on its next fault. Call only on a confirmed fault-alert send.
inline void fault_latch_arm(bool faulted_a, bool faulted_b) {
  if (faulted_a) g_fault_latch_a = true;
  if (faulted_b) g_fault_latch_b = true;
}

// Low-battery once-per-depletion latch. Call only on a confirmed low-batt send.
inline void batt_latch_arm() { g_last_batt_alert = true; }

// Record this wake's raw predictor decision into the debounce history ring.
inline void predict_hist_push(AlertState decision) {
  g_predict_hist[g_predict_hist_head] = decision;
  g_predict_hist_head = (g_predict_hist_head + 1) % PREDICT_DEBOUNCE_N;
  if (g_predict_hist_count < PREDICT_DEBOUNCE_N) g_predict_hist_count++;
}

// True when the full debounce window is held AND every entry equals `side`
// (a non-OK predicted-breach state). predictor.h uses this to gate firing.
inline bool predict_debounced(AlertState side) {
  if (side == ALERT_OK) return false;
  if (g_predict_hist_count < PREDICT_DEBOUNCE_N) return false;
  for (uint16_t i = 0; i < PREDICT_DEBOUNCE_N; i++)
    if (g_predict_hist[i] != side) return false;
  return true;
}

// --- Sensor-fault detection (issue #12) ---------------------------------
// Reason codes for the latched fault, per sensor. 0 = healthy (no fault).
enum FaultReason : uint8_t {
  FAULT_NONE = 0,
  FAULT_NAN,     // no reading (NaN) for FAULT_CONSEC_N wakes
  FAULT_STUCK,   // temp_b pinned at STUCK_TEMP_B for FAULT_CONSEC_N wakes
  FAULT_STALE,   // value unchanged to 3 dp for STALE_SAMPLES_M wakes
};
RTC_DATA_ATTR uint8_t g_fault_reason_a = FAULT_NONE;
RTC_DATA_ATTR uint8_t g_fault_reason_b = FAULT_NONE;

// Round to 3 dp so "unchanged to > 3 dp" (issue #12) is an exact comparison.
inline float round3(float v) { return std::roundf(v * 1000.0f) / 1000.0f; }

// Advance one sensor's fault counters from a fresh reading. `is_b` enables the
// stuck-STUCK_TEMP_B check (temp_b only). Sets *reason to the current latched
// fault (or FAULT_NONE) so sensor_faulted()/fault_reason() can read it. A read
// that is neither a hard fault nor stale resets every run for that sensor.
inline void fault_observe_one(float v, bool is_b, uint16_t *fault_run,
                              float *stale_last, uint16_t *stale_run,
                              uint8_t *reason) {
  bool is_nan   = std::isnan(v);
  bool is_stuck = is_b && !is_nan && v == STUCK_TEMP_B;

  // Hard-fault run (NaN or stuck): counts consecutive bad reads.
  if (is_nan || is_stuck) (*fault_run)++;
  else                    *fault_run = 0;

  // Stale run: consecutive valid reads unchanged to 3 dp. NaN breaks the run
  // (it is a hard fault, tracked above, not a stuck-value case).
  if (is_nan) {
    *stale_run = 0;
  } else if (*stale_run > 0 && round3(v) == round3(*stale_last)) {
    (*stale_run)++;
  } else {
    *stale_run  = 1;   // first sample of a (possibly) new steady value
  }
  if (!is_nan) *stale_last = v;

  // Latch a reason once a threshold is reached; NaN/stuck take priority over
  // stale (a NaN reads as unchanged-to-3dp too, but "no reading" is clearer).
  if (is_nan   && *fault_run >= FAULT_CONSEC_N)  *reason = FAULT_NAN;
  else if (is_stuck && *fault_run >= FAULT_CONSEC_N) *reason = FAULT_STUCK;
  else if (*stale_run >= STALE_SAMPLES_M)         *reason = FAULT_STALE;
  else                                            *reason = FAULT_NONE;
}

// Evaluate both sensors' fault state for this wake. Call once per wake after the
// reads (temp_a = A, temp_b = B), before the CRIT/report paths.
inline void faults_observe(float a, float b) {
  fault_observe_one(a, /*is_b=*/false, &g_fault_run_a, &g_stale_last_a,
                    &g_stale_run_a, &g_fault_reason_a);
  fault_observe_one(b, /*is_b=*/true,  &g_fault_run_b, &g_stale_last_b,
                    &g_stale_run_b, &g_fault_reason_b);
}

// True when the named sensor (is_a → A/temp_a, else B/temp_b) is currently
// faulted. Reflects the most recent faults_observe() call.
inline bool sensor_faulted(bool is_a) {
  return (is_a ? g_fault_reason_a : g_fault_reason_b) != FAULT_NONE;
}

// Human-readable reason for the alert body. "" when the sensor is healthy.
inline const char *fault_reason(bool is_a) {
  switch (is_a ? g_fault_reason_a : g_fault_reason_b) {
    case FAULT_NAN:   return "no reading";
    case FAULT_STUCK: return "stuck 0C";
    case FAULT_STALE: return "stale";
    default:          return "";
  }
}
