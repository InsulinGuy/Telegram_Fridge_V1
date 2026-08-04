// Host-compiled unit tests for logger/predictor.h (issue #3).
//
// Same host trick as test_ring_buffer.cpp: RTC_DATA_ATTR is defined away so the
// RTC-backed rings become ordinary statics. The predictor is pure logic over
// those rings, so a host build exercises it fully — the four canonical scenarios
// from CLAUDE.md §Predictive alert plus the closed-form / EMA units.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/tp test/test_predictor.cpp && /tmp/tp
// (test/run.sh builds and runs this alongside test_ring_buffer.cpp.)

#define RTC_DATA_ATTR   // no-op on host: plain statics, no RTC section

#include "../logger/predictor.h"

#include <cassert>
#include <cmath>
#include <cstdio>

static int g_checks = 0;
static bool near(float a, float b, float eps = 0.5f) {  // minutes: 0.5 is plenty
  g_checks++;
  return std::fabs(a - b) < eps;
}
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

// Reset all predictor-relevant RTC state to a cold-boot baseline.
static void reset_state() {
  g_fridge_hist_count = g_fridge_hist_head = 0;
  g_box_hist_count = g_box_hist_head = 0;
  g_predict_hist_count = g_predict_hist_head = 0;
  g_predict_holdoff = 0;
  g_last_predict_alert = ALERT_OK;
}

// One simulated wake, mirroring the production order in wake_cycle: tick the
// hold-off once per wake, record this wake's readings into both EMA rings, then
// run the predictor step. Returns the side to fire (ALERT_OK = stay quiet).
static AlertState wake(float box, float fridge) {
  predict_holdoff_tick();
  box_hist_push(box);
  fridge_hist_push(fridge);
  return predictor_step();
}

// ---- Unit: ema() -----------------------------------------------------------
static int test_ema() {
  float all_five[] = {5.0f, 5.0f, 5.0f, 5.0f};
  CHECK(near(ema(all_five, 4), 5.0f, 1e-4f));         // constant → constant

  float with_nan[] = {std::nanf(""), 6.0f};
  CHECK(near(ema(with_nan, 2), 6.0f, 1e-4f));         // NaN skipped

  float rising[] = {0.0f, 1.0f, 2.0f, 3.0f};
  double mean = (0 + 1 + 2 + 3) / 4.0;
  CHECK(ema(rising, 4) > mean);                        // recent weighted more
  return 0;
}

// ---- Unit: t_to_threshold() closed form ------------------------------------
static int test_t_to_threshold() {
  const float tau = TAU_BOX_MIN;   // 120 min

  // Steady state: fridge sits between the two box CRIT bounds, box near it →
  // asymptotes short of both, +INF each way.
  CHECK(std::isinf(t_to_threshold(4.0f, 5.0f, THRESH_BOX_CRIT_HIGH, tau)));
  CHECK(std::isinf(t_to_threshold(4.0f, 5.0f, THRESH_BOX_CRIT_LOW,  tau)));

  // Warming: fridge 15, box 5 → crosses 8 at 120·ln(10/7) ≈ 42.8 min.
  CHECK(near(t_to_threshold(15.0f, 5.0f, THRESH_BOX_CRIT_HIGH, tau), 42.77f));
  // ...and never reaches CRIT_LOW while warming → +INF.
  CHECK(std::isinf(t_to_threshold(15.0f, 5.0f, THRESH_BOX_CRIT_LOW, tau)));

  // Cooling: fridge −2, box 5 → crosses 2 at 120·ln(7/4) ≈ 67.1 min.
  CHECK(near(t_to_threshold(-2.0f, 5.0f, THRESH_BOX_CRIT_LOW, tau), 67.14f));
  CHECK(std::isinf(t_to_threshold(-2.0f, 5.0f, THRESH_BOX_CRIT_HIGH, tau)));

  // NaN in → +INF (never alert on a missing reading).
  CHECK(std::isinf(t_to_threshold(std::nanf(""), 5.0f, 8.0f, tau)));
  return 0;
}

// ---- Scenario 1: steady state → never fires --------------------------------
static int test_scenario_steady() {
  reset_state();
  // Box holds ~5 °C; fridge cycles 3.5–4.5 with the compressor. Fridge stays
  // between the box CRIT bounds, so the box asymptotes safely — no projection.
  const float fridge_cycle[] = {4.0f, 3.6f, 4.4f, 3.7f, 4.3f, 4.0f, 3.8f, 4.2f};
  for (int i = 0; i < 24; i++) {
    AlertState s = wake(5.0f, fridge_cycle[i % 8]);
    CHECK(s == ALERT_OK);
  }
  return 0;
}

// ---- Scenario 2: fridge dies (warm failure) → fires within horizon ---------
static int test_scenario_fridge_dies() {
  reset_state();
  // Warm up to a healthy steady state first.
  for (int i = 0; i < 4; i++) CHECK(wake(5.0f, 4.0f) == ALERT_OK);

  // Compressor dies: fridge air climbs toward room temp; the box lags behind.
  const float fridge_up[] = {8.0f, 11.0f, 14.0f, 16.0f, 18.0f};
  const float box_up[]    = {5.1f,  5.3f,  5.6f,  6.0f,  6.5f};  // still < 8
  bool fired = false;
  for (int i = 0; i < 5 && !fired; i++) {
    AlertState s = wake(box_up[i], fridge_up[i]);
    if (s == ALERT_PREDICTED_BREACH_HIGH) {
      fired = true;
      CHECK(box_up[i] < THRESH_BOX_CRIT_HIGH);        // fired BEFORE box CRIT
      CHECK(predictor_t_min() < (float)PREDICT_HORIZON);
    } else {
      CHECK(s == ALERT_OK);                            // never the wrong side
    }
  }
  CHECK(fired);
  return 0;
}

// ---- Scenario 3: door-open transient → does NOT fire (debounce eats it) -----
static int test_scenario_door_open() {
  reset_state();
  for (int i = 0; i < 4; i++) CHECK(wake(5.0f, 4.0f) == ALERT_OK);

  // A single wake sees room air (door open) then it shuts and recovers. Even if
  // the spike sample votes "breach", the very next sample votes OK, so the
  // PREDICT_DEBOUNCE_N=2 run never completes → no alert.
  CHECK(wake(5.0f, 22.0f) == ALERT_OK);   // spike (at most 1 breach vote)
  CHECK(wake(5.0f, 4.0f)  == ALERT_OK);   // recovered → debounce broken
  CHECK(wake(5.0f, 4.0f)  == ALERT_OK);
  CHECK(wake(5.0f, 4.0f)  == ALERT_OK);
  return 0;
}

// ---- Scenario 4: slow drift up → fires ahead of box CRIT -------------------
static int test_scenario_slow_drift() {
  reset_state();
  for (int i = 0; i < 4; i++) CHECK(wake(5.0f, 4.0f) == ALERT_OK);

  // Fridge drifts up over hours (thermostat creeping); the box follows slowly.
  const float fridge[] = {5.0f, 6.5f, 8.0f, 9.5f, 11.0f, 12.5f, 14.0f};
  const float box[]    = {5.1f, 5.3f, 5.6f, 6.0f,  6.4f,  6.9f,  7.4f}; // < 8
  bool fired = false;
  for (int i = 0; i < 7 && !fired; i++) {
    AlertState s = wake(box[i], fridge[i]);
    if (s == ALERT_PREDICTED_BREACH_HIGH) {
      fired = true;
      CHECK(box[i] < THRESH_BOX_CRIT_HIGH);           // warned before CRIT
    } else {
      CHECK(s == ALERT_OK);
    }
  }
  CHECK(fired);
  return 0;
}

// ---- Scenario 4b: slow drift down → fires LOW ahead of box CRIT ------------
static int test_scenario_slow_drift_low() {
  reset_state();
  for (int i = 0; i < 4; i++) CHECK(wake(5.0f, 4.0f) == ALERT_OK);

  const float fridge[] = {2.0f, 0.0f, -2.0f, -4.0f, -6.0f, -8.0f};
  const float box[]    = {4.8f, 4.5f,  4.1f,  3.7f,  3.3f,  2.9f}; // > 2
  bool fired = false;
  for (int i = 0; i < 6 && !fired; i++) {
    AlertState s = wake(box[i], fridge[i]);
    if (s == ALERT_PREDICTED_BREACH_LOW) {
      fired = true;
      CHECK(box[i] > THRESH_BOX_CRIT_LOW);
    } else {
      CHECK(s == ALERT_OK);
    }
  }
  CHECK(fired);
  return 0;
}

// ---- Hold-off: after a fire + send-ack, stays quiet for the window ----------
static int test_holdoff() {
  reset_state();
  for (int i = 0; i < 4; i++) wake(5.0f, 4.0f);

  // Drive a sustained warm failure until it fires, then simulate the send-ack.
  AlertState first = ALERT_OK;
  for (int i = 0; i < 6 && first == ALERT_OK; i++) first = wake(6.0f, 16.0f);
  CHECK(first == ALERT_PREDICTED_BREACH_HIGH);
  predict_holdoff_arm();                          // send-ack arms the hold-off
  CHECK(g_predict_holdoff == PREDICT_HOLDOFF_SAMPLES);

  // The condition persists, but consecutive fires are spaced PREDICT_HOLDOFF_SAMPLES
  // wakes apart: the firing wake is sample 0, then the next N−1 wakes are quiet…
  for (int i = 0; i < PREDICT_HOLDOFF_SAMPLES - 1; i++)
    CHECK(wake(6.0f, 16.0f) == ALERT_OK);

  // …and the N-th wake after the fire (== 2 h later) may fire again.
  CHECK(wake(6.0f, 16.0f) == ALERT_PREDICTED_BREACH_HIGH);
  return 0;
}

int main() {
  int rc = 0;
  rc |= test_ema();
  rc |= test_t_to_threshold();
  rc |= test_scenario_steady();
  rc |= test_scenario_fridge_dies();
  rc |= test_scenario_door_open();
  rc |= test_scenario_slow_drift();
  rc |= test_scenario_slow_drift_low();
  rc |= test_holdoff();
  if (rc == 0) std::printf("OK  predictor: all %d checks passed\n", g_checks);
  return rc;
}
