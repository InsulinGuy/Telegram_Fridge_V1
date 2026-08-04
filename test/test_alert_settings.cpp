// Host-compiled unit tests for the settings-aware zone classification
// (issue #50): alert_state_live() in logger/settings.h.
//
// Same host trick as the other suites: define RTC_DATA_ATTR away and skip the
// ESPHome/NVS includes (guarded in the headers), so the pure classification
// core is exercised directly. Two things are proven:
//   1. With factory defaults, alert_state_live() == the compile-time
//      alert_state() across a temperature sweep (guardrail #4, bit-for-bit).
//   2. After a persisted /setbox / /setfridge (simulated by writing g_settings),
//      alert_state_live() moves the zone while the constexpr alert_state() does
//      not — i.e. the persisted band is actually consumed.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/tas test/test_alert_settings.cpp && /tmp/tas
// (test/run.sh builds and runs this alongside the other suites.)

#define RTC_DATA_ATTR   // no-op on host: plain statics, no RTC section

#include "../logger/settings.h"

#include <cstdio>

static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

int main() {
  // --- 1. Defaults reproduce the compile-time ladder bit-for-bit ------------
  {
    g_settings = settings_defaults();
    // Sweep -10..+15 C at 0.25 C steps, both sensor sets.
    for (int i = -40; i <= 60; ++i) {
      const float v = i * 0.25f;
      CHECK(alert_state_live(v, true)  == alert_state(v, true));
      CHECK(alert_state_live(v, false) == alert_state(v, false));
    }
    // NaN never alerts, on either path.
    const float nan = std::nanf("");
    CHECK(alert_state_live(nan, true)  == ALERT_OK);
    CHECK(alert_state_live(nan, false) == ALERT_OK);
  }

  // --- 2. A persisted box band is consumed by _live but not by the fallback -
  {
    g_settings = settings_defaults();
    // 7.5 C sits in the box WARN_HIGH band by default (warn_high 7.0, crit_high 8.0).
    CHECK(alert_state_live(7.5f, true) == ALERT_WARN_HIGH);
    CHECK(alert_state(7.5f, true)      == ALERT_WARN_HIGH);

    // Tighten the box crit_high to 7.0 (still legal: warn_high must stay <= it).
    g_settings.box_warn_high = 6.5f;
    g_settings.box_crit_high = 7.0f;
    // Now 7.5 C is over the live crit bound -> CRIT_HIGH via _live ...
    CHECK(alert_state_live(7.5f, true) == ALERT_CRIT_HIGH);
    // ... but the compile-time fallback is unchanged (still WARN_HIGH).
    CHECK(alert_state(7.5f, true)      == ALERT_WARN_HIGH);
  }

  // --- 3. A persisted fridge band is consumed independently of the box set --
  {
    g_settings = settings_defaults();
    // 5.5 C is OK for the fridge by default (warn_high 6.0).
    CHECK(alert_state_live(5.5f, false) == ALERT_OK);

    // Lower the fridge warn_high to 5.0; 5.5 C now enters WARN_HIGH via _live.
    g_settings.fridge_warn_high = 5.0f;
    CHECK(alert_state_live(5.5f, false) == ALERT_WARN_HIGH);
    CHECK(alert_state(5.5f, false)      == ALERT_OK);   // fallback unchanged

    // The box set is untouched by a fridge write.
    CHECK(alert_state_live(5.5f, true)  == ALERT_OK);
  }

  std::printf("ok  (%d checks)\n", g_checks);
  return 0;
}
