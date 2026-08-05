// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
// Host-compiled unit tests for logger/ring_buffer.h (issue #2).
//
// The header targets esp-idf, where RTC_DATA_ATTR places symbols in RTC slow
// memory. On the host that attribute has no meaning, so we define it away and
// the buffers become ordinary statics — enough to exercise push/clear/summary
// and the predictor-state rings, which is all this test cares about. RTC
// persistence across a soft reset is an esp-idf property verified on hardware,
// not something a host build can (or needs to) reproduce.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/trb test/test_ring_buffer.cpp && /tmp/trb

#define RTC_DATA_ATTR   // no-op on host: plain statics, no RTC section

#include "../logger/ring_buffer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

static int g_checks = 0;
static bool near(float a, float b, float eps = 1e-4f) {
  g_checks++;
  return std::fabs(a - b) < eps;
}
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

// Reset all RTC-backed state to a known baseline between test cases (a cold
// boot zeroes these on real hardware).
static void reset_state() {
  ring_clear();
  g_last_alert = ALERT_OK;
  g_last_predict_alert = ALERT_OK;
  g_fridge_hist_count = g_fridge_hist_head = 0;
  g_box_hist_count = g_box_hist_head = 0;
  g_predict_hist_count = g_predict_hist_head = 0;
  g_predict_holdoff = 0;
  g_fault_run_a = g_fault_run_b = 0;
  g_stale_run_a = g_stale_run_b = 0;
  g_stale_last_a = g_stale_last_b = 0.0f;
  g_fault_reason_a = g_fault_reason_b = FAULT_NONE;
  g_fault_latch_a = g_fault_latch_b = false;
  g_last_batt_alert = false;
  g_last_send_epoch = 0;
}

static int test_empty_summary() {
  reset_state();
  Digest d = ring_summary();
  CHECK(d.n == 0);
  CHECK(std::isnan(d.cur_a));
  CHECK(std::isnan(d.cur_b));
  return 0;
}

static int test_push_and_summary() {
  reset_state();
  ring_push(100, 4.0f, 5.0f);
  ring_push(200, 6.0f, 3.0f);
  ring_push(300, 2.0f, 7.0f);
  Digest d = ring_summary();
  CHECK(d.n == 3);
  // current == most recent push
  CHECK(near(d.cur_a, 2.0f));
  CHECK(near(d.cur_b, 7.0f));
  CHECK(near(d.min_a, 2.0f));
  CHECK(near(d.max_a, 6.0f));
  CHECK(near(d.mean_a, (4.0f + 6.0f + 2.0f) / 3.0f));
  CHECK(near(d.min_b, 3.0f));
  CHECK(near(d.max_b, 7.0f));
  CHECK(near(d.mean_b, (5.0f + 3.0f + 7.0f) / 3.0f));
  // ring_digest() is a back-compat alias for ring_summary()
  Digest d2 = ring_digest();
  CHECK(d2.n == d.n && near(d2.cur_a, d.cur_a));
  return 0;
}

static int test_clear() {
  reset_state();
  ring_push(1, 5.0f, 5.0f);
  ring_push(2, 6.0f, 6.0f);
  CHECK(g_ring_count == 2 && g_wake_count == 0);
  g_wake_count = 9;
  ring_clear();
  CHECK(g_ring_count == 0);
  CHECK(g_ring_head == 0);
  CHECK(g_wake_count == 0);
  CHECK(ring_summary().n == 0);
  return 0;
}

static int test_wraparound() {
  reset_state();
  // Overfill: push CAPACITY + 5, so the 5 oldest are overwritten.
  for (int i = 0; i < RING_CAPACITY + 5; i++)
    ring_push(i, (float)i, (float)(i + 100));
  Digest d = ring_summary();
  CHECK(d.n == RING_CAPACITY);           // count saturates at capacity
  const int last = RING_CAPACITY + 5 - 1;
  CHECK(near(d.cur_a, (float)last));      // current is the newest value
  CHECK(near(d.cur_b, (float)(last + 100)));
  CHECK(near(d.max_a, (float)last));      // oldest (0..4) were evicted
  CHECK(near(d.min_a, (float)(last - RING_CAPACITY + 1))); // = 5
  return 0;
}

static int test_fridge_ema() {
  reset_state();
  CHECK(std::isnan(fridge_ema()));        // empty -> NaN
  // Constant input -> EMA equals that constant regardless of window fill.
  for (int i = 0; i < PREDICT_SMOOTH_N; i++) fridge_hist_push(4.0f);
  CHECK(g_fridge_hist_count == PREDICT_SMOOTH_N);
  CHECK(near(fridge_ema(), 4.0f));
  // A NaN sample is skipped, not propagated.
  reset_state();
  fridge_hist_push(std::nanf(""));
  fridge_hist_push(6.0f);
  CHECK(near(fridge_ema(), 6.0f));
  // Newest reading is weighted most: rising ramp -> EMA above the mean.
  reset_state();
  for (int i = 0; i < PREDICT_SMOOTH_N; i++) fridge_hist_push((float)i);
  float mean = (PREDICT_SMOOTH_N - 1) / 2.0f;
  CHECK(fridge_ema() > mean);
  return 0;
}

static int test_fridge_hist_wrap() {
  reset_state();
  // Overwrite the whole window with a constant; head/count stay bounded.
  for (int i = 0; i < PREDICT_SMOOTH_N * 3; i++) fridge_hist_push(7.5f);
  CHECK(g_fridge_hist_count == PREDICT_SMOOTH_N);
  CHECK(near(fridge_ema(), 7.5f));
  return 0;
}

static int test_predict_debounce() {
  reset_state();
  CHECK(!predict_debounced(ALERT_OK));                 // OK never debounces
  CHECK(!predict_debounced(ALERT_PREDICTED_BREACH_HIGH)); // empty history
  // A partial window does not satisfy the debounce.
  predict_hist_push(ALERT_PREDICTED_BREACH_HIGH);
  if (PREDICT_DEBOUNCE_N > 1)
    CHECK(!predict_debounced(ALERT_PREDICTED_BREACH_HIGH));
  // Fill the window with the same side -> fires.
  reset_state();
  for (int i = 0; i < PREDICT_DEBOUNCE_N; i++)
    predict_hist_push(ALERT_PREDICTED_BREACH_HIGH);
  CHECK(predict_debounced(ALERT_PREDICTED_BREACH_HIGH));
  CHECK(!predict_debounced(ALERT_PREDICTED_BREACH_LOW)); // wrong side
  // A single OK breaks the streak.
  predict_hist_push(ALERT_OK);
  CHECK(!predict_debounced(ALERT_PREDICTED_BREACH_HIGH));
  return 0;
}

// --- Sensor-fault detection (issue #12) ---------------------------------

static int test_fault_nan() {
  reset_state();
  const float ok = 4.0f;
  // One NaN on A is not yet a fault (needs FAULT_CONSEC_N consecutive).
  faults_observe(std::nanf(""), ok);
  CHECK(FAULT_CONSEC_N < 2 || !sensor_faulted(true));
  // FAULT_CONSEC_N consecutive NaN on A -> fault, reason "no reading".
  reset_state();
  for (int i = 0; i < FAULT_CONSEC_N; i++) faults_observe(std::nanf(""), ok);
  CHECK(sensor_faulted(true));
  CHECK(!sensor_faulted(false));
  CHECK(std::strcmp(fault_reason(true), "no reading") == 0);
  // A valid read clears the fault and resets the run.
  faults_observe(ok, ok);
  CHECK(!sensor_faulted(true));
  CHECK(g_fault_run_a == 0);
  return 0;
}

static int test_fault_nan_run_resets() {
  reset_state();
  const float ok = 4.0f;
  // NaN, then a valid read, then NaN again must NOT trip (run reset between).
  faults_observe(std::nanf(""), ok);
  faults_observe(ok, ok);
  faults_observe(std::nanf(""), ok);
  CHECK(FAULT_CONSEC_N < 2 || !sensor_faulted(true));
  return 0;
}

static int test_fault_stuck_b() {
  reset_state();
  const float ok = 4.0f;
  // temp_b pinned at STUCK_TEMP_B (0.00) for FAULT_CONSEC_N wakes -> stuck fault.
  for (int i = 0; i < FAULT_CONSEC_N; i++) faults_observe(ok, STUCK_TEMP_B);
  CHECK(sensor_faulted(false));
  CHECK(std::strcmp(fault_reason(false), "stuck 0C") == 0);
  // A valid B read clears it.
  faults_observe(ok, 3.2f);
  CHECK(!sensor_faulted(false));
  // The same 0.00 on A (temp_a) is NOT a stuck fault — stuck check is temp_b only.
  reset_state();
  for (int i = 0; i < FAULT_CONSEC_N; i++) faults_observe(0.0f, ok);
  CHECK(!sensor_faulted(true) || std::strcmp(fault_reason(true), "stuck 0C") != 0);
  return 0;
}

static int test_fault_stale() {
  reset_state();
  const float ok_b = 4.0f;
  // Same A value to 3 dp for STALE_SAMPLES_M reads -> stale.
  for (int i = 0; i < STALE_SAMPLES_M; i++) faults_observe(5.123f, ok_b + i * 0.5f);
  CHECK(sensor_faulted(true));
  CHECK(std::strcmp(fault_reason(true), "stale") == 0);
  // A changed A read (beyond 3 dp) clears the stale run.
  faults_observe(5.130f, ok_b);
  CHECK(!sensor_faulted(true));
  CHECK(g_stale_run_a == 1);
  // Sub-3dp jitter still counts as unchanged (rounds to the same value).
  reset_state();
  for (int i = 0; i < STALE_SAMPLES_M; i++)
    faults_observe(5.1231f + (i % 2) * 0.0003f, ok_b);  // 5.1231/5.1234 -> 5.123
  CHECK(sensor_faulted(true));
  return 0;
}

static int test_fault_latch() {
  reset_state();
  const float ok = 4.0f;
  // Drive A into a NaN fault; latch mimics the YAML on-entry/clear-on-heal rule.
  for (int i = 0; i < FAULT_CONSEC_N; i++) faults_observe(std::nanf(""), ok);
  CHECK(sensor_faulted(true) && !g_fault_latch_a);   // entry: not yet latched
  g_fault_latch_a = true;                            // YAML sets the latch, sends
  faults_observe(std::nanf(""), ok);                 // still faulted, still latched
  CHECK(sensor_faulted(true) && g_fault_latch_a);    // no re-alert
  faults_observe(ok, ok);                            // healthy read
  CHECK(!sensor_faulted(true));
  g_fault_latch_a = false;                           // YAML clears on heal -> re-arm
  CHECK(!g_fault_latch_a);
  return 0;
}

// Issue #14: hold-off latches advance on send-ack, never at dispatch. These
// exercise the arm_*() helpers the send scripts call from on_response 2xx.
static int test_send_ack_latches() {
  // CRIT: box-first precedence; only a confirmed send arms g_last_alert.
  reset_state();
  CHECK(g_last_alert == ALERT_OK);                     // dispatch alone: unarmed
  crit_latch_arm(ALERT_CRIT_HIGH, ALERT_OK);           // box crit -> box wins
  CHECK(g_last_alert == ALERT_CRIT_HIGH);
  reset_state();
  crit_latch_arm(ALERT_OK, ALERT_CRIT_LOW);            // fridge-only crit
  CHECK(g_last_alert == ALERT_CRIT_LOW);

  // Fault: latch only the sensor(s) faulted at send time.
  reset_state();
  fault_latch_arm(true, false);
  CHECK(g_fault_latch_a && !g_fault_latch_b);
  fault_latch_arm(false, true);                        // additive; a still latched
  CHECK(g_fault_latch_a && g_fault_latch_b);

  // Battery: once-per-depletion latch.
  reset_state();
  CHECK(!g_last_batt_alert);
  batt_latch_arm();
  CHECK(g_last_batt_alert);

  // mark_send_ok: records a valid epoch, ignores epoch 0 (no clock).
  reset_state();
  CHECK(g_last_send_epoch == 0);
  mark_send_ok(0);                                     // pre-SNTP: no regression
  CHECK(g_last_send_epoch == 0);
  mark_send_ok(1700000000u);
  CHECK(g_last_send_epoch == 1700000000u);
  mark_send_ok(0);                                     // later failure to read clock
  CHECK(g_last_send_epoch == 1700000000u);             // keeps last good
  return 0;
}

int main() {
  int rc = 0;
  rc |= test_empty_summary();
  rc |= test_send_ack_latches();
  rc |= test_push_and_summary();
  rc |= test_clear();
  rc |= test_wraparound();
  rc |= test_fridge_ema();
  rc |= test_fridge_hist_wrap();
  rc |= test_predict_debounce();
  rc |= test_fault_nan();
  rc |= test_fault_nan_run_resets();
  rc |= test_fault_stuck_b();
  rc |= test_fault_stale();
  rc |= test_fault_latch();
  if (rc == 0)
    std::printf("OK  ring_buffer: all %d checks passed\n", g_checks);
  return rc;
}
