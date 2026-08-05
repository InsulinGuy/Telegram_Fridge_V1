// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
#pragma once
// TeleFridge V1 — runtime-configurable settings: struct, NVS persistence,
// Telegram /set* command parser + validators, /status and /defaults.
//
// This is the contract from docs/settings-inventory.md (issue #32) codified for
// issue #4. Scope = Tier 1 + Tier 2 only; predictor/approach knobs are a
// deferred addendum (out of scope until logger/predictor.h exists). The
// alert_debounce_n *firing-side* logic is split to issue #49 — this file carries
// the knob as a persisted, guardrailed setting only, it does NOT change alerting.
//
// Persistence is NVS (rare writes, acceptable per ADR-005), NOT the RTC-memory
// ring buffer. All five guardrails run on the WRITE PATH here (apply_command /
// settings_apply_*), so they hold regardless of caller — the Telegram channel
// today (#8) and the web UI later (#31/#48) both route through the same setters.
//
// Guardrails (docs/settings-inventory.md §Guardrails):
//   1. Ordering:  crit_low <= warn_low < warn_high <= crit_high (both sets),
//      re-validated as a whole after any single-field write.
//   2. Pharma envelope: box_crit_low/box_crit_high within [0.0, 10.0], rejected
//      with a specific message.
//   3. Capacity cap: report_every <= RING_CAPACITY (bound to the constant).
//   4. NaN/unset -> compiled default in helpers.h. A factory-fresh device (or a
//      wiped NVS) behaves exactly like today's firmware.
//   5. Remote visibility: settings_overrides_line() feeds the 4-h report a
//      compact "overrides:" line for any field differing from its default.

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "helpers.h"       // THRESH_* / predictor defaults / constants
#include "ring_buffer.h"   // RING_CAPACITY (capacity-cap guardrail)

// ESPHome's preference backend (NVS on esp-idf). Guarded so this header still
// compiles in a host-side unit test outside the ESPHome build (load/store then
// degrade to RAM-only, which is all a host round-trip test needs to exercise).
#if defined(__has_include)
#  if __has_include("esphome/core/preferences.h")
#    include "esphome/core/preferences.h"
#    define TF_HAVE_ESPHOME_PREFS 1
#  endif
#endif

// ---------------------------------------------------------------------------
// Field bounds (docs/settings-inventory.md Tier 1 + Tier 2 tables).
// ---------------------------------------------------------------------------
// Box field range IS the pharma-envelope hard bound (guardrail #2).
constexpr float   BOX_FIELD_MIN      = 0.0f;
constexpr float   BOX_FIELD_MAX      = 10.0f;
constexpr float   FRIDGE_FIELD_MIN   = -20.0f;
constexpr float   FRIDGE_FIELD_MAX   = 30.0f;
constexpr uint16_t REPORT_EVERY_MIN  = 1;
constexpr uint16_t REPORT_EVERY_MAX  = RING_CAPACITY;   // capacity cap, not a literal
constexpr uint16_t SAMPLE_INTERVAL_MIN_LO = 1;
constexpr uint16_t SAMPLE_INTERVAL_MIN_HI = 60;
constexpr uint16_t CRIT_HOLDOFF_MIN_LO = 0;
constexpr uint16_t CRIT_HOLDOFF_MIN_HI = 1440;
constexpr uint8_t  ALERT_DEBOUNCE_LO = 1;
constexpr uint8_t  ALERT_DEBOUNCE_HI = 8;

// Bumped whenever the RuntimeSettings layout changes — a stale blob of a prior
// layout (same NVS key) fails the magic check on load and repairs to defaults.
constexpr uint32_t SETTINGS_MAGIC   = 0x54460401;   // "TF" + issue 04 rev 01
constexpr uint32_t SETTINGS_NVS_KEY = 0x7E1EF11D;   // stable make_preference key

// ---------------------------------------------------------------------------
// The persisted settings blob.
// ---------------------------------------------------------------------------
struct RuntimeSettings {
  uint32_t magic;                 // SETTINGS_MAGIC — layout guard (not user-set)

  // Tier 1 — per-sensor threshold sets (ADR-011).
  float box_crit_low, box_warn_low, box_warn_high, box_crit_high;
  float fridge_crit_low, fridge_warn_low, fridge_warn_high, fridge_crit_high;
  uint16_t report_every;          // samples per report

  // Tier 2.
  uint16_t sample_interval_min;   // deep-sleep duration, minutes
  uint16_t crit_realert_holdoff_min;  // 0 = reminders off (today's behaviour)
  uint8_t  alert_debounce_n;      // knob only in #4; firing logic is #49

  // Housekeeping.
  uint32_t update_offset;         // Telegram getUpdates cursor (owned by #8)
  uint16_t settings_rev;          // bumped on every persisted change
};

// Compiled defaults — the guardrail-#4 fallback. A factory-fresh device uses
// exactly these, so its behaviour is identical to pre-settings firmware.
inline RuntimeSettings settings_defaults() {
  RuntimeSettings s{};
  s.magic            = SETTINGS_MAGIC;
  s.box_crit_low     = THRESH_BOX_CRIT_LOW;
  s.box_warn_low     = THRESH_BOX_WARN_LOW;
  s.box_warn_high    = THRESH_BOX_WARN_HIGH;
  s.box_crit_high    = THRESH_BOX_CRIT_HIGH;
  s.fridge_crit_low  = THRESH_FRIDGE_CRIT_LOW;
  s.fridge_warn_low  = THRESH_FRIDGE_WARN_LOW;
  s.fridge_warn_high = THRESH_FRIDGE_WARN_HIGH;
  s.fridge_crit_high = THRESH_FRIDGE_CRIT_HIGH;
  s.report_every     = 16;
  s.sample_interval_min       = 15;
  s.crit_realert_holdoff_min  = 0;
  s.alert_debounce_n          = 1;   // N=1 == today's instant classification
  s.update_offset    = 0;
  s.settings_rev     = 0;
  return s;
}

// Live settings + its preference handle. Defined here (single-TU include model,
// same rationale as ring_buffer.h) so no separate .cpp is needed.
inline RuntimeSettings g_settings = settings_defaults();
#ifdef TF_HAVE_ESPHOME_PREFS
inline esphome::ESPPreferenceObject g_settings_pref;
inline bool g_settings_pref_made = false;
#endif

// ---------------------------------------------------------------------------
// Settings-aware alert classification (issue #50).
// ---------------------------------------------------------------------------
// Same zone ladder as helpers.h alert_state(), but reads the LIVE per-sensor
// bands from g_settings instead of the compile-time THRESH_* constants — so a
// persisted /setbox / /setfridge actually changes what alerts (and the report's
// zone labels), surviving reboot via NVS. Guardrail #4 keeps defaults == the
// compiled bands, so on a factory-fresh device this agrees bit-for-bit with the
// constexpr alert_state(). Named _live (not an overload of the (float,bool)
// form) so every call site reads explicitly as "classify against live settings".
inline AlertState alert_state_live(float value, bool is_box) {
  const RuntimeSettings &s = g_settings;
  return is_box
    ? alert_state_bands(value, s.box_crit_low, s.box_warn_low,
                        s.box_warn_high, s.box_crit_high)
    : alert_state_bands(value, s.fridge_crit_low, s.fridge_warn_low,
                        s.fridge_warn_high, s.fridge_crit_high);
}

// ---------------------------------------------------------------------------
// Validation helpers.
// ---------------------------------------------------------------------------

// Replace any NaN threshold with its compiled default (guardrail #4, per-field).
inline void settings_fill_nan(RuntimeSettings &s) {
  const RuntimeSettings d = settings_defaults();
  if (std::isnan(s.box_crit_low))     s.box_crit_low     = d.box_crit_low;
  if (std::isnan(s.box_warn_low))     s.box_warn_low     = d.box_warn_low;
  if (std::isnan(s.box_warn_high))    s.box_warn_high    = d.box_warn_high;
  if (std::isnan(s.box_crit_high))    s.box_crit_high    = d.box_crit_high;
  if (std::isnan(s.fridge_crit_low))  s.fridge_crit_low  = d.fridge_crit_low;
  if (std::isnan(s.fridge_warn_low))  s.fridge_warn_low  = d.fridge_warn_low;
  if (std::isnan(s.fridge_warn_high)) s.fridge_warn_high = d.fridge_warn_high;
  if (std::isnan(s.fridge_crit_high)) s.fridge_crit_high = d.fridge_crit_high;
}

// Validate one four-zone threshold set. Fills `err` and returns false on the
// first violation. Enforces per-field range, the pharma envelope for the box
// set (guardrail #2), and the ordering guardrail (#1).
inline bool validate_threshold_set(float crit_low, float warn_low,
                                   float warn_high, float crit_high,
                                   bool is_box, std::string &err) {
  const float lo   = is_box ? BOX_FIELD_MIN : FRIDGE_FIELD_MIN;
  const float hi   = is_box ? BOX_FIELD_MAX : FRIDGE_FIELD_MAX;
  const char *name = is_box ? "box" : "fridge";
  char b[192];

  const float vals[4] = {crit_low, warn_low, warn_high, crit_high};
  const char *lbl[4]  = {"crit_low", "warn_low", "warn_high", "crit_high"};
  for (int i = 0; i < 4; i++) {
    if (std::isnan(vals[i])) {
      snprintf(b, sizeof(b), "%s %s is not a number", name, lbl[i]);
      err = b; return false;
    }
    if (vals[i] < lo || vals[i] > hi) {
      snprintf(b, sizeof(b), "%s %s=%.1f out of range [%.1f, %.1f]",
               name, lbl[i], vals[i], lo, hi);
      err = b; return false;
    }
  }

  // Guardrail #2 — pharma envelope, explicit even though it coincides with the
  // box field bound (kept legible + survives a field-range edit).
  if (is_box) {
    if (crit_low < 0.0f || crit_low > 10.0f ||
        crit_high < 0.0f || crit_high > 10.0f) {
      snprintf(b, sizeof(b),
        "box CRIT bounds must stay within the pharma envelope [0.0, 10.0] C "
        "(got crit_low=%.1f, crit_high=%.1f)", crit_low, crit_high);
      err = b; return false;
    }
  }

  // Guardrail #1 — ordering: crit_low <= warn_low < warn_high <= crit_high.
  if (!(crit_low <= warn_low && warn_low < warn_high && warn_high <= crit_high)) {
    snprintf(b, sizeof(b),
      "%s zones must satisfy crit_low <= warn_low < warn_high <= crit_high "
      "(got %.1f, %.1f, %.1f, %.1f)",
      name, crit_low, warn_low, warn_high, crit_high);
    err = b; return false;
  }
  return true;
}

// Whole-settings integrity check (both sets + scalar bounds). Used on load to
// decide whether a passed-CRC blob is sane, and after every write.
inline bool settings_valid(const RuntimeSettings &s, std::string &err) {
  if (!validate_threshold_set(s.box_crit_low, s.box_warn_low,
                              s.box_warn_high, s.box_crit_high, true, err))
    return false;
  if (!validate_threshold_set(s.fridge_crit_low, s.fridge_warn_low,
                              s.fridge_warn_high, s.fridge_crit_high, false, err))
    return false;
  // Guardrail #3 — capacity cap.
  if (s.report_every < REPORT_EVERY_MIN || s.report_every > REPORT_EVERY_MAX) {
    char b[96];
    snprintf(b, sizeof(b), "report_every=%u out of range [%u, %u]",
             s.report_every, REPORT_EVERY_MIN, REPORT_EVERY_MAX);
    err = b; return false;
  }
  if (s.sample_interval_min < SAMPLE_INTERVAL_MIN_LO ||
      s.sample_interval_min > SAMPLE_INTERVAL_MIN_HI) {
    char b[96];
    snprintf(b, sizeof(b), "sample_interval_min=%u out of range [%u, %u]",
             s.sample_interval_min, SAMPLE_INTERVAL_MIN_LO, SAMPLE_INTERVAL_MIN_HI);
    err = b; return false;
  }
  if (s.crit_realert_holdoff_min > CRIT_HOLDOFF_MIN_HI) {
    char b[96];
    snprintf(b, sizeof(b), "crit_holdoff_min=%u out of range [%u, %u]",
             s.crit_realert_holdoff_min, CRIT_HOLDOFF_MIN_LO, CRIT_HOLDOFF_MIN_HI);
    err = b; return false;
  }
  if (s.alert_debounce_n < ALERT_DEBOUNCE_LO || s.alert_debounce_n > ALERT_DEBOUNCE_HI) {
    char b[96];
    snprintf(b, sizeof(b), "debounce_n=%u out of range [%u, %u]",
             s.alert_debounce_n, ALERT_DEBOUNCE_LO, ALERT_DEBOUNCE_HI);
    err = b; return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// NVS load / store.
// ---------------------------------------------------------------------------

// Persist g_settings to NVS. Bumps settings_rev, then saves + syncs so a
// subsequent deep_sleep.enter can't lose the write. No-op (RAM only) off-device.
inline bool settings_store() {
  g_settings.settings_rev++;
  g_settings.magic = SETTINGS_MAGIC;
#ifdef TF_HAVE_ESPHOME_PREFS
  if (!g_settings_pref_made) {
    g_settings_pref =
      esphome::global_preferences->make_preference<RuntimeSettings>(SETTINGS_NVS_KEY);
    g_settings_pref_made = true;
  }
  bool ok = g_settings_pref.save(&g_settings);
  esphome::global_preferences->sync();
  return ok;
#else
  return true;
#endif
}

// Load settings from NVS at boot. On empty/corrupt NVS, a magic mismatch, or a
// blob that fails validation, fall back to compiled defaults (guardrail #4) so a
// factory-fresh device behaves exactly like today's firmware.
inline void settings_load() {
  g_settings = settings_defaults();
#ifdef TF_HAVE_ESPHOME_PREFS
  g_settings_pref =
    esphome::global_preferences->make_preference<RuntimeSettings>(SETTINGS_NVS_KEY);
  g_settings_pref_made = true;
  RuntimeSettings loaded{};
  if (g_settings_pref.load(&loaded) && loaded.magic == SETTINGS_MAGIC) {
    settings_fill_nan(loaded);            // per-field NaN -> default
    std::string err;
    if (settings_valid(loaded, err)) {
      g_settings = loaded;                // stored settings are sane — adopt
    }
    // else: keep compiled defaults (a corrupt-but-CRC-valid blob repairs itself)
  }
#endif
}

// ---------------------------------------------------------------------------
// Command parsing.
// ---------------------------------------------------------------------------

// strtof/strtoul wrappers that reject trailing junk ("7.5x" is not a number).
inline bool parse_float_strict(const std::string &t, float &out) {
  if (t.empty()) return false;
  const char *c = t.c_str();
  char *end = nullptr;
  errno = 0;
  float v = strtof(c, &end);
  if (end == c || *end != '\0' || errno != 0 || std::isnan(v) || std::isinf(v))
    return false;
  out = v;
  return true;
}
inline bool parse_uint_strict(const std::string &t, long &out) {
  if (t.empty()) return false;
  const char *c = t.c_str();
  char *end = nullptr;
  errno = 0;
  long v = strtol(c, &end, 10);
  if (end == c || *end != '\0' || errno != 0 || v < 0) return false;
  out = v;
  return true;
}

inline std::vector<std::string> settings_tokenize(const std::string &s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    size_t start = i;
    while (i < s.size() && !std::isspace((unsigned char)s[i])) i++;
    if (i > start) out.push_back(s.substr(start, i - start));
  }
  return out;
}

inline std::string settings_fmt1(float v) {
  char b[16];
  snprintf(b, sizeof(b), "%.1f", v);
  return b;
}

// Apply a full four-zone set to `dst` (box or fridge) after validation.
inline bool apply_threshold_set(RuntimeSettings &s, bool is_box,
                                float cl, float wl, float wh, float ch,
                                std::string &reply) {
  std::string err;
  if (!validate_threshold_set(cl, wl, wh, ch, is_box, err)) {
    reply = "Rejected: " + err;
    return false;
  }
  if (is_box) {
    s.box_crit_low = cl; s.box_warn_low = wl;
    s.box_warn_high = wh; s.box_crit_high = ch;
  } else {
    s.fridge_crit_low = cl; s.fridge_warn_low = wl;
    s.fridge_warn_high = wh; s.fridge_crit_high = ch;
  }
  return true;
}

// /setbox and /setfridge share this: 4 numeric args = whole set; or
// "<field> <value>" single-field form (re-validates the whole set, guardrail #1).
inline bool cmd_set_thresholds(bool is_box, const std::vector<std::string> &tok,
                               std::string &reply) {
  const char *name = is_box ? "box" : "fridge";
  RuntimeSettings s = g_settings;   // work on a copy; commit only if valid

  if (tok.size() == 5) {            // /setX cl wl wh ch
    float v[4];
    for (int i = 0; i < 4; i++) {
      if (!parse_float_strict(tok[i + 1], v[i])) {
        reply = std::string("Rejected: ") + name + " value '" + tok[i + 1] +
                "' is not a number";
        return false;
      }
    }
    if (!apply_threshold_set(s, is_box, v[0], v[1], v[2], v[3], reply)) return false;
  } else if (tok.size() == 3) {     // /setX <field> <value>
    const std::string &field = tok[1];
    float val;
    if (!parse_float_strict(tok[2], val)) {
      reply = std::string("Rejected: value '") + tok[2] + "' is not a number";
      return false;
    }
    float cl = is_box ? s.box_crit_low  : s.fridge_crit_low;
    float wl = is_box ? s.box_warn_low  : s.fridge_warn_low;
    float wh = is_box ? s.box_warn_high : s.fridge_warn_high;
    float ch = is_box ? s.box_crit_high : s.fridge_crit_high;
    if      (field == "crit_low")  cl = val;
    else if (field == "warn_low")  wl = val;
    else if (field == "warn_high") wh = val;
    else if (field == "crit_high") ch = val;
    else {
      reply = std::string("Rejected: unknown ") + name +
              " field '" + field + "' (crit_low|warn_low|warn_high|crit_high)";
      return false;
    }
    if (!apply_threshold_set(s, is_box, cl, wl, wh, ch, reply)) return false;
  } else {
    reply = std::string("Usage: /set") + name +
            " <crit_low> <warn_low> <warn_high> <crit_high>  |  /set" + name +
            " <field> <value>";
    return false;
  }

  g_settings = s;
  settings_store();
  reply = std::string("OK: ") + name + " set to " +
          settings_fmt1(is_box ? s.box_crit_low  : s.fridge_crit_low)  + " / " +
          settings_fmt1(is_box ? s.box_warn_low  : s.fridge_warn_low)  + " / " +
          settings_fmt1(is_box ? s.box_warn_high : s.fridge_warn_high) + " / " +
          settings_fmt1(is_box ? s.box_crit_high : s.fridge_crit_high) + " C";
  return true;
}

// Generic scalar setter: /setX <n>, bounds [lo, hi], writes *field on success.
inline bool cmd_set_uint(const std::vector<std::string> &tok, const char *label,
                         long lo, long hi, uint16_t *field16, uint8_t *field8,
                         std::string &reply) {
  if (tok.size() != 2) {
    char b[96];
    snprintf(b, sizeof(b), "Usage: /set%s <%ld..%ld>", label, lo, hi);
    reply = b; return false;
  }
  long v;
  if (!parse_uint_strict(tok[1], v)) {
    reply = std::string("Rejected: '") + tok[1] + "' is not a whole number";
    return false;
  }
  if (v < lo || v > hi) {
    char b[96];
    snprintf(b, sizeof(b), "Rejected: %s=%ld out of range [%ld, %ld]", label, v, lo, hi);
    reply = b; return false;
  }
  if (field16) *field16 = (uint16_t)v;
  if (field8)  *field8  = (uint8_t)v;
  settings_store();
  char b[96];
  snprintf(b, sizeof(b), "OK: %s set to %ld", label, v);
  reply = b;
  return true;
}

// ---------------------------------------------------------------------------
// /status, /defaults, overrides line, help.
// ---------------------------------------------------------------------------

// Compact "overrides:" line for the 4-h report (guardrail #5). Empty when every
// value equals its default, so the report stays short in the common case.
inline std::string settings_overrides_line() {
  const RuntimeSettings d = settings_defaults();
  const RuntimeSettings &s = g_settings;
  std::vector<std::string> parts;
  if (s.box_crit_low != d.box_crit_low || s.box_warn_low != d.box_warn_low ||
      s.box_warn_high != d.box_warn_high || s.box_crit_high != d.box_crit_high)
    parts.push_back("box=" + settings_fmt1(s.box_crit_low) + "/" +
      settings_fmt1(s.box_warn_low) + "/" + settings_fmt1(s.box_warn_high) + "/" +
      settings_fmt1(s.box_crit_high));
  if (s.fridge_crit_low != d.fridge_crit_low || s.fridge_warn_low != d.fridge_warn_low ||
      s.fridge_warn_high != d.fridge_warn_high || s.fridge_crit_high != d.fridge_crit_high)
    parts.push_back("fridge=" + settings_fmt1(s.fridge_crit_low) + "/" +
      settings_fmt1(s.fridge_warn_low) + "/" + settings_fmt1(s.fridge_warn_high) + "/" +
      settings_fmt1(s.fridge_crit_high));
  char b[48];
  if (s.report_every != d.report_every) {
    snprintf(b, sizeof(b), "report_every=%u", s.report_every); parts.push_back(b);
  }
  if (s.sample_interval_min != d.sample_interval_min) {
    snprintf(b, sizeof(b), "interval=%umin", s.sample_interval_min); parts.push_back(b);
  }
  if (s.crit_realert_holdoff_min != d.crit_realert_holdoff_min) {
    snprintf(b, sizeof(b), "crit_ho=%umin", s.crit_realert_holdoff_min); parts.push_back(b);
  }
  if (s.alert_debounce_n != d.alert_debounce_n) {
    snprintf(b, sizeof(b), "debounce=%u", s.alert_debounce_n); parts.push_back(b);
  }
  if (parts.empty()) return "";
  std::string out = "overrides: ";
  for (size_t i = 0; i < parts.size(); i++) { if (i) out += ", "; out += parts[i]; }
  return out;
}

inline std::string settings_status() {
  const RuntimeSettings &s = g_settings;
  std::string o = "\xE2\x9A\x99 TeleFridge settings (rev ";
  o += std::to_string(s.settings_rev) + ")\n";
  o += "box    " + settings_fmt1(s.box_crit_low) + " / " +
       settings_fmt1(s.box_warn_low) + " / " + settings_fmt1(s.box_warn_high) +
       " / " + settings_fmt1(s.box_crit_high) + " C\n";
  o += "fridge " + settings_fmt1(s.fridge_crit_low) + " / " +
       settings_fmt1(s.fridge_warn_low) + " / " + settings_fmt1(s.fridge_warn_high) +
       " / " + settings_fmt1(s.fridge_crit_high) + " C\n";
  char b[128];
  snprintf(b, sizeof(b),
    "report_every %u (max %u)\ninterval %u min\ncrit_holdoff %u min\ndebounce %u",
    s.report_every, REPORT_EVERY_MAX, s.sample_interval_min,
    s.crit_realert_holdoff_min, s.alert_debounce_n);
  o += b;
  std::string ov = settings_overrides_line();
  if (!ov.empty()) o += "\n" + ov;
  return o;
}

inline std::string settings_help() {
  return
    "TeleFridge commands:\n"
    "/setbox <cl> <wl> <wh> <ch>\n"
    "/setfridge <cl> <wl> <wh> <ch>\n"
    "/setbox <field> <value>  (field=crit_low|warn_low|warn_high|crit_high)\n"
    "/setreport <1..32>\n"
    "/setinterval <1..60> (min)\n"
    "/setholdoff <0..1440> (min)\n"
    "/setdebounce <1..8>\n"
    "/status  /defaults  /help";
}

// Dispatch one command line. Returns true if a change was applied & persisted;
// false for a rejection, a query (/status,/help), or an unknown command. `reply`
// is always set to a user-facing string. This is the single write-path entry the
// Telegram channel (#8) and the web UI (#48) both call.
inline bool apply_command(const std::string &line, std::string &reply) {
  std::vector<std::string> tok = settings_tokenize(line);
  if (tok.empty()) { reply = "Empty command. " + settings_help(); return false; }

  // Normalise the command word: lowercase, strip a "@botname" suffix.
  std::string cmd = tok[0];
  size_t at = cmd.find('@');
  if (at != std::string::npos) cmd = cmd.substr(0, at);
  for (char &c : cmd) c = (char)std::tolower((unsigned char)c);
  tok[0] = cmd;

  if (cmd == "/setbox")    return cmd_set_thresholds(true,  tok, reply);
  if (cmd == "/setfridge") return cmd_set_thresholds(false, tok, reply);
  if (cmd == "/setreport")
    return cmd_set_uint(tok, "report", REPORT_EVERY_MIN, REPORT_EVERY_MAX,
                        &g_settings.report_every, nullptr, reply);
  if (cmd == "/setinterval")
    return cmd_set_uint(tok, "interval", SAMPLE_INTERVAL_MIN_LO, SAMPLE_INTERVAL_MIN_HI,
                        &g_settings.sample_interval_min, nullptr, reply);
  if (cmd == "/setholdoff")
    return cmd_set_uint(tok, "holdoff", CRIT_HOLDOFF_MIN_LO, CRIT_HOLDOFF_MIN_HI,
                        &g_settings.crit_realert_holdoff_min, nullptr, reply);
  if (cmd == "/setdebounce")
    return cmd_set_uint(tok, "debounce", ALERT_DEBOUNCE_LO, ALERT_DEBOUNCE_HI,
                        nullptr, &g_settings.alert_debounce_n, reply);
  if (cmd == "/status")   { reply = settings_status(); return false; }
  if (cmd == "/help")     { reply = settings_help();   return false; }
  if (cmd == "/defaults") {
    uint32_t offset = g_settings.update_offset;   // don't disturb the getUpdates cursor
    g_settings = settings_defaults();
    g_settings.update_offset = offset;
    settings_store();
    reply = "OK: all settings restored to defaults";
    return true;
  }
  reply = "Unknown command '" + cmd + "'. " + settings_help();
  return false;
}
