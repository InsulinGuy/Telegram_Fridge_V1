// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
// This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
// NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
// Host-compiled unit tests for logger/commands.h (issue #33 / #8).
//
// Same host trick as the other suites: define RTC_DATA_ATTR away and skip the
// ESPHome/JSON includes (guarded in the header), so the pure command-poll core —
// auth filter, offset advance, dispatch, reply assembly — is exercised directly
// over synthesised TgUpdate batches. No JSON parsing here: that is the thin
// ESPHome-only adapter, out of host scope by the same rule the YAML lambdas are.
//
// Build & run:  c++ -std=c++17 -Wall -o /tmp/tc test/test_commands.cpp && /tmp/tc
// (test/run.sh builds and runs this alongside the other suites.)

#define RTC_DATA_ATTR   // no-op on host: plain statics, no RTC section

#include "../logger/commands.h"

#include <cstdio>
#include <string>
#include <vector>

static int g_checks = 0;
#define CHECK(cond) do { g_checks++; if (!(cond)) { \
  std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); return 1; } } while (0)

static const long long OWNER = 123456789LL;
static const long long STRANGER = 999999999LL;

// Reset settings to compiled defaults before each scenario so cross-test writes
// don't leak (apply_command persists into the shared g_settings).
static void reset_settings() {
  g_settings = settings_defaults();
}

static TgUpdate mk(long long id, long long chat, const std::string &text) {
  return TgUpdate{id, chat, text};
}

int main() {
  // --- 1. Authorised /setreport applies and is acknowledged ------------------
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    std::vector<TgUpdate> batch = { mk(10, OWNER, "/setreport 8") };
    bool has_reply = poll_updates(batch, OWNER, offset, reply);

    CHECK(has_reply);
    CHECK(g_settings.report_every == 8);            // applied
    CHECK(offset == 11);                            // advanced past update 10
    CHECK(reply.find("report") != std::string::npos);
  }

  // --- 2. Unauthorised chat is IGNORED but still consumed --------------------
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    std::vector<TgUpdate> batch = { mk(20, STRANGER, "/setreport 4") };
    bool has_reply = poll_updates(batch, OWNER, offset, reply);

    CHECK(!has_reply);                              // no reply to a stranger
    CHECK(reply.empty());
    CHECK(g_settings.report_every == 16);           // default untouched
    CHECK(offset == 21);                            // BUT offset advanced (no replay)
  }

  // --- 3. Mixed batch: strangers skipped, owner commands applied IN ORDER ----
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    std::vector<TgUpdate> batch = {
      mk(30, STRANGER, "/setreport 2"),      // ignored
      mk(31, OWNER,    "/setinterval 30"),   // applied
      mk(32, OWNER,    "/setreport 8"),      // applied
    };
    poll_updates(batch, OWNER, offset, reply);

    CHECK(g_settings.report_every == 8);
    CHECK(g_settings.sample_interval_min == 30);
    CHECK(offset == 33);                            // past the highest id (32)
    // Both owner acks present, stranger's command never ran.
    CHECK(reply.find("interval") != std::string::npos);
    CHECK(reply.find("report") != std::string::npos);
  }

  // --- 4. Malformed command from owner → usage/rejection reply, no crash -----
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    std::vector<TgUpdate> batch = { mk(40, OWNER, "/setreport banana") };
    bool has_reply = poll_updates(batch, OWNER, offset, reply);

    CHECK(has_reply);                              // owner gets feedback...
    CHECK(g_settings.report_every == 16);          // ...but nothing applied
    CHECK(offset == 41);
    CHECK(reply.find("Rejected") != std::string::npos ||
          reply.find("not a whole number") != std::string::npos);
  }

  // --- 5. Guardrail still enforced via the poll path (bad ordering rejected) -
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    // warn_high below warn_low would collapse a zone — settings.h must reject.
    std::vector<TgUpdate> batch = { mk(50, OWNER, "/setbox 2 7 3 8") };
    poll_updates(batch, OWNER, offset, reply);

    CHECK(g_settings.box_warn_low == 3.0f);         // unchanged (default)
    CHECK(reply.find("Rejected") != std::string::npos);
    CHECK(offset == 51);
  }

  // --- 6. Empty poll (no pending updates) → no reply, offset unchanged -------
  {
    reset_settings();
    uint32_t offset = 77;
    std::string reply;
    std::vector<TgUpdate> batch;                    // getUpdates returned []
    bool has_reply = poll_updates(batch, OWNER, offset, reply);

    CHECK(!has_reply);
    CHECK(offset == 77);                            // cursor untouched
  }

  // --- 7. Non-text message (no text) from owner is consumed, not dispatched --
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    std::vector<TgUpdate> batch = { mk(60, OWNER, "") };  // e.g. a sticker
    bool has_reply = poll_updates(batch, OWNER, offset, reply);

    CHECK(!has_reply);
    CHECK(offset == 61);                            // still consumed
  }

  // --- 8. Out-of-order ids: offset lands on the max, not the last seen -------
  {
    reset_settings();
    uint32_t offset = 0;
    std::string reply;
    std::vector<TgUpdate> batch = {
      mk(72, OWNER, "/status"),   // query only
      mk(70, OWNER, "/help"),
      mk(71, OWNER, "/status"),
    };
    poll_updates(batch, OWNER, offset, reply);
    CHECK(offset == 73);                            // max(72)+1, not 71+1
  }

  // --- 9. parse_chat_id handles bare + negative (supergroup) ids -------------
  {
    CHECK(parse_chat_id("123456789") == 123456789LL);
    CHECK(parse_chat_id("-1001234567890") == -1001234567890LL);
    CHECK(parse_chat_id("garbage") == 0);           // fails closed
  }

  std::printf("test_commands: all %d checks passed\n", g_checks);
  return 0;
}
