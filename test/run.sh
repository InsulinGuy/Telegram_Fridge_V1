#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
# This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
# NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
# Host-compile and run the logger unit tests (no hardware needed). See issue #2.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
c++ -std=c++17 -Wall -Wextra -o "$tmp/trb" test/test_ring_buffer.cpp
"$tmp/trb"
c++ -std=c++17 -Wall -Wextra -o "$tmp/tp" test/test_predictor.cpp
"$tmp/tp"
c++ -std=c++17 -Wall -Wextra -o "$tmp/tc" test/test_commands.cpp
"$tmp/tc"
c++ -std=c++17 -Wall -Wextra -o "$tmp/tas" test/test_alert_settings.cpp
"$tmp/tas"
c++ -std=c++17 -Wall -Wextra -o "$tmp/trl" test/test_realert.cpp
"$tmp/trl"
c++ -std=c++17 -Wall -Wextra -o "$tmp/tr" test/test_report.cpp
"$tmp/tr"
c++ -std=c++17 -Wall -Wextra -o "$tmp/ta" test/test_axp192.cpp
"$tmp/ta"
