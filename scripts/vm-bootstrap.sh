#!/usr/bin/env bash
#
# One-shot AERA Recovery build bootstrap for the Xiaomi Redmi 10 (selene).
#
# Run this on a fresh Ubuntu 22.04 / 24.04 machine (VM, cloud instance, or
# bare-metal) with:
#   - 16 GB RAM minimum (32 GB recommended)
#   - 300 GB free disk on an SSD (400 GB recommended)
#   - 8+ CPU cores
#
# Usage:
#   GITHUB_TOKEN=ghp_xxx ./scripts/vm-bootstrap.sh
#   REPO_URL=https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA.git \
#     AERA_SRC=$HOME/android/AERA_16.0 JOBS=12 ./scripts/vm-bootstrap.sh
#
# GITHUB_TOKEN is only needed while the device-tree repo is private. It is
# also used, if `gh` is installed, to attach the built image to a release.
#
set -euo pipefail

REPO_URL="${REPO_URL:-https://github.com/GlitchedBlue/android_device_xiaomi_selene-AERA.git}"
REPO_BRANCH="${REPO_BRANCH:-aera-16.0}"
AERA_SRC="${AERA_SRC:-$HOME/android/AERA_16.0}"
JOBS="${JOBS:-$(nproc --all 2>/dev/null || echo 4)}"
GITHUB_TOKEN="${GITHUB_TOKEN:-}"
GIT_NAME="${GIT_NAME:-AERA Builder}"
GIT_EMAIL="${GIT_EMAIL:-builder@localhost}"
WORKDIR="${WORKDIR:-$HOME/aera-selene-build}"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "Run this on Linux (Ubuntu 22.04/24.04)."

# ---------------------------------------------------------------------------
# 1. Packages
# ---------------------------------------------------------------------------
log "Installing build dependencies (sudo required)"
sudo apt-get update
sudo apt-get install -y \
    git curl python3 repo openjdk-17-jdk \
    bc bison build-essential ccache flex g++-multilib gcc-multilib \
    gnupg gperf imagemagick lib32ncurses5-dev lib32readline-dev \
    lib32z1-dev libelf-dev liblz4-tool libncurses5 libncurses5-dev \
    libsdl1.2-dev libssl-dev libxml2 libxml2-utils lzop pngcrush \
    rsync schedtool squashfs-tools xsltproc zip zlib1g-dev

# ---------------------------------------------------------------------------
# 2. repo launcher
# ---------------------------------------------------------------------------
if ! command -v repo >/dev/null 2>&1; then
    log "Installing the repo launcher"
    mkdir -p "$HOME/bin"
    curl -fsSL https://storage.googleapis.com/git-repo-downloads/repo > "$HOME/bin/repo"
    chmod +x "$HOME/bin/repo"
    export PATH="$HOME/bin:$PATH"
fi

git config --global user.name  "$GIT_NAME"
git config --global user.email "$GIT_EMAIL"

# ---------------------------------------------------------------------------
# 3. Sanity checks
# ---------------------------------------------------------------------------
mkdir -p "$AERA_SRC"
free_gb=$(df -Pk "$AERA_SRC" | awk 'NR==2 {printf "%d", $4/1024/1024}')
[ "${free_gb:-0}" -ge 250 ] || die "Only ${free_gb} GB free in $AERA_SRC; need ~300 GB."
mem_gb=$(awk '/MemTotal/ {printf "%d", $2/1024/1024}' /proc/meminfo)
[ "${mem_gb:-0}" -ge 15 ] || log "WARNING: ${mem_gb} GB RAM; 16 GB minimum, 32 GB recommended."

# ---------------------------------------------------------------------------
# 4. Clone the project repo
# ---------------------------------------------------------------------------
log "Cloning $REPO_URL ($REPO_BRANCH)"
if [ -n "$GITHUB_TOKEN" ]; then
    CLONE_URL="https://x-access-token:${GITHUB_TOKEN}@github.com/GlitchedBlue/android_device_xiaomi_selene-AERA.git"
else
    CLONE_URL="$REPO_URL"
fi
rm -rf "$WORKDIR"
git clone --depth 1 --branch "$REPO_BRANCH" "$CLONE_URL" "$WORKDIR"

# ---------------------------------------------------------------------------
# 5. Sync AERA + build
# ---------------------------------------------------------------------------
log "Starting the AERA build (this takes hours)"
cd "$WORKDIR"
AERA_SRC="$AERA_SRC" JOBS="$JOBS" ./build-aera-selene.sh

# ---------------------------------------------------------------------------
# 6. Report / optional release upload
# ---------------------------------------------------------------------------
OUT="$AERA_SRC/out/target/product/selene"
log "Build finished. Artifacts in $OUT:"
ls -lh "$OUT"/AERA-*.img "$OUT"/boot.img 2>/dev/null || true

if [ -n "$GITHUB_TOKEN" ] && command -v gh >/dev/null 2>&1; then
    IMG=$(ls "$OUT"/AERA-*.img 2>/dev/null | head -n1 || true)
    if [ -n "$IMG" ]; then
        TAG="selene-$(date +%Y%m%d-%H%M%S)"
        log "Uploading $IMG to release $TAG"
        echo "$GITHUB_TOKEN" | gh auth login --with-token
        gh release create "$TAG" "$IMG" \
            --repo GlitchedBlue/android_device_xiaomi_selene-AERA \
            --title "AERA selene $TAG" \
            --notes "Automated build of $(basename "$IMG")" \
            || log "Release upload failed; the image is still at $IMG"
    fi
fi
