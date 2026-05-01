#!/usr/bin/env bash
# Test that check_vendor_drift.sh correctly detects vendor drift.
# This verifies the fix for #521.

set -e

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

echo "=== Test 1: Base case (no drift) ==="
bash scripts/check_vendor_drift.sh
echo "PASS: no drift detected as expected"

echo ""
echo "=== Test 2: Drift detection (client dep in server code) ==="
# Temporarily add sokol (client-only) to server code
TEST_FILE="server/test_drift_sokol_temp.c"
cat > "$TEST_FILE" << 'EOF'
#include "vendor/sokol/sokol_gfx.h"
int main(void) { return 0; }
EOF

if bash scripts/check_vendor_drift.sh 2>&1 | grep -q "ERROR: vendor/sokol"; then
  echo "PASS: drift detected correctly for sokol"
  rm "$TEST_FILE"
else
  echo "FAIL: drift not detected for sokol"
  rm "$TEST_FILE"
  exit 1
fi

echo ""
echo "=== Test 3: Drift detection (minimp3 in server code) ==="
TEST_FILE="shared/test_drift_minimp3_temp.h"
cat > "$TEST_FILE" << 'EOF'
#include "../vendor/minimp3/minimp3.h"
EOF

if bash scripts/check_vendor_drift.sh 2>&1 | grep -q "ERROR: vendor/minimp3"; then
  echo "PASS: drift detected correctly for minimp3"
  rm "$TEST_FILE"
else
  echo "FAIL: drift not detected for minimp3"
  rm "$TEST_FILE"
  exit 1
fi

echo ""
echo "=== Test 4: No false positive for existing cenetex ==="
# cenetex is included and NOT in .dockerignore, so should be fine
bash scripts/check_vendor_drift.sh
echo "PASS: no false positive for cenetex"

echo ""
echo "✓ All vendor drift detection tests passed"
