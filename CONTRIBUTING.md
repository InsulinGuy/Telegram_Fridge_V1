# Contributing to TeleFridge

Thanks for taking a look. TeleFridge is a hobbyist project, but it watches
insulin-class refrigerated storage, so a few things are stricter than usual.

**Before anything else, read [`DISCLAIMER.md`](./DISCLAIMER.md).** This is not a
medical device, and no contribution can make it one.

## The one hard rule

**The box CRIT bounds (2–8 °C) are the pharma-compliance line and are LOCKED.**
`THRESH_BOX_CRIT_LOW = 2.0` and `THRESH_BOX_CRIT_HIGH = 8.0` in
`logger/helpers.h`, and the runtime guardrails in `logger/settings.h` that stop a
`/set` command loosening them, are not up for casual change. A PR that widens
them, or that weakens a guardrail so they can be widened at runtime, will not be
merged without an ADR arguing the case.

The same goes for the two sensor roles: `temp_a` is **always** the box interior
(the safety authority) and `temp_b` is **always** the fridge air. Swapping them
is a compliance defect, not a preference — the reasoning is in `CLAUDE.md` under
Hardware → Sensors and ADR-011.

## How the project is documented

[`CLAUDE.md`](./CLAUDE.md) is the single source of truth: the design, every
locked invariant, the architecture decision log (ADRs), the known-issues table
and the bring-up status. Read it before changing anything structural. Sections
marked **LOCKED** need a new ADR, not just a patch.

Two things to know when reading it:

- **`#NN` issue references throughout the repo cite the original `telefridge_V1`
  tracker** this project was split out of (ADR-025). They are historical
  citations and do not match this repo's issue numbers.
- Anything described as *design only* (e.g. the approach trigger, ADR-014) is
  not implemented. The Open items section is the authoritative status list.

## Before you open a pull request

1. **Run the tests:** `./test/run.sh`. It host-compiles seven suites with plain
   `c++17` — no hardware, no ESPHome, a few seconds. All checks must pass; CI
   runs the same script.
2. **Add tests for logic changes.** Everything in `logger/*.h` is deliberately
   framework-free so it can be tested on the host. If your change is not
   testable that way, say why in the PR.
3. **Check the config still builds:** `esphome config telefridge.yaml` (you will
   need a `secrets.yaml` — copy `secrets.yaml.example`).
4. **Say what you tested on hardware, and what you did not.** "Compiles, not
   flashed" is a perfectly good answer; a silent assumption is not. Several
   alert paths cannot be validated without a fridge.
5. **Update `CLAUDE.md` in the same PR** if you change behaviour it documents.
   Docs drifting from code is the failure mode this project takes most
   seriously — a stale invariant is worse than no invariant.

## Style

- Temperatures in °C; report to 1 dp, keep 2 dp internally.
- GPIO references use the `GPIONN` form, never bare integers.
- Secrets live only in `secrets.yaml` (git-ignored). Never commit real
  credentials, your SSID, your bot token, or a chat ID — check your diff.
- New files get the SPDX header the existing files carry.

## Licensing

TeleFridge is **GPL-3.0-or-later**. By contributing you agree your work is
licensed under the same terms.
