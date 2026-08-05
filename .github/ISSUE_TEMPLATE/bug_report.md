---
name: Bug report
about: Something in TeleFridge does not work as documented
labels: bug
---

> ⚠️ **Safety first.** TeleFridge is a hobbyist project and **not a medical
> device** — see [`DISCLAIMER.md`](../../DISCLAIMER.md). If a medication or
> sample is at risk *right now*, deal with that first; this issue can wait.

**What happened, and what you expected instead**

**How to reproduce it** (what the device was doing — sampling wake, 4-h report,
button press, alert)

**Setup**
- ESPHome version:
- Board (M5StickC, or other):
- Sensors (M5 ENV II Unit / ENV HAT, or a substitute):
- Firmware version (the `fw` field in the report's housekeeping line):
- Any settings changed from default (paste `/status`):

**Logs** — serial output or the Telegram message, whichever is relevant.

> ⚠️ **Redact before pasting — logs print more than you might expect.**
>
> The one that catches people out: on a failed or non-2xx send, ESPHome logs the
> **whole URL, and your bot token is inside it**. It looks like this, and no log
> level hides it (it is logged at ERROR):
>
> ```
> [E][http_request]: HTTP Request failed; URL: https://api.telegram.org/bot123456789:AAH...rest-of-your-token.../sendMessage; Code: 0
> ```
>
> Search your log for `api.telegram.org` and replace everything between `/bot`
> and `/sendMessage` (or `/getUpdates`) with `<REDACTED>`. Also redact your
> **Wi-Fi SSID** and **chat ID**.
>
> **If you have already posted a token anywhere, rotate it now:** message
> [@BotFather](https://t.me/botfather), send `/revoke`, pick the bot, and put the
> new token in `secrets.yaml`. Assume a token that was public for even a moment
> is compromised — see [`SECURITY.md`](../../SECURITY.md).
