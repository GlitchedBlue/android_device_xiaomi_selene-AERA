#!/usr/bin/env bash
#
# AERA Recovery build for the Xiaomi Redmi 10 / Redmi 10 Prime (selene).
#
# This script runs INSIDE a Crave build job, not on your PC. The workflow
# streams it to the remote node with:
#
#   crave run --projectID <id> --no-artifacts --no-patch -- \
#     "GH_TOKEN=... bash -c 'echo <base64> | base64 -d | bash'"
#
# It follows Crave's documented "unsupported ROM" recipe:
#   - start from the closest supported base project (Android 16)
#   - repo init the AERA manifest over it
#   - let /opt/crave/resync.sh fix up the sync conflicts
#   - drop in device/xiaomi/selene and build
#
# It deliberately never runs `rm -rf *`; Crave bans accounts that do.
#
set -o pipefail

GH_REPO="${GH_REPO:-GlitchedBlue/android_device_xiaomi_selene-AERA}"
GH_BRANCH="${GH_BRANCH:-aera-16.0}"
GH_TOKEN="${GH_TOKEN:-}"
AERA_MANIFEST="${AERA_MANIFEST:-https://github.com/AERA-Recovery/android_manifest.git}"
AERA_BRANCH="${AERA_BRANCH:-aera-16.0}"
DEVICE="${DEVICE:-selene}"
LUNCH_TARGET="${LUNCH_TARGET:-twrp_selene-bp2a-eng}"
BUILD_CMD="${BUILD_CMD:-mka adbd bootimage}"
JOBS="${JOBS:-$(nproc --all 2>/dev/null || echo 8)}"
ARTIFACT_DIR="${ARTIFACT_DIR:-crave-artifacts}"

banner() { printf '\n\033[1;36m########## %s ##########\033[0m\n' "$*"; }
fatal()  { printf '\n\033[1;31mFATAL: %s\033[0m\n' "$*" >&2; exit 1; }

banner "AERA $DEVICE build started $(date -u +%FT%TZ)"
echo "cwd     : $(pwd)"
echo "cores   : $JOBS"
echo "lunch   : $LUNCH_TARGET"
echo "build   : $BUILD_CMD"
free -g 2>/dev/null | head -2
df -h . | tail -1

# ---------------------------------------------------------------------------
# 0. Sanity: we must be sitting in the base project's workspace
# ---------------------------------------------------------------------------
[ -d .repo ] || fatal "no .repo here - this must run through 'crave run'"
[ -f build/envsetup.sh ] || fatal "no build/envsetup.sh - wrong workspace"
[ -x "$(command -v repo)" ] || fatal "the repo tool is missing on this node"

git config --global user.name  "${GIT_NAME:-AERA Builder}"
git config --global user.email "${GIT_EMAIL:-builder@localhost}"
git config --global --add safe.directory '*' 2>/dev/null || true

# ---------------------------------------------------------------------------
# 1. Point the workspace at the AERA manifest
# ---------------------------------------------------------------------------
banner "repo init -u $AERA_MANIFEST -b $AERA_BRANCH"
rm -rf .repo/local_manifests

if repo init -u "$AERA_MANIFEST" -b "$AERA_BRANCH" --git-lfs --no-repo-verify; then
    echo "repo init done (with git-lfs)"
elif repo init -u "$AERA_MANIFEST" -b "$AERA_BRANCH" --no-repo-verify; then
    echo "repo init done (without git-lfs)"
else
    fatal "repo init failed"
fi

# ---------------------------------------------------------------------------
# 2. Sync (Crave's conflict-tolerant wrapper)
# ---------------------------------------------------------------------------
banner "repo sync"
date -u +'sync start %FT%TZ'
if [ -x /opt/crave/resync.sh ]; then
    /opt/crave/resync.sh
else
    echo "/opt/crave/resync.sh not found - falling back to plain repo sync"
    find .repo -name '*.lock' -delete 2>/dev/null || true
    repo sync -c -j"$JOBS" --force-sync --no-clone-bundle --no-tags --prune \
        --optimized-fetch --force-remove-dirty
fi
sync_rc=$?
date -u +'sync end   %FT%TZ'
echo "SYNC_RC=$sync_rc"
[ "$sync_rc" -ne 0 ] && fatal "repo sync failed"

# ---------------------------------------------------------------------------
# 3. Install the device tree from GitHub
# ---------------------------------------------------------------------------
banner "Fetching device tree ($GH_REPO @ $GH_BRANCH)"
DT_SRC="/tmp/aera-device-tree"
rm -rf "$DT_SRC"

if [ -n "$GH_TOKEN" ]; then
    # Keep the token out of .git/config and out of the remote URL.
    export GIT_CONFIG_COUNT=1
    export GIT_CONFIG_KEY_0=http.extraheader
    export GIT_CONFIG_VALUE_0="Authorization: basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64 | tr -d '\n')"
fi

git clone --depth 1 --branch "$GH_BRANCH" "https://github.com/${GH_REPO}.git" "$DT_SRC" \
    || fatal "could not clone the device tree (is the token valid?)"

unset GIT_CONFIG_COUNT GIT_CONFIG_KEY_0 GIT_CONFIG_VALUE_0

rm -rf "device/xiaomi/$DEVICE"
mkdir -p device/xiaomi
cp -a "$DT_SRC/device/xiaomi/$DEVICE" "device/xiaomi/$DEVICE" \
    || fatal "could not copy the device tree"
chmod +x "device/xiaomi/$DEVICE/vendorsetup.sh" 2>/dev/null || true
echo "device tree files: $(find "device/xiaomi/$DEVICE" -type f | wc -l)"

# ---------------------------------------------------------------------------
# 4. Build
# ---------------------------------------------------------------------------
banner "envsetup + lunch $LUNCH_TARGET"
export ALLOW_MISSING_DEPENDENCIES=true
export LC_ALL=C
export BUILD_USERNAME="${BUILD_USERNAME:-crave}"
export BUILD_HOSTNAME="${BUILD_HOSTNAME:-crave}"

# shellcheck disable=SC1091
source build/envsetup.sh || fatal "could not source build/envsetup.sh"
lunch "$LUNCH_TARGET" || fatal "lunch $LUNCH_TARGET failed"

banner "Building: $BUILD_CMD"
date -u +'build start %FT%TZ'
$BUILD_CMD
build_rc=$?
date -u +'build end   %FT%TZ'
echo "BUILD_RC=$build_rc"

# ---------------------------------------------------------------------------
# 5. Collect the images so the runner can pull them
# ---------------------------------------------------------------------------
OUT="out/target/product/$DEVICE"
banner "Outputs in $OUT"
ls -lh "$OUT" 2>/dev/null | head -60 || true

rm -rf "$ARTIFACT_DIR"
mkdir -p "$ARTIFACT_DIR"
for f in "$OUT"/AERA-*.img "$OUT"/boot.img "$OUT"/recovery.img "$OUT"/dtbo.img; do
    if [ -f "$f" ]; then
        cp -f "$f" "$ARTIFACT_DIR/" && echo "  collected $(basename "$f")"
    fi
done
ls -lh "$ARTIFACT_DIR" 2>/dev/null || true

banner "Finished $(date -u +%FT%TZ) rc=$build_rc"
exit "$build_rc"
