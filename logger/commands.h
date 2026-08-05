// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
#pragma once
// TeleFridge V1 — inbound Telegram command channel (issue #33 / #8).
//
// The device already brings the radio up on every report/alert wake to POST a
// sendMessage. This header adds the PULL half: while the radio is up, issue one
// extra GET to Telegram's getUpdates, hand each pending "/set…" line to the
// settings.h write path (all guardrails already live there), consolidate the
// acks into one reply, and advance the getUpdates cursor.
//
// "Poll", never "drain" — the device polls Telegram for commands on each radio
// wake (the term "drain" collides with battery drain, this project's obsession).
//
// LATENCY CONTRACT (ADR-016): commands apply on the NEXT radio wake — up to the
// report cadence (4 h default), or near-immediately when sent as a reply to a
// CRIT alert (that alert wake polls too). Polling on every 15-min sampling wake
// is off the table: it would enable Wi-Fi every wake and demolish ADR-006.
//
// SECURITY: only messages whose chat.id equals the configured telegram_chat_id
// are dispatched. Unauthorised messages are still consumed (offset-advanced) so
// they don't accumulate and re-deliver forever — but they never touch settings.
//
// PERSISTENCE: the cursor lives in RuntimeSettings.update_offset, which is NVS-
// backed (issue #4), so it survives a full power cycle. RTC memory would be lost
// on a bank/cell cutoff and old commands would replay — hence NVS, not RTC.
//
// HOST-TESTABILITY: the risky logic (auth filter, offset advance, dispatch,
// reply assembly) is the pure `poll_updates(vector<TgUpdate>, …)` core, compiled
// and unit-tested on the host (test/test_commands.cpp) with no ESPHome/JSON dep.
// The JSON parse is a thin ESPHome-only adapter (`poll_updates_json`) guarded by
// __has_include, exactly the split predictor.h/ring_buffer.h use for RTC state.

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "settings.h"   // apply_command() write-path + RuntimeSettings

// ESPHome's JSON helper (ArduinoJson under the hood). Guarded so this header
// still compiles in a host unit test outside the ESPHome build — there the JSON
// adapter is dropped and only the pure core is exercised.
#if defined(__has_include)
#  if __has_include("esphome/components/json/json_util.h")
#    include "esphome/components/json/json_util.h"
#    define TF_HAVE_ESPHOME_JSON 1
#  endif
#endif

// One parsed Telegram update, reduced to the three fields we act on.
struct TgUpdate {
  long long update_id;   // monotonic per bot; cursor is max(update_id)+1
  long long chat_id;     // message.chat.id — the auth key
  std::string text;      // message.text — the command line
};

// Consolidated reply to POST back after a poll (empty = nothing to send). Kept
// as a translation-unit global (same single-include model as g_settings) so the
// YAML lambda can read it after the GET action returns, without an ESPHome
// `globals:` entry.
inline std::string g_cmd_reply;

// Parse a numeric chat id from the configured secret string (bare integer,
// possibly negative for supergroups). Returns 0 on garbage — which can't match a
// real Telegram chat.id, so a misconfigured secret fails closed (ignores all).
inline long long parse_chat_id(const std::string &s) {
  const char *c = s.c_str();
  char *end = nullptr;
  long long v = strtoll(c, &end, 10);
  if (end == c) return 0;
  return v;
}

// PURE CORE — host-tested. Dispatch a batch of already-parsed updates:
//   * advance `offset` past EVERY update (authorised or not) so nothing
//     re-delivers on the next poll;
//   * dispatch only updates from `authorized_chat_id` to the settings write path;
//   * concatenate each command's ack into `reply`.
// Returns true if there is a reply to send. `offset` is the live NVS cursor
// (g_settings.update_offset on-device) and is mutated in place.
inline bool poll_updates(const std::vector<TgUpdate> &updates,
                         long long authorized_chat_id,
                         uint32_t &offset, std::string &reply) {
  reply.clear();
  for (const auto &u : updates) {
    // Consume this update first, unconditionally — an unauthorised or malformed
    // message must not be re-fetched forever (Done-when: unauthorised ignored
    // but not re-delivered). offset = highest id + 1; guarded for out-of-order.
    if (u.update_id >= 0 && (unsigned long long)(u.update_id + 1) > offset)
      offset = (uint32_t)(u.update_id + 1);

    if (u.chat_id != authorized_chat_id) continue;   // not ours → ignore
    if (u.text.empty()) continue;                     // non-text message → skip

    std::string r;
    apply_command(u.text, r);   // validation + NVS persist all inside settings.h
    if (!reply.empty()) reply += "\n";
    reply += r;
  }
  return !reply.empty();
}

#ifdef TF_HAVE_ESPHOME_JSON
// ESPHome-only adapter: parse a getUpdates response body into TgUpdates, then
// run the pure core. `configured_chat_id` is the raw secret string. Returns true
// if there is a reply to send (left in `reply`).
inline bool poll_updates_json(const std::string &body,
                              const std::string &configured_chat_id,
                              uint32_t &offset, std::string &reply) {
  std::vector<TgUpdate> updates;
  esphome::json::parse_json(body, [&](JsonObject root) -> bool {
    if (!(root["ok"] | false)) return false;          // {"ok":false,...} → bail
    for (JsonObject upd : root["result"].as<JsonArray>()) {
      TgUpdate u;
      u.update_id = upd["update_id"] | (long long) 0;
      JsonObject msg = upd["message"];
      u.chat_id = msg["chat"]["id"] | (long long) 0;
      u.text = std::string(msg["text"] | "");
      updates.push_back(u);
    }
    return true;
  });
  return poll_updates(updates, parse_chat_id(configured_chat_id), offset, reply);
}
#endif  // TF_HAVE_ESPHOME_JSON
