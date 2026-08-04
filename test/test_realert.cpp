// Host-compiled unit tests for logger/realert.h — the #65 CRIT re-alert reminder
// and the #18 worsening escalation (ADR-019), both riding the ADR-018 send-ack
// discipline. Same host trick as the sibling suites: RTC_DATA_ATTR is defined away
// so the RTC-backed episode state becomes ordinary statics and the pure decision /
// arm helpers are exercised directly.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/trl test/test_realert.cpp && /tmp/trl
// (test/run.sh builds and runs this alongside the other suites.)

#define RTC_DATA_ATTR   // no-op on host: plain statics, no RTC section

#include "../logger/realert.h"

#include <cassert>
#include <cmath>
#include <cstdio>

static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

// Reset all episode state + settings to a known baseline (a cold boot zeroes RTC).
static void reset(uint16_t holdoff_min = 0, uint16_t sample_min = 15) {
  g_settings = settings_defaults();
  g_settings.sample_interval_min   = sample_min;
  g_settings.crit_realert_holdoff_min = holdoff_min;
  g_crit_realert_ticks = 0;
  g_crit_alert_value   = std::nanf("");
  g_predict_alert_tmin = INFINITY;
  g_escal_window_ticks = 0;
  g_escal_count        = 0;
  g_last_predict_alert = ALERT_OK;
  g_predict_holdoff    = 0;
  g_fridge_hist_count = g_fridge_hist_head = 0;
  g_box_hist_count = g_box_hist_head = 0;
}

int main() {
  // --- 1. wakes_for_min rounds up, never zero -------------------------------
  reset(0, 15);
  CHECK(realert_wakes_for_min(30) == 2);
  CHECK(realert_wakes_for_min(31) == 3);   // rounds up
  CHECK(realert_wakes_for_min(1)  == 1);   // sub-interval -> next wake
  CHECK(realert_wakes_for_min(0)  == 1);   // never 0

  // --- 2. Reminders OFF by default (holdoff_min = 0) — today's behaviour -----
  {
    reset(/*holdoff*/0);
    crit_episode_begin(/*box_crit*/true, 8.4f);      // entry at 8.4 °C
    CHECK(std::isnan(g_crit_alert_value) == false && g_crit_alert_value == 8.4f);
    for (int i = 0; i < 20; i++) {
      realert_tick();
      // Steady at 8.4 (no worsening), reminders off -> never re-alerts.
      CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 8.4f) == REALERT_NONE);
    }
  }

  // --- 3. Reminder fires every N minutes while in CRIT (#65) -----------------
  {
    reset(/*holdoff*/30, /*sample*/15);              // 30 min = 2 wakes
    crit_episode_begin(true, 8.4f);
    CHECK(g_crit_realert_ticks == 2);
    realert_tick();                                  // ticks 2 -> 1
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 8.4f) == REALERT_NONE);
    realert_tick();                                  // ticks 1 -> 0
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 8.4f) == REALERT_REMINDER);
    crit_realert_arm(REALERT_REMINDER, 8.4f);        // send-ack re-arms
    CHECK(g_crit_realert_ticks == 2);
    realert_tick();
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 8.4f) == REALERT_NONE);
  }

  // --- 4. Escalation on worsening, precedence over reminder (#18) ------------
  {
    reset(/*holdoff*/30);
    crit_episode_begin(true, 8.2f);                  // baseline 8.2
    // Below the +1.5 step -> no escalation.
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 9.0f) == REALERT_NONE);
    // >= 8.2 + 1.5 = 9.7 -> escalate, even though the reminder clock has not
    // elapsed (escalation out-ranks the reminder).
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 9.8f) == REALERT_ESCALATION);
    crit_realert_arm(REALERT_ESCALATION, 9.8f);      // baseline moves to 9.8
    CHECK(g_crit_alert_value == 9.8f);
    CHECK(g_escal_count == 1);
    // Now 9.8 is the baseline; 9.8 alone is not a further +1.5 worsening.
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 9.8f) == REALERT_NONE);
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 11.4f) == REALERT_ESCALATION);
  }

  // --- 5. Escalation, CRIT_LOW direction (lower is worse) --------------------
  {
    reset(0);
    crit_episode_begin(true, 1.8f);                  // baseline 1.8 (below 2.0)
    CHECK(crit_realert_decision(ALERT_CRIT_LOW, 1.0f) == REALERT_NONE);   // -0.8 only
    CHECK(crit_realert_decision(ALERT_CRIT_LOW, 0.2f) == REALERT_ESCALATION); // <= 1.8-1.5
  }

  // --- 6. Hourly escalation cap bounds volume --------------------------------
  {
    reset(0);
    crit_episode_begin(true, 8.0f);
    float v = 8.0f;
    int fired = 0;
    for (int i = 0; i < 10; i++) {
      v += 2.0f;                                     // always a fresh +1.5 worsening
      if (crit_realert_decision(ALERT_CRIT_HIGH, v) == REALERT_ESCALATION) {
        crit_realert_arm(REALERT_ESCALATION, v);
        fired++;
      }
    }
    CHECK(fired == ESCALATE_MAX_PER_HOUR);           // capped at 3/hour
    CHECK(!escal_budget_ok());
  }

  // --- 7. Budget refills after the hour window elapses -----------------------
  {
    reset(0, /*sample*/15);
    g_escal_count = ESCALATE_MAX_PER_HOUR;           // spent out
    escal_budget_spend();                            // opens the window (60min=4 wakes)
    CHECK(!escal_budget_ok());
    for (int i = 0; i < realert_wakes_for_min(60); i++) realert_tick();
    // One more tick past the window boundary refills the budget.
    realert_tick();
    CHECK(escal_budget_ok());
    CHECK(g_escal_count == 0);
  }

  // --- 8. Episode reset clears the box baseline ------------------------------
  {
    reset(0);   // reminders off, so this isolates the escalation baseline
    crit_episode_begin(true, 8.4f);
    CHECK(!std::isnan(g_crit_alert_value));
    crit_episode_reset();
    CHECK(std::isnan(g_crit_alert_value));
    CHECK(g_crit_realert_ticks == 0);
    CHECK(crit_realert_decision(ALERT_CRIT_HIGH, 20.0f) == REALERT_NONE); // no baseline -> no escalation
  }

  // --- 9. Not critical -> always NONE ---------------------------------------
  {
    reset(30);
    crit_episode_begin(true, 8.4f);
    for (int i = 0; i < 5; i++) realert_tick();
    CHECK(crit_realert_decision(ALERT_OK, 5.0f)       == REALERT_NONE);
    CHECK(crit_realert_decision(ALERT_WARN_HIGH, 7.5f) == REALERT_NONE);
  }

  // --- 10. Predicted-breach escalation: ETA halves (#18) --------------------
  {
    reset(0);
    // Set up a warming projection: box below fridge so predictor targets CRIT_HIGH.
    for (int i = 0; i < PREDICT_SMOOTH_N; i++) { box_hist_push(6.0f); fridge_hist_push(20.0f); }
    g_last_predict_alert = ALERT_PREDICTED_BREACH_HIGH;
    g_predict_holdoff    = PREDICT_HOLDOFF_SAMPLES;   // inside the quiet window
    float now = predictor_t_min();
    CHECK(std::isfinite(now));
    predict_alert_record(now * 2.0f);                // last alerted at ~2x current ETA
    AlertState esc = predict_escalation_side();
    CHECK(esc == ALERT_PREDICTED_BREACH_HIGH);       // ETA collapsed to <= half -> escalate
    // Not in hold-off -> no escalation (a fresh fire is the normal path).
    g_predict_holdoff = 0;
    CHECK(predict_escalation_side() == ALERT_OK);
    // ETA not yet halved -> no escalation.
    g_predict_holdoff = PREDICT_HOLDOFF_SAMPLES;
    predict_alert_record(now * 1.2f);
    CHECK(predict_escalation_side() == ALERT_OK);
  }

  std::printf("test_realert: all %d checks passed\n", g_checks);
  return 0;
}
