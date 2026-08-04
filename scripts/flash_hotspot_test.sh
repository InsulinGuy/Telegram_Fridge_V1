#!/usr/bin/env bash
# Flash telefridge.yaml using secrets_hotspot_test.yaml (hotspot A/B test for issue #24).
# Temporarily swaps secrets_hotspot_test.yaml in as secrets.yaml, then restores on exit.
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -f secrets_hotspot_test.yaml ]]; then
  echo "ERROR: secrets_hotspot_test.yaml not found" >&2
  exit 1
fi

if [[ -f secrets.yaml ]]; then
  cp secrets.yaml secrets.yaml.bak
  trap 'mv secrets.yaml.bak secrets.yaml; echo "secrets.yaml restored"' EXIT
else
  trap 'rm -f secrets.yaml; echo "secrets.yaml removed (was not present before)"' EXIT
fi

cp secrets_hotspot_test.yaml secrets.yaml
echo "Using secrets_hotspot_test.yaml for this build"

esphome run telefridge.yaml "$@"
