// Host-compiled unit tests for logger/report.h (issue #63).
//
// Same host trick as the other suites: define RTC_DATA_ATTR away so the RTC-
// backed ring/fault/predictor state becomes ordinary statics, and the message
// builders — pure string composition over that state — are exercised directly.
//
// The load-bearing check here is html_ok(): every message uses parse_mode=HTML,
// and Telegram rejects malformed HTML with HTTP 400 (a failed send). So each
// builder's output is scanned for balanced, whitelisted tags. Plus snapshot-ish
// substring assertions for the hierarchy, and a 4096-char length bound.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/tr test/test_report.cpp && /tmp/tr
// (test/run.sh builds and runs this alongside the other suites.)

#define RTC_DATA_ATTR   // no-op on host: plain statics, no RTC section

#include "../logger/report.h"
#include "../logger/realert.h"   // realert_prefix() — issue #65/#18 re-alert markers

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static bool has(const std::string &s, const std::string &sub) {
  return s.find(sub) != std::string::npos;
}

static int count_of(const std::string &s, const std::string &sub) {
  int n = 0;
  for (size_t p = s.find(sub); p != std::string::npos; p = s.find(sub, p + sub.size()))
    n++;
  return n;
}

// Balanced-and-whitelisted tag scanner. Returns false on an unknown tag, a
// mismatched close, or an unclosed tag left on the stack at end — any of which
// would make Telegram reject the message. Dynamic text is html-escaped upstream,
// so the only literal '<' in a well-formed body is a real tag open.
static const char *WHITELIST[] = {"b", "i", "u", "s", "code", "pre", "blockquote"};
static bool tag_ok(const std::string &name) {
  for (auto *w : WHITELIST) if (name == w) return true;
  return false;
}
static bool html_ok(const std::string &s) {
  std::vector<std::string> stack;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] != '<') continue;
    size_t close = s.find('>', i);
    if (close == std::string::npos) return false;         // unterminated tag
    std::string inner = s.substr(i + 1, close - i - 1);
    i = close;
    bool closing = !inner.empty() && inner[0] == '/';
    if (closing) inner = inner.substr(1);
    // Tag name is the first token (attributes like "blockquote expandable" ok).
    std::string name = inner.substr(0, inner.find(' '));
    if (!tag_ok(name)) return false;
    if (closing) {
      if (stack.empty() || stack.back() != name) return false;
      stack.pop_back();
    } else {
      stack.push_back(name);
    }
  }
  return stack.empty();
}

// --- Fixtures ---------------------------------------------------------------

static void reset() {
  g_ring_count = g_ring_head = 0;
  g_wake_count = 0;
  g_fault_reason_a = g_fault_reason_b = FAULT_NONE;
  g_fridge_hist_count = g_fridge_hist_head = 0;
  g_box_hist_count = g_box_hist_head = 0;
  g_last_send_epoch = 0;
  g_settings = settings_defaults();
}

// Report call with fixed housekeeping args (rssi/up/batt/epoch/wake/cadence).
static std::string report(bool button = false) {
  return build_report(-67, 42, 4.10f, 95.0f, /*now*/1000u, g_wake_count, button,
                      /*interval*/240u);
}

int main() {
  // --- 1. Healthy report: box-first verdict, pre table, collapsed diag -------
  {
    reset();
    for (int i = 0; i < 16; i++) ring_push(1000u + i * 900u, 4.2f, 5.1f);
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "<b>Box OK"));                 // tier 0 verdict, box-first
    CHECK(has(m, "\xF0\x9F\x9F\xA2"));          // 🟢
    CHECK(has(m, "<pre>"));                     // tier 1 aligned table
    CHECK(has(m, "Fridge"));                    // fridge row present
    CHECK(has(m, "<blockquote expandable>"));   // tier 3 collapsed
    CHECK(has(m, "\xF0\x9F\x94\x8B"));          // 🔋 battery line
    CHECK(!has(m, "out of range"));             // nothing out of band
    CHECK(m.size() < 4096);
  }

  // --- 2. Box CRIT: verdict flips, excursion line appears --------------------
  {
    reset();
    for (int i = 0; i < 15; i++) ring_push(1000u + i * 900u, 4.0f, 5.0f);
    ring_push(1000u + 15 * 900u, 8.4f, 11.2f);   // latest reading in CRIT_HIGH
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "\xF0\x9F\x94\xB4"));          // 🔴
    CHECK(has(m, "TOO WARM"));                  // CRIT_HIGH verdict word
    CHECK(has(m, "out of range"));              // excursion count line
    CHECK(!has(m, "Box OK"));
  }

  // --- 3. Faulted box sensor: never renders "OK" -----------------------------
  {
    reset();
    for (int i = 0; i < 16; i++) ring_push(1000u + i * 900u, 4.2f, 5.1f);
    g_fault_reason_a = FAULT_NAN;               // box sensor faulted
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "SENSOR FAULT"));
    CHECK(has(m, "no reading"));
    CHECK(has(m, "\xF0\x9F\x9B\x91"));          // 🛑
    CHECK(!has(m, "Box OK"));                   // the defect this fixes
  }

  // --- 4. Empty buffer ------------------------------------------------------
  {
    reset();
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "no samples buffered"));
    CHECK(m.size() < 4096);
  }

  // --- 5. Pre-SNTP window (epoch 0): no window line, still valid -------------
  {
    reset();
    for (int i = 0; i < 16; i++) ring_push(0u, 4.2f, 5.1f);   // no clock
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "16 samples"));
    CHECK(!has(m, "UTC"));                       // no window rendered without a clock
  }

  // --- 6. Button-forced report is labelled ----------------------------------
  {
    reset();
    for (int i = 0; i < 4; i++) ring_push(1000u + i * 900u, 4.2f, 5.1f);
    std::string m = report(/*button=*/true);
    CHECK(html_ok(m));
    CHECK(has(m, "on-demand"));
  }

  // --- 7. Full 32-sample ring stays under the 4096-char limit ---------------
  {
    reset();
    g_settings.box_crit_low = 1.0f;             // force an overrides line too
    for (int i = 0; i < RING_CAPACITY; i++) ring_push(1000u + i * 900u, 4.2f, 5.1f);
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "overrides:"));
    CHECK(m.size() < 4096);
  }

  // --- 7b. "last send" line surfaces a prior confirmed send (issue #14) ------
  {
    reset();
    for (int i = 0; i < 8; i++) ring_push(1000u + i * 900u, 4.2f, 5.1f);
    CHECK(!has(report(), "last send"));          // none recorded yet
    g_last_send_epoch = 1700000000u;             // a prior 2xx send
    std::string m = report();
    CHECK(html_ok(m));
    CHECK(has(m, "last send"));
    CHECK(has(m, "UTC"));
    CHECK(m.size() < 4096);
  }

  // --- 8. Boot message -------------------------------------------------------
  {
    reset();
    std::string m = build_boot_msg(28.8f, 28.0f, -60, 5, 1000u, 240u, 4.10f, 95.0f);
    CHECK(html_ok(m));
    CHECK(has(m, "online"));
    CHECK(has(m, "<pre>"));
    CHECK(has(m, "<blockquote expandable>"));
    CHECK(m.size() < 4096);
  }

  // --- 9. Alert bodies + shared footer --------------------------------------
  {
    reset();
    std::string crit = build_crit_alert("Box", true, 8.4f, ALERT_CRIT_HIGH)
                     + alert_footer(4.10f, 95.0f, 1000u);
    CHECK(html_ok(crit));
    CHECK(has(crit, "<code>"));                 // threshold math in monospace
    CHECK(has(crit, "8.0"));                    // the breached bound

    std::string pred = build_predict_alert(ALERT_PREDICTED_BREACH_HIGH, 6.8f, 11.2f, 35.0f)
                     + alert_footer(4.10f, 95.0f, 1000u);
    CHECK(html_ok(pred));
    CHECK(has(pred, "PREDICTED BREACH"));
    CHECK(has(pred, "driver"));

    g_fault_reason_b = FAULT_STALE;
    std::string fault = build_fault_alert() + alert_footer(4.10f, 95.0f, 1000u);
    CHECK(html_ok(fault));
    CHECK(has(fault, "Fridge"));
    CHECK(has(fault, "stale"));

    std::string batt = build_batt_alert(3.30f, 12.0f);
    CHECK(html_ok(batt));
    CHECK(has(batt, "LOW BATTERY"));

    // Re-alert prefixes (#65 reminder / #18 escalation) prepended to a CRIT/predict
    // body must keep the HTML balanced (a malformed tag -> Telegram HTTP 400).
    std::string rem = realert_prefix(REALERT_REMINDER)
                    + build_crit_alert("Box", true, 8.6f, ALERT_CRIT_HIGH)
                    + alert_footer(4.10f, 95.0f, 1000u);
    CHECK(html_ok(rem));
    CHECK(has(rem, "reminder"));
    std::string esc = realert_prefix(REALERT_ESCALATION)
                    + build_predict_alert(ALERT_PREDICTED_BREACH_HIGH, 7.0f, 12.0f, 20.0f)
                    + alert_footer(4.10f, 95.0f, 1000u);
    CHECK(html_ok(esc));
    CHECK(has(esc, "ESCALATION"));
    CHECK(realert_prefix(REALERT_ENTRY)[0] == '\0');   // entry/none: no prefix
  }

  // --- 10. Device label leads EVERY outbound message ------------------------
  // A sibling board posts to the same chat, so an untagged alert is ambiguous
  // exactly when that costs most. The label must be first, present once, and
  // must not break the HTML (a malformed tag is an HTTP 400 = a failed send).
  {
    reset();
    for (int i = 0; i < 16; i++) ring_push(1000u + i * 900u, 4.2f, 5.1f);

    // Applied to each message type the device can send, incl. the multi-builder
    // CRIT body — the case that made tagging inside the builders unworkable.
    std::vector<std::string> bodies = {
      report(),
      build_boot_msg(5.0f, 5.2f, -60, 10u, 2000u, 240u, 4.10f, 95.0f),
      build_crit_alert("Box", true, 8.6f, ALERT_CRIT_HIGH) + alert_footer(4.10f, 95.0f, 1000u),
      realert_prefix(REALERT_ESCALATION)
        + build_crit_alert("Box", true, 9.9f, ALERT_CRIT_HIGH)
        + build_crit_alert("Fridge", false, 11.0f, ALERT_CRIT_HIGH)
        + alert_footer(4.10f, 95.0f, 1000u),
      build_predict_alert(ALERT_PREDICTED_BREACH_HIGH, 7.0f, 12.0f, 20.0f),
      build_fault_alert(),
      build_batt_alert(3.30f, 12.0f),
    };
    for (const std::string &body : bodies) {
      const std::string m = with_device_tag(body);
      CHECK(html_ok(m));                          // tag must not unbalance the HTML
      CHECK(m.rfind("<b>Stick C</b>\n", 0) == 0); // FIRST thing in the message
      CHECK(m.size() < 4096);
      // Exactly once — the bug tagging-inside-builders would have caused.
      CHECK(count_of(m, "Stick C") == 1);
      // The original body survives intact underneath the label.
      CHECK(has(m, body));
    }

    // The command reply is sent WITHOUT parse_mode (it echoes raw user text),
    // so its label must be plain — literal "<b>" would be shown to the user.
    const std::string reply = with_device_tag("OK: report set to 8", false);
    CHECK(reply.rfind("Stick C\n", 0) == 0);
    CHECK(!has(reply, "<b>"));
    CHECK(has(reply, "OK: report set to 8"));

    // A label is only useful if it distinguishes this board from the sibling.
    CHECK(std::string(DEVICE_LABEL) == "Stick C");
  }

  std::printf("test_report: all %d checks passed\n", g_checks);
  return 0;
}
