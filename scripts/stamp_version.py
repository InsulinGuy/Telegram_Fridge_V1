#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Insulin Guy and the TeleFridge contributors.
# This file is part of TeleFridge. It comes with ABSOLUTELY NO WARRANTY and is
# NOT a medical device — see DISCLAIMER.md. See LICENSE for the full terms.
"""Stamp FW_VERSION in logger/build_number.h with `git describe` output.
Called by Claude Code PreToolUse hook before esphome run/compile."""
import subprocess, re, pathlib, sys

repo = pathlib.Path(__file__).parent.parent
header = repo / "logger" / "build_number.h"

try:
    version = subprocess.check_output(
        ["git", "-C", str(repo), "describe", "--tags", "--always", "--dirty"],
        text=True, stderr=subprocess.DEVNULL
    ).strip()
except subprocess.CalledProcessError:
    version = "unknown"

text = header.read_text()
updated = re.sub(r'(FW_VERSION\[\] = ")[^"]*"', f'FW_VERSION[] = "{version}"', text)

if updated != text:
    header.write_text(updated)
    print(f"FW_VERSION stamped: {version}", file=sys.stderr)
