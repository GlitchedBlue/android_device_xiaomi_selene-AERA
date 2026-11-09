#!/usr/bin/env bash
#
# Build AERA Recovery for the Xiaomi Redmi 10 (selene).
#
# Requirements (Linux only):
#   - Ubuntu 22.04 / 24.04 (or a compatible distribution)
#   - 16 GB RAM minimum, 32 GB recommended
#   - 300 GB free disk space on an SSD
#   - repo, git, curl, OpenJDK 17, and the standard AOSP build packages
#
# Usage:
#   ./build-aera-selene.sh              # sync + build
#   AERA_SRC=~/android/AERA_16.0 ./build-aera-selene.sh
#   JOBS=8 ./build-aera-selene.sh
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${AERA_SRC:-$HOME/android/AERA_16.0}"
MANIFEST="https://github.com/AERA-Recovery/android_manifest.git"
BRANCH="aera-16.0"
JOBS="${JOBS:-$(nproc --all 2>/dev/null || echo 4)}"
DEVICE="selene"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Sanity checks
# ---------------------------------------------------------------------------
[ "$(uname -s)" = "Linux" ] || die "AERA must be built on Linux. Use WSL2/Ubuntu, a Linux box, or a Linux CI runner."
command -v repo >/dev/null 2>&1 || die "'repo' is not installed. See https://source.android.com/setup/develop#installing-repo"
command -v git  >/dev/null 2>&1 || die "'git' is not installed."

if [ -d "$SRC" ]; then
    free_gb=$(df -Pk "$SRC" | awk 'NR==2 {printf "%d", $4/1024/1024}')
else
    free_gb=$(df -Pk "$(dirname "$SRC")" | awk 'NR==2 {printf "%d", $4/1024/1024}')
fi
[ "${free_gb:-0}" -ge 250 ] || die "Only ${free_gb} GB free. AERA needs roughly 300 GB."

mem_gb=$(awk '/MemTotal/ {printf "%d", $2/1024/1024}' /proc/meminfo)
[ "${mem_gb:-0}" -ge 15 ] || log "WARNING: ${mem_gb} GB RAM detected; 16 GB minimum, 32 GB recommended."

# ---------------------------------------------------------------------------
# Sync the AERA manifest
# ---------------------------------------------------------------------------
mkdir -p "$SRC"
cd "$SRC"

if [ ! -d .repo ]; then
    log "Initialising AERA manifest in $SRC"
    repo init -u "$MANIFEST" -b "$BRANCH"
fi

log "Syncing AERA source (this takes a while on the first run)"
repo sync -c --force-sync --no-clone-bundle --no-tags -j"$JOBS"

# ---------------------------------------------------------------------------
# Install the device tree
# ---------------------------------------------------------------------------
log "Installing device/xiaomi/$DEVICE"
mkdir -p "device/xiaomi"
if command -v rsync >/dev/null 2>&1; then
    rsync -a --delete "$SCRIPT_DIR/device/xiaomi/$DEVICE/" "device/xiaomi/$DEVICE/"
else
    rm -rf "device/xiaomi/$DEVICE"
    cp -a "$SCRIPT_DIR/device/xiaomi/$DEVICE" "device/xiaomi/$DEVICE"
fi

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
log "Building twrp_$DEVICE-bp2a-eng"
set +u
# shellcheck disable=SC1091
source build/envsetup.sh
lunch "twrp_${DEVICE}-bp2a-eng"
set -u

# selene is recovery-as-boot, so the recovery image is boot.img.
mka adbd bootimage -j"$JOBS"

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
OUT_DIR="out/target/product/$DEVICE"
log "Build finished. Artifacts in $OUT_DIR:"
ls -lh "$OUT_DIR"/AERA-*.img "$OUT_DIR"/boot.img 2>/dev/null || true
echo
echo "Flash the AERA boot image with:"
echo "  fastboot flash boot $OUT_DIR/AERA-R1.0-Alpha-Unofficial-$DEVICE.img"
echo "Read docs/FLASHING.md before flashing."
