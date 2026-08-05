// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
#pragma once
// TeleFridge V1 — re-alert while still in CRIT: the #65 time reminder and the
// #18 worsening escalation, sharing one decision point (ADR-018 / ADR-019).
//
// Both answer the same question a wake asks once we are already inside a CRIT
// episode: "do we speak again this wake?" — with two triggers:
//   • REMINDER (#65)   — crit_realert_holdoff_min minutes have elapsed since the
//                        last CRIT send (0 = off, today's single-shot behaviour).
//   • ESCALATION (#18) — the box has worsened by >= ESCALATE_DELTA_C beyond the
//                        value that last alerted (always on, capped per hour).
// Escalation takes precedence over the reminder on the same wake, and re-arms the
// reminder clock so a further worsening still has room to escalate.
//
// Per ADR-018 every clock here advances ONLY on a confirmed 2xx send: the wake
// decides (a pure read of the RTC state in ring_buffer.h), the send script's
// on_response 2xx branch calls the arm_/begin_ helpers. A failed send leaves the
// state untouched, so the next wake re-evaluates and retries. The one exception is
// realert_tick(), the once-per-wake elapsed-time advance (mirrors
// predict_holdoff_tick()) — ticking down is measuring time, not arming a hold-off.
//
// This header needs g_settings (the reminder interval + sample cadence), so it
// includes settings.h and predictor.h; the RTC globals it drives live in
// ring_buffer.h. Include it in the YAML AFTER settings.h + predictor.h.

#include <cmath>
#include "helpers.h"
#include "ring_buffer.h"
#include "settings.h"
#include "predictor.h"

// Why a still-critical episode re-sends this wake. NONE = stay quiet. ENTRY is not
// returned by the decision — it is passed to the arm path for the first alert of
// an episode (the existing box|fridge CRIT-entry dispatch), so one send script can
// serve entry, reminder and escalation.
enum ReAlertReason {
  REALERT_NONE = 0,
  REALERT_REMINDER,
  REALERT_ESCALATION,
  REALERT_ENTRY,
};

// Convert a minutes interval into whole wakes at the current sample cadence, so
// the reminder/rate-limit clocks measure real time without needing a valid SNTP
// clock on a sampling wake (only radio wakes sync). Rounds up; never returns 0, so
// even a sub-interval setting fires no sooner than the next wake.
inline uint16_t realert_wakes_for_min(uint16_t minutes) {
  uint16_t si = g_settings.sample_interval_min;
  if (si == 0) si = 1;
  uint32_t w = ((uint32_t)minutes + si - 1) / si;
  return (uint16_t)(w == 0 ? 1 : w);
}

// Once per wake (call alongside predict_holdoff_tick): advance the reminder
// countdown and the escalation rate-limit window; refill the hourly budget when
// the window elapses.
inline void realert_tick() {
  if (g_crit_realert_ticks > 0) g_crit_realert_ticks--;
  if (g_escal_window_ticks > 0) g_escal_window_ticks--;
  else                          g_escal_count = 0;   // window elapsed → budget refilled
}

// Is there escalation budget left in the current hour (shared by both sources)?
inline bool escal_budget_ok() { return g_escal_count < ESCALATE_MAX_PER_HOUR; }

// Spend one escalation from the hourly budget, opening the window if idle.
inline void escal_budget_spend() {
  if (g_escal_window_ticks == 0) g_escal_window_ticks = realert_wakes_for_min(60);
  if (g_escal_count < 0xFF) g_escal_count++;
}

// --- Absolute box-CRIT re-alert (temp_a) --------------------------------------

// Decide the in-zone re-send reason for the box. Pure read; state is advanced only
// by the arm/begin helpers on send-ack. `box_state` is the live box classification
// (alert_state_live(temp_a, true)); `temp_a` the current box reading. Returns NONE
// unless the box is in CRIT.
inline ReAlertReason crit_realert_decision(AlertState box_state, float temp_a) {
  if (!is_crit(box_state)) return REALERT_NONE;
  // Escalation (#18) first — needs an established baseline and spare budget.
  if (!std::isnan(g_crit_alert_value) && escal_budget_ok()) {
    bool worse =
      (box_state == ALERT_CRIT_HIGH && temp_a >= g_crit_alert_value + ESCALATE_DELTA_C) ||
      (box_state == ALERT_CRIT_LOW  && temp_a <= g_crit_alert_value - ESCALATE_DELTA_C);
    if (worse) return REALERT_ESCALATION;
  }
  // Reminder (#65) — enabled and the interval elapsed.
  if (g_settings.crit_realert_holdoff_min > 0 && g_crit_realert_ticks == 0)
    return REALERT_REMINDER;
  return REALERT_NONE;
}

// Begin a box-CRIT episode on the ENTRY alert's send-ack. Sets the escalation
// baseline to temp_a when the box itself is the critical sensor (box_crit); a
// fridge-only entry leaves the baseline unset (NaN) until the box goes critical.
// Arms the reminder countdown either way.
inline void crit_episode_begin(bool box_crit, float temp_a) {
  g_crit_alert_value   = box_crit ? temp_a : std::nanf("");
  g_crit_realert_ticks = realert_wakes_for_min(g_settings.crit_realert_holdoff_min);
}

// Advance the episode on an in-zone re-send's send-ack (reminder or escalation).
// Both re-arm the reminder countdown; escalation also moves the baseline to the
// new worse value and spends budget. A reminder lazily seeds the baseline if a
// fridge-first entry never did, so a later worsening can still escalate.
inline void crit_realert_arm(ReAlertReason reason, float temp_a) {
  g_crit_realert_ticks = realert_wakes_for_min(g_settings.crit_realert_holdoff_min);
  if (reason == REALERT_ESCALATION) {
    g_crit_alert_value = temp_a;
    escal_budget_spend();
  } else if (reason == REALERT_REMINDER && std::isnan(g_crit_alert_value)) {
    g_crit_alert_value = temp_a;
  }
}

// Reset the box episode when the box leaves CRIT (recovery). Leaves the shared
// escalation budget window running (it self-refills) and the predicted-breach
// baseline alone (that episode is the predictor's).
inline void crit_episode_reset() {
  g_crit_realert_ticks = 0;
  g_crit_alert_value   = std::nanf("");
}

// --- Predicted-breach escalation (#18) ----------------------------------------
// The predictor's own hold-off (g_predict_holdoff) is the quiet window after a
// predicted-breach alert. Escalate within it when the projected ETA has collapsed
// to <= ESCALATE_PREDICT_FRAC of the ETA that last alerted, on the same side.

// Record the ETA that last alerted (call on a normal predicted-breach send-ack,
// beside predict_holdoff_arm()).
inline void predict_alert_record(float t_min) { g_predict_alert_tmin = t_min; }

// Side to escalate this wake, or ALERT_OK. Only fires inside the predictor
// hold-off (a fresh predictor_step() fire is the normal path) and with budget.
inline AlertState predict_escalation_side() {
  if (!predict_in_holdoff() || !escal_budget_ok()) return ALERT_OK;
  if (!std::isfinite(g_predict_alert_tmin)) return ALERT_OK;
  const PredictorResult r =
      predictor_evaluate(box_ema(), fridge_ema(), TAU_BOX_MIN);
  if (r.side == ALERT_OK || !std::isfinite(r.t_min)) return ALERT_OK;
  if (r.side == g_last_predict_alert &&
      r.t_min <= ESCALATE_PREDICT_FRAC * g_predict_alert_tmin)
    return r.side;
  return ALERT_OK;
}

// Advance the predicted-breach episode on an escalation send-ack: move the ETA
// baseline, re-arm the predictor hold-off, spend budget.
inline void predict_escalation_arm(float t_min) {
  g_predict_alert_tmin = t_min;
  predict_holdoff_arm();
  escal_budget_spend();
}

// --- Message prefix -----------------------------------------------------------
// Prepended to build_crit_alert()/build_predict_alert() bodies. Balanced,
// whitelisted HTML (the report host test asserts tag balance). "" for ENTRY/NONE.
inline const char *realert_prefix(ReAlertReason reason) {
  switch (reason) {
    case REALERT_ESCALATION:
      return "\xF0\x9F\x94\xBA <b>ESCALATION \xE2\x80\x94 getting worse</b>\n";  // 🔺 —
    case REALERT_REMINDER:
      return "\xF0\x9F\x94\x81 <b>still critical (reminder)</b>\n";              // 🔁
    default:
      return "";
  }
}
