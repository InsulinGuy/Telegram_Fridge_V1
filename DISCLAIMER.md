# Disclaimer — read before use

**SPDX-License-Identifier: GPL-3.0-or-later**

TeleFridge is a hobbyist, DIY temperature-logging project. It is provided
**"as is", with NO WARRANTY of any kind** — see sections 15 and 16 of the
[GNU GPL v3](./LICENSE) (Disclaimer of Warranty / Limitation of Liability),
which govern this project and are restated in plain terms below.

## Not a medical device

This project monitors an insulated box holding a temperature-sensitive payload
(referenced in the design as insulin-class, spec range **2–8 °C**). It is **not**
a medical device, is **not** certified or validated for medical, clinical, or
pharmaceutical use, and has **not** been evaluated by any regulatory authority.

## Do not rely on it as a safeguard

- **Do not** use TeleFridge as the sole or primary means of protecting any
  medication, vaccine, biological sample, or other temperature-sensitive item.
- Sensor accuracy, alerting, Wi-Fi delivery, and battery life are all **best
  effort** and can fail silently. Alerts may be delayed, dropped, or never sent
  (weak signal, dead battery, firmware bug, network outage, or a fault in a
  third-party dependency).
- Several design parameters (notably the predictor's `TAU_BOX_MIN`) ship as
  **uncalibrated placeholders** and must be fitted to your own hardware before
  any output can be trusted.

Always keep an independent, validated cold-chain safeguard for anything whose
temperature actually matters, and treat any TeleFridge reading or alert as
advisory information only.

## Your responsibility

By building, flashing, or using this project you accept full responsibility for
the outcome. The authors and contributors accept **no liability** for spoiled
medication, lost samples, property damage, personal injury, or any other loss
arising from its use or failure to work, to the fullest extent permitted by law.
