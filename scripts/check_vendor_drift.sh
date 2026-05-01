#!/usr/bin/env bash
# Check that all vendor/ includes in server code are not in the .dockerignore
# exclusion list. This catches #include statements that would build locally
# but fail in the Docker image (see #518, #520, #521).
#
# Usage: bash scripts/check_vendor_drift.sh
# Exit 0 if OK, exit 1 if drift detected.

set -e

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

# Vendor dirs listed in .dockerignore as excluded from the server image
IGNORED_VENDORS=$(grep -E "^vendor/" .dockerignore | sed 's|vendor/||;s|/||' | sort -u)

# Find all #include "vendor/..." and #include "../vendor/..." in server code
INCLUDED_VENDORS=$(
  {
    grep -rh '#include.*vendor' server/ shared/ src/ --include="*.c" --include="*.h" 2>/dev/null || true
  } | sed 's|.*vendor/||;s|/.*||' | sort -u
)

echo "Ignored vendors in .dockerignore: $IGNORED_VENDORS"
echo "Included vendors in source code: $INCLUDED_VENDORS"

DRIFT=""
for vendor in $INCLUDED_VENDORS; do
  if echo "$IGNORED_VENDORS" | grep -q "^$vendor\$"; then
    echo "ERROR: vendor/$vendor is used in server/shared code but is ignored in .dockerignore"
    DRIFT="$DRIFT vendor/$vendor"
  fi
done

if [ -n "$DRIFT" ]; then
  echo ""
  echo "Vendor drift detected:$DRIFT"
  echo ""
  echo "Fix by:"
  echo "  - Moving vendor from .dockerignore, OR"
  echo "  - Moving the #include to client-only code"
  exit 1
fi

echo "✓ No vendor drift detected"
exit 0
