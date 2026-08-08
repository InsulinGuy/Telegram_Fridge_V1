// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
#pragma once
// TeleFridge V1 — build the Telegram message bodies from the ring-buffer digest.
// See CLAUDE.md "Telegram report" and docs/telegram-message-ia.md (issue #63).
//
// Messages use Telegram parse_mode=HTML (a fixed tag whitelist: <b> <i> <code>
// <pre> <blockquote>; no CSS/colour/tables). The four-tier hierarchy is:
//   0 verdict  — is the payload safe? (bold headline + status emoji)
//   1 evidence — box trend (predictor) + the per-sensor <pre> stats table
//   2 context  — fridge advisory, out-of-range excursion count
//   3 housekeeping — window, RSSI, uptime, fw, cadence, overrides (collapsed)
// Battery sits just above tier 3 (housekeeping but actionable — ADR-010).
//
// The actual HTTPS POST is done by ESPHome's http_request component in the YAML;
// this header only composes the message body string. Bodies are built with
// std::string append (not a fixed char buffer): a truncated snprintf could emit
// an unclosed tag, which Telegram rejects with HTTP 400 and the send fails.

#include <string>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "ring_buffer.h"
#include "helpers.h"
#include "predictor.h"     // predictor_evaluate/box_ema/fridge_ema — box trend line
#include "settings.h"      // settings_overrides_line() — guardrail #5 (remote visibility)
#include "build_number.h"  // FW_VERSION (report.h is the actual consumer)

// --- Device label ---------------------------------------------------------
// This chat receives messages from MORE THAN ONE device (a sibling board posts
// to the same Telegram channel), so every outbound message must say which box
// it came from, as the FIRST thing in the message — otherwise an alert is
// ambiguous exactly when ambiguity is most expensive.
//
// Applied by with_device_tag() at the SEND SITE, not inside the individual
// builders. That placement is deliberate: several messages are composed from
// more than one builder (the CRIT body is realert_prefix() + up to two
// build_crit_alert() calls + alert_footer()), so tagging inside a builder would
// emit the label twice on some paths and mid-message on others. One wrap at the
// point the body becomes a message gives exactly one label, always first.
constexpr char DEVICE_LABEL[] = "Stick C";

// Prepend the device label to a finished message body.
//
// `html` must match the send's parse_mode. HTML messages get a bold label; the
// inbound-command reply is deliberately sent as PLAIN TEXT (it echoes raw user
// input that may contain < or >, which would 400 the send under HTML), so it
// takes the untagged form. Passing html=true for a plain-text send would just
// show literal "<b>" to the user; passing false for an HTML send is harmless.
// TEMPORARY DIAGNOSTIC FOOTER (BOX SENSOR FAULT "no reading" investigation).
// Appended to EVERY outbound message via with_device_tag(), which is the one
// place all seven send sites funnel through — the alternative was editing each
// http_request.post body and missing one.
//
// PLAIN TEXT ONLY, no tags: with_device_tag() serves both the HTML sends and the
// plain-text command reply, and an unbalanced tag would 400 the send outright
// (the failure mode ADR-017 already warns about). Emitting no markup is correct
// on both paths.
//
// Reads only RTC state captured earlier in the wake — it must not touch the
// PMIC here, because this runs at send time, long after the read being reported.
// Delete this function and its call when the investigation closes.
inline std::string diag_footer() {
  char buf[128];
  // A FAILED read must never render as a plausible value. Reg 0x00 = 0x00 means
  // "no external supply present", i.e. running on the cell — the exact condition
  // under investigation — so printing a failed read as 0x00 would manufacture
  // the finding we are trying to test for. Same reasoning as axp192.h returning
  // NaN rather than 0 for a failed voltage read (ADR-023).
  char rails_s[16], pwr_s[16], exten_s[8];
  if (g_diag_rails < 0) { snprintf(rails_s, sizeof(rails_s), "FAIL");
                          snprintf(exten_s, sizeof(exten_s), "?"); }
  else                  { snprintf(rails_s, sizeof(rails_s), "0x%02X", (unsigned) g_diag_rails);
                          snprintf(exten_s, sizeof(exten_s), "%u",
                                   (unsigned) ((g_diag_rails & 0x40) ? 1 : 0)); }
  if (g_diag_pwr < 0)   snprintf(pwr_s, sizeof(pwr_s), "FAIL");
  else                  snprintf(pwr_s, sizeof(pwr_s), "0x%02X", (unsigned) g_diag_pwr);

  snprintf(buf, sizeof(buf), "\ndiag 0x12=%s EXTEN=%s pwr=%s nan_a=%u nanwakes=%u",
           rails_s, exten_s, pwr_s,
           (unsigned) g_diag_nan_a, (unsigned) g_diag_nan_wakes);
  return std::string(buf);
}

inline std::string with_device_tag(const std::string &body, bool html = true) {
  std::string out;
  if (html) {
    out += "<b>";
    out += html_escape(DEVICE_LABEL);
    out += "</b>\n";
  } else {
    out += DEVICE_LABEL;
    out += "\n";
  }
  out += body;
  out += diag_footer();
  return out;
}

// Escape a string for embedding in a JSON double-quoted value. Runs AFTER the
// HTML has been assembled (html_escape → tags → json_escape → YAML URL-encode).
inline std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      default:   out += c;      break;
    }
  }
  return out;
}

// One-line battery status (JST Li-ion, sole supply). Voltage from the board's
// A13 divider, % estimated by li_ion_soc(). Shows "n/a" on NaN (no reading).
inline std::string batt_str(float batt_v, float batt_pct) {
  char b[48];
  if (std::isnan(batt_v) || std::isnan(batt_pct))
    snprintf(b, sizeof(b), "%s battery n/a", batt_glyph(batt_pct));
  else
    snprintf(b, sizeof(b), "%s %.2f V / %.0f%%", batt_glyph(batt_pct), batt_v, batt_pct);
  return std::string(b);
}

// --- Small formatting helpers (issue #63) --------------------------------

// "HH:MM" in UTC for the given epoch, or "" when the clock was never valid (0).
inline std::string hhmm_utc(uint32_t epoch) {
  if (epoch == 0) return "";
  time_t t = (time_t)epoch;
  struct tm *tm_info = gmtime(&t);
  char b[8];
  snprintf(b, sizeof(b), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);
  return std::string(b);
}

// "HH:MM–HH:MM UTC" window from the digest's epoch bounds, or "" if unclocked.
inline std::string window_str(const Digest &d) {
  if (d.first_epoch == 0) return "";
  return hhmm_utc(d.first_epoch) + "\xE2\x80\x93" + hhmm_utc(d.last_epoch) + " UTC";  // en dash
}

// One right-justified 5-wide temperature cell for the <pre> table; "-" on NaN.
inline std::string tcell(float v) {
  char b[8];
  if (std::isnan(v)) snprintf(b, sizeof(b), "%5s", "-");
  else               snprintf(b, sizeof(b), "%5.1f", v);
  return std::string(b);
}

// --- Report sub-blocks (issue #63) ---------------------------------------

// Tier 0 verdict line for the box (temp_a, the safety authority per ADR-011).
// A faulted box reports FAULT, never a fabricated "OK" zone.
inline std::string report_verdict(const Digest &d, bool box_faulted) {
  if (box_faulted)
    return std::string(zone_emoji(ALERT_SENSOR_FAULT))
      + " <b>Box SENSOR FAULT \xE2\x80\x94 " + html_escape(fault_reason(true)) + "</b>";
  AlertState s = alert_state_live(d.cur_a, /*is_box=*/true);  // live bands (issue #50)
  char t[16];
  snprintf(t, sizeof(t), "%.1f", d.cur_a);
  return std::string(zone_emoji(s)) + " <b>Box " + alert_label(s)
    + " \xE2\x80\x94 " + t + "\xC2\xB0" "C</b>";                          // — / °C
}

// Tier 1 trend line: box direction from the predictor, plus the fridge reading.
// "steady" when no bound is projected; the ETA is marked "(est)" because
// TAU_BOX_MIN is a pre-commissioning placeholder (CLAUDE.md τ_box calibration).
inline std::string report_trend(const Digest &d, bool box_faulted) {
  std::string fr;
  if (std::isnan(d.cur_b)) {
    fr = "fridge \xE2\x80\x94";                                          // em dash
  } else {
    char b[24];
    snprintf(b, sizeof(b), "fridge %.1f\xC2\xB0" "C", d.cur_b);
    fr = b;
  }
  std::string head = "steady";
  if (!box_faulted) {
    PredictorResult r = predictor_evaluate(box_ema(), fridge_ema(), TAU_BOX_MIN);
    if (r.side != ALERT_OK && std::isfinite(r.t_min)) {
      const char *dir = (r.side == ALERT_PREDICTED_BREACH_HIGH) ? "warming" : "cooling";
      float bound = (r.side == ALERT_PREDICTED_BREACH_HIGH)
                      ? THRESH_BOX_CRIT_HIGH : THRESH_BOX_CRIT_LOW;
      char b[64];
      snprintf(b, sizeof(b), "%s \xE2\x86\x92 %.0f\xC2\xB0" "C ~%.0fmin (est)",
               dir, bound, r.t_min);                                     // → arrow
      head = b;
    }
  }
  return "<i>" + head + " \xC2\xB7 " + fr + "</i>";                      // middle dot
}

// Tier 1/2 <pre> stats table. Content is all fixed labels + snprintf'd numbers,
// so nothing here needs HTML-escaping. A faulted sensor renders "-" across.
inline std::string report_table(const Digest &d, bool box_faulted, bool fridge_faulted) {
  auto row = [](const char *label, float cur, float mn, float mx, float avg, bool faulted) {
    char lab[16];
    snprintf(lab, sizeof(lab), "%-6s", label);
    std::string r = std::string(lab) + " ";
    if (faulted) {
      char dashes[40];
      snprintf(dashes, sizeof(dashes), "%5s %5s %5s %5s", "-", "-", "-", "-");
      r += dashes;
    } else {
      r += tcell(cur) + " " + tcell(mn) + " " + tcell(mx) + " " + tcell(avg);
    }
    return r;
  };
  char h[48];
  snprintf(h, sizeof(h), "%-6s %5s %5s %5s %5s", "", "Cur", "Min", "Max", "Avg");
  std::string p = "<pre>";
  p += h; p += "\n";
  p += row("Box",    d.cur_a, d.min_a, d.max_a, d.mean_a, box_faulted);   p += "\n";
  p += row("Fridge", d.cur_b, d.min_b, d.max_b, d.mean_b, fridge_faulted);
  p += "</pre>";
  return p;
}

// Tier 3 collapsed diagnostics. Everything the *logger* health depends on, one
// tap away so a healthy report stays short.
inline std::string diag_block(const Digest &d, int rssi, uint32_t uptime_s,
                              uint16_t wake_count, uint32_t report_interval_min,
                              bool button_wake) {
  std::string b = "<blockquote expandable>";
  char l[112];
  if (d.n > 0) {
    snprintf(l, sizeof(l), "%u samples", (unsigned)d.n);
    b += l;
    std::string win = window_str(d);
    if (!win.empty()) { b += " \xC2\xB7 "; b += win; }
    b += "\n";
  }
  snprintf(l, sizeof(l), "%s %d dBm \xC2\xB7 up %us \xC2\xB7 wake %u",
           rssi_bars(rssi), rssi, (unsigned)uptime_s, (unsigned)wake_count);
  b += l;
  if (button_wake) b += " \xC2\xB7 on-demand";
  b += "\n";
  char iv[16];
  if (report_interval_min % 60 == 0) snprintf(iv, sizeof(iv), "%uh", report_interval_min / 60);
  else                               snprintf(iv, sizeof(iv), "%umin", report_interval_min);
  b += "fw "; b += html_escape(FW_VERSION); b += " \xC2\xB7 report every "; b += iv;
  // Last confirmed 2xx send (issue #14): lets the owner spot silently-missed
  // messages. Absent until the first success or before a valid clock exists.
  if (g_last_send_epoch != 0) {
    b += "\nlast send "; b += hhmm_utc(g_last_send_epoch); b += " UTC";
  }
  std::string ov = settings_overrides_line();
  if (!ov.empty()) { b += "\n"; b += html_escape(ov); }
  b += "</blockquote>";
  return b;
}

// Trailing context footer shared by the alert bodies: battery + wall-clock, so a
// 3 a.m. alert says whether the logger itself will survive the night.
inline std::string alert_footer(float batt_v, float batt_pct, uint32_t now_epoch) {
  std::string f = "\n" + batt_str(batt_v, batt_pct);
  std::string t = hhmm_utc(now_epoch);
  if (!t.empty()) { f += " \xC2\xB7 "; f += t; f += " UTC"; }
  return f;
}

// --- Message builders -----------------------------------------------------

// Compose the scheduled 4-hour report (parse_mode=HTML). Leads with the box
// verdict; fridge is advisory; diagnostics collapse into the blockquote.
inline std::string build_report(int rssi, uint32_t uptime_s, float batt_v, float batt_pct,
                                uint32_t now_epoch, uint16_t wake_count,
                                bool button_wake, uint32_t report_interval_min) {
  (void)now_epoch;   // window bounds come from the digest epochs, not "now"
  Digest d = ring_summary();
  if (d.n == 0) {
    std::string out = "\xF0\x9F\x93\x8B <b>TeleFridge</b>\n<i>no samples buffered</i>\n";  // 📋
    out += batt_str(batt_v, batt_pct) + "\n";
    out += diag_block(d, rssi, uptime_s, wake_count, report_interval_min, button_wake);
    return out;
  }
  bool bf = sensor_faulted(true), ff = sensor_faulted(false);
  std::string out = report_verdict(d, bf) + "\n";
  out += report_trend(d, bf) + "\n";
  out += report_table(d, bf, ff) + "\n";
  // Excursion line — how much of the window the box spent out of range. Matters
  // for a pharma payload: a brief blip and a 45-min excursion are different events.
  if (!bf && d.n_out_a > 0) {
    char e[64];
    snprintf(e, sizeof(e), "\xE2\x9A\xA0\xEF\xB8\x8F %u of %u samples out of range",
             (unsigned)d.n_out_a, (unsigned)d.n);                        // ⚠️
    out += e; out += "\n";
  }
  out += batt_str(batt_v, batt_pct) + "\n";
  out += diag_block(d, rssi, uptime_s, wake_count, report_interval_min, button_wake);
  return out;
}

// Parse __DATE__ ("Mmm [D]D YYYY") + __TIME__ ("HH:MM:SS") into "YYYY-MM-DD HH:MM".
inline std::string build_time_iso() {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *d = __DATE__;
  const char *t = __TIME__;
  char mon[4] = {d[0], d[1], d[2], 0};
  int month = 1;
  for (int i = 0; i < 12; i++) {
    if (strncmp(mon, months + i * 3, 3) == 0) { month = i + 1; break; }
  }
  int day  = (d[4] == ' ') ? (d[5] - '0') : ((d[4] - '0') * 10 + (d[5] - '0'));
  int year = (d[7]-'0')*1000 + (d[8]-'0')*100 + (d[9]-'0')*10 + (d[10]-'0');
  char buf[24];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %c%c:%c%c",
    year, month, day, t[0], t[1], t[3], t[4]);
  return std::string(buf);
}

// Compose the one-shot power-on boot notification (sent once per power cycle).
// now_epoch: SNTP Unix timestamp (0 if not yet synced).
// report_interval_min: report_every * sample_interval_min, passed from YAML.
inline std::string build_boot_msg(float ta, float tb, int rssi, uint32_t uptime_s,
                                  uint32_t now_epoch, uint32_t report_interval_min,
                                  float batt_v, float batt_pct) {
  AlertState sa = alert_state_live(ta, /*is_box=*/true);   // live bands (issue #50)
  AlertState sb = alert_state_live(tb, /*is_box=*/false);
  // Headline glyph tracks the box zone (temp_a, the ADR-011 authority) — a boot
  // with an already-warm box must not announce itself green (issue #2).
  std::string out = std::string(zone_emoji(sa)) + " <b>TeleFridge ";
  out += html_escape(FW_VERSION); out += " online</b>\n";
  char sc[64];
  snprintf(sc, sizeof(sc), "Box %.1f\xC2\xB0" "C \xC2\xB7 fridge %.1f\xC2\xB0" "C", ta, tb);
  out += "<i>"; out += sc; out += "</i>\n";
  // Temp / Zone table (zone last so a multi-word label can't break alignment).
  char h[48], rA[64], rB[64];
  snprintf(h,  sizeof(h),  "%-6s %6s  %s", "", "Temp", "Zone");
  snprintf(rA, sizeof(rA), "%-6s %5.1f\xC2\xB0 %s", "Box",    ta, alert_label(sa));
  snprintf(rB, sizeof(rB), "%-6s %5.1f\xC2\xB0 %s", "Fridge", tb, alert_label(sb));
  out += "<pre>"; out += h; out += "\n"; out += rA; out += "\n"; out += rB; out += "</pre>\n";
  out += batt_str(batt_v, batt_pct) + "\n";
  // Diagnostics.
  out += "<blockquote expandable>";
  out += "built "; out += html_escape(build_time_iso()); out += "\n";
  char nb[40] = "unknown";
  if (now_epoch > 0) {
    time_t t = (time_t)now_epoch;
    strftime(nb, sizeof(nb), "%Y-%m-%d %H:%M UTC", gmtime(&t));
  }
  out += "now   "; out += nb; out += "\n";
  char l[80];
  snprintf(l, sizeof(l), "%s %d dBm \xC2\xB7 up %us", rssi_bars(rssi), rssi, (unsigned)uptime_s);
  out += l; out += "\n";
  char iv[16];
  if (report_interval_min % 60 == 0) snprintf(iv, sizeof(iv), "%uh", report_interval_min / 60);
  else                               snprintf(iv, sizeof(iv), "%umin", report_interval_min);
  out += "report every "; out += iv;
  out += "</blockquote>";
  return out;
}

// Compose an out-of-schedule CRIT alert for one sensor (parse_mode=HTML). `which`
// is a display label ("Box"/"Fridge"); `is_box` selects its threshold set for the
// breached bound. The YAML appends alert_footer() once after concatenating.
inline std::string build_crit_alert(const char *which, bool is_box, float temp, AlertState s) {
  float bound = (s == ALERT_CRIT_HIGH)
    ? (is_box ? THRESH_BOX_CRIT_HIGH : THRESH_FRIDGE_CRIT_HIGH)
    : (is_box ? THRESH_BOX_CRIT_LOW  : THRESH_FRIDGE_CRIT_LOW);
  char buf[192];
  snprintf(buf, sizeof(buf),
    "%s <b>%s %s \xE2\x80\x94 %.1f\xC2\xB0" "C</b>\n"                    // emoji, —, °C
    "<i>limit <code>%.1f\xC2\xB0" "C</code></i>",
    zone_emoji(s), which, alert_label(s), temp, bound);
  return std::string(buf);
}

// Compose an out-of-schedule PREDICTED BREACH alert (ADR-013 / issue #3). The box
// is not yet out of range but is projected to cross a box CRIT bound within the
// horizon; `side` picks direction and `t_min` the ETA (INFINITY → "imminently").
inline std::string build_predict_alert(AlertState side, float temp_a, float temp_b,
                                       float t_min) {
  const char *dir = (side == ALERT_PREDICTED_BREACH_HIGH) ? "warming" : "cooling";
  const float bound = (side == ALERT_PREDICTED_BREACH_HIGH)
                        ? THRESH_BOX_CRIT_HIGH : THRESH_BOX_CRIT_LOW;
  char eta[28];
  if (std::isfinite(t_min)) snprintf(eta, sizeof(eta), "in ~%.0f min", t_min);
  else                      snprintf(eta, sizeof(eta), "imminently");
  char buf[256];
  snprintf(buf, sizeof(buf),
    "\xE2\x9A\xA0\xEF\xB8\x8F <b>PREDICTED BREACH \xE2\x80\x94 box %s</b>\n"   // ⚠️ —
    "<code>%.1f\xC2\xB0" "C \xE2\x86\x92 %.0f\xC2\xB0" "C %s</code>\n"        // → °C
    "<i>driver: fridge %.1f\xC2\xB0" "C</i>",
    dir, temp_a, bound, eta, temp_b);
  return std::string(buf);
}

// Compose an out-of-schedule SENSOR FAULT alert (issue #12). Lists whichever
// sensor(s) are faulted with the reason. Reads the latched fault state set by
// faults_observe(); call only when at least one sensor is faulted.
inline std::string build_fault_alert() {
  // Glyph must stay in step with zone_emoji(ALERT_SENSOR_FAULT) — the alert and
  // the report verdict describe the same condition (issue #2).
  std::string text = std::string(zone_emoji(ALERT_SENSOR_FAULT))
    + " <b>TeleFridge SENSOR FAULT</b>";
  if (sensor_faulted(true)) {
    text += "\n<i>Box: ";    text += html_escape(fault_reason(true));  text += "</i>";
  }
  if (sensor_faulted(false)) {
    text += "\n<i>Fridge: "; text += html_escape(fault_reason(false)); text += "</i>";
  }
  return text;
}

// Compose an out-of-schedule LOW BATTERY alert (JST Li-ion, sole supply). Sent
// once per depletion via the g_last_batt_alert latch; prompts a manual recharge.
inline std::string build_batt_alert(float batt_v, float batt_pct) {
  char buf[160];
  snprintf(buf, sizeof(buf),
    "\xF0\x9F\xAA\xAB <b>TeleFridge LOW BATTERY</b>\n"                   // 🪫
    "<i>%.0f%% (%.2f V) \xE2\x80\x94 recharge the cell</i>",             // em dash
    batt_pct, batt_v);
  return std::string(buf);
}
