<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Security

TeleFridge is a hobbyist project maintained in someone's spare time. There is no
security team, no SLA, and no CVE process. Please read
[`DISCLAIMER.md`](./DISCLAIMER.md) first — this is **not a medical device**, and
nothing here should be your only safeguard for a medication or sample.

That said, the device holds credentials that matter, so this page explains what
they are, how they can leak, and what to do about it.

## Reporting a vulnerability

Open a [GitHub issue](../../issues) for anything routine.

**If the report would itself expose a user's credentials or put a payload at
risk, do not open a public issue.** Use GitHub's
[private vulnerability reporting](../../security/advisories/new) instead. Expect
a best-effort response, not a guaranteed one.

## What this device holds

| Secret | Lives in | If it leaks |
| --- | --- | --- |
| `telegram_bot_token` | `secrets.yaml`, compiled into the firmware | Anyone can read the chat's pending messages and **send messages as your bot** — including a fake "all clear" while your fridge is actually failing |
| `telegram_chat_id` | `secrets.yaml`, compiled in | Low on its own; combined with the token it identifies exactly who to message |
| `wifi_ssid` / `wifi_password` | `secrets.yaml`, compiled in | Your home network |
| `ota_password` | `secrets.yaml`, compiled in | Lets someone flash new firmware onto the device over Wi-Fi while it is in safe mode |

`secrets.yaml` is listed in `.gitignore` and must never be committed. The
repository ships only `secrets.yaml.example`, which contains placeholders.

## The leak that actually happens: your bot token in a log

This is the one to know about.

On a failed or non-2xx HTTP request, ESPHome logs the **entire request URL**, and
the Telegram Bot API puts your token *in the URL path*:

```
[E][http_request]: HTTP Request failed; URL: https://api.telegram.org/bot123456789:AAH...token.../sendMessage; Code: 0
```

Two things make this easy to miss:

- It is logged at **ERROR** level, so lowering `logger: level:` does **not**
  suppress it.
- It does not look like a secret. People diligently redact a line labelled
  "token" and paste a URL without a second thought.

Failed sends are not rare — weak Wi-Fi is the normal cause — so this is exactly
the log someone reaches for when asking for help.

**Before sharing any log:** search it for `api.telegram.org` and replace
everything between `/bot` and the next `/` with `<REDACTED>`.

## Rotating a leaked bot token

Assume a token that was public for even a moment is compromised; bots are
scraped from public logs and pastes automatically.

1. Message [@BotFather](https://t.me/botfather) in Telegram.
2. Send `/revoke` and pick the bot. The old token dies immediately.
3. Put the new token in `secrets.yaml` as `telegram_bot_token`.
4. Re-flash the device — the token is compiled into the firmware, so a rebuild is
   required for the change to take effect.

Rotating Wi-Fi or OTA passwords works the same way: edit `secrets.yaml`, re-flash.

## Notes on the device's exposure

- The device runs **no network services** — there is no ESPHome API server, no
  web server, no captive portal, no mDNS. It only makes outbound HTTPS requests
  to `api.telegram.org`.
- Those requests use `verify_ssl: true` against a **pinned root CA**
  (`certs/telegram_root_ca.pem`).
- Inbound control is limited to Telegram commands, and every update is checked
  against the configured `telegram_chat_id` — messages from any other chat are
  ignored. Commands can only adjust settings within hard-coded guardrails; the
  **2–8 °C box limits cannot be loosened at runtime**.
- **OTA is the main inbound attack surface.** Pressing reset ~10 times puts the
  device into safe mode, where it stays awake with Wi-Fi on and listens for an
  update, guarded only by `ota_password`. Use a strong random value
  (`openssl rand -hex 16`).
- Secrets are compiled into the firmware image, so **a `.bin` built for your
  device contains your Wi-Fi and Telegram credentials in recoverable form.**
  Never share a built binary, and note that `.gitignore` excludes `*.bin` and
  `*.elf` for this reason.
