#pragma once
// TeleFridge V1 — Newton's-law-of-cooling predictor (ADR-013, issue #3).
//
// The box is a lumped thermal mass exchanging heat with the fridge air, so with
// T_box = temp_a (box interior, SHT40) and T_fridge = temp_b (fridge air, STTS22H)
// Newton's law gives  T_box(t) = T_fridge − (T_fridge − T_box)·exp(−t/τ_box).
// Solving for the time until T_box crosses a box CRIT bound T_thresh:
//
//     t_to_threshold = τ_box · ln( (T_fridge − T_box) / (T_fridge − T_thresh) )
//
// The log argument is > 1 only when the box is genuinely heading toward that
// bound; ≤ 1 means it asymptotes short of it (steady state) → +∞, no alert.
//
// Firing: on every wake, smooth BOTH signals (EMA over PREDICT_SMOOTH_N samples —
// the fridge EMA tames compressor cycling, the box EMA rejects single-sample
// glitches), project the nearest bound, and emit ALERT_PREDICTED_BREACH_{HIGH,LOW}
// when the projection is inside PREDICT_HORIZON, the same side has persisted for
// PREDICT_DEBOUNCE_N samples, and we are not inside the post-alert hold-off.
//
// This header holds the PURE logic + the per-wake step. The RTC-memory state it
// reads/advances (the two EMA rings, the debounce ring, the hold-off counter)
// lives in ring_buffer.h; the Wi-Fi send + hold-off arming are wired in the YAML.

#include <cmath>
#include "helpers.h"
#include "ring_buffer.h"

// Arithmetic exponential moving average over an ordered array (oldest→newest, so
// the most recent sample carries the most weight). NaN entries are skipped;
// returns NaN if nothing valid is held. alpha = 2/(PREDICT_SMOOTH_N+1), matching
// the ring-aware fridge_ema()/box_ema() in ring_buffer.h (which are the wrappers
// production uses; this free function is the same math on a plain array, used by
// the host tests and callers that already hold an ordered window).
inline float ema(const float *history, int n) {
  const float alpha = 2.0f / (PREDICT_SMOOTH_N + 1);
  float acc = std::nanf("");
  for (int i = 0; i < n; i++) {
    float v = history[i];
    if (std::isnan(v)) continue;
    acc = std::isnan(acc) ? v : (alpha * v + (1.0f - alpha) * acc);
  }
  return acc;
}

// Minutes until the box (t_box) reaches t_thresh given the fridge air (t_fridge)
// and the box time constant tau (minutes). Returns +INFINITY when the box never
// reaches it (log arg ≤ 1 — steady state / wrong direction) or on a NaN input.
// Both temperature args are expected to be the SMOOTHED values.
inline float t_to_threshold(float t_fridge, float t_box, float t_thresh, float tau) {
  if (std::isnan(t_fridge) || std::isnan(t_box)) return INFINITY;
  const float num = t_fridge - t_box;      // signed distance fridge is "pulling"
  const float den = t_fridge - t_thresh;   // fridge headroom past the threshold
  if (den == 0.0f) return INFINITY;
  const float arg = num / den;
  if (!(arg > 1.0f)) return INFINITY;      // ≤1, negative, or NaN → never crosses
  return tau * std::log(arg);
}

// One predictor evaluation: which box CRIT bound (if any) is projected to be hit,
// and in how many minutes. side is ALERT_OK when neither bound is reachable.
struct PredictorResult {
  AlertState side;   // ALERT_OK / ALERT_PREDICTED_BREACH_HIGH / ..._LOW
  float t_min;       // minutes to that bound (INFINITY when side == ALERT_OK)
};

// Evaluate both box CRIT bounds from the smoothed box/fridge temps and keep the
// nearer finite one. Only one can be finite in normal operation: a warming box
// (fridge above box) can only be projected onto CRIT_HIGH, a cooling box onto
// CRIT_LOW; the losing side returns +INFINITY from t_to_threshold by construction.
inline PredictorResult predictor_evaluate(float box_sm, float fridge_sm, float tau) {
  PredictorResult r{ALERT_OK, INFINITY};
  if (std::isnan(box_sm) || std::isnan(fridge_sm)) return r;
  const float t_high = t_to_threshold(fridge_sm, box_sm, THRESH_BOX_CRIT_HIGH, tau);
  const float t_low  = t_to_threshold(fridge_sm, box_sm, THRESH_BOX_CRIT_LOW,  tau);
  if (t_high < t_low)       { r.side = ALERT_PREDICTED_BREACH_HIGH; r.t_min = t_high; }
  else if (t_low  < t_high) { r.side = ALERT_PREDICTED_BREACH_LOW;  r.t_min = t_low;  }
  return r;   // both INFINITY (steady state) → OK / INFINITY
}

// Minutes-to-limit for the CURRENT smoothed state, for the alert/report body.
// INFINITY when steady (render as "steady"). Uses the same inputs as the step.
inline float predictor_t_min() {
  return predictor_evaluate(box_ema(), fridge_ema(), TAU_BOX_MIN).t_min;
}

// Per-wake predictor step. Call ONCE per wake AFTER box_hist_push(temp_a) and
// fridge_hist_push(temp_b) have recorded this wake's readings, so the EMAs include
// the current sample. Records this sample's debounce vote internally and returns
// the side to FIRE this wake, or ALERT_OK to stay quiet. On a non-OK return the
// caller enables Wi-Fi, sends the predicted-breach alert, and — only on a
// confirmed send-ack — calls predict_holdoff_arm() (issue #3: a failed send must
// not silence the predictor). Does NOT clear the ring buffer (like the CRIT path).
inline AlertState predictor_step() {
  const PredictorResult r =
      predictor_evaluate(box_ema(), fridge_ema(), TAU_BOX_MIN);

  // A breach is "seen" this sample only if it is projected within the warning
  // horizon; otherwise this sample votes OK, which breaks any in-progress debounce
  // run (a transient that recovers is eaten here). Then apply debounce + hold-off.
  const AlertState decision =
      (r.side != ALERT_OK && r.t_min < (float)PREDICT_HORIZON) ? r.side : ALERT_OK;
  predict_hist_push(decision);

  if (decision == ALERT_OK)          return ALERT_OK;  // nothing imminent
  if (!predict_debounced(decision))  return ALERT_OK;  // needs N consecutive
  if (predict_in_holdoff())          return ALERT_OK;  // still within quiet window
  return decision;
}
