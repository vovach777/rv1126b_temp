#!/bin/bash
# Apply dual camera display patch on Linux build host.
# Usage:  ./apply_patch.sh  [sdk_root]
#   sdk_root defaults to current directory.

set -e

SDK_ROOT="${1:-.}"
cd "$SDK_ROOT"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "==> Converting patch + new source to Unix line endings (dos2unix)..."
dos2unix "$SCRIPT_DIR/dual_camera_patch.diff"
dos2unix "$SCRIPT_DIR/rkadk_dual_disp_test.c"

echo "==> Applying git patch..."
git apply --whitespace=fix "$SCRIPT_DIR/dual_camera_patch.diff"

echo "==> Copying new test app into app/rkadk/examples/..."
mkdir -p app/rkadk/examples
cp "$SCRIPT_DIR/rkadk_dual_disp_test.c" app/rkadk/examples/

echo "==> Done."
echo "Now build the SDK; new target: rkadk_dual_disp_test"
echo "Run on device:  rkadk_dual_disp_test -a /etc/iqfiles -p /data/rkadk"
