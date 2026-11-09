#!/usr/bin/env bash
#
# Attach a Linux machine to GitHub Actions as a self-hosted runner for the
# AERA selene repo. This is the standard way AOSP/recovery projects "build on
# GitHub": GitHub queues the job, the runner machine does the compile.
#
# Run on the Linux machine that will build (Ubuntu 22.04/24.04, >=16 GB RAM,
# >=300 GB free SSD, 8+ cores):
#
#   GITHUB_TOKEN=ghp_xxx ./scripts/setup-runner.sh
#
# The token needs `repo` + `workflow` (classic) or Contents/Workflows/Admin
# (fine-grained). It is only used to register the runner.
#
set -euo pipefail

REPO="${REPO:-GlitchedBlue/android_device_xiaomi_selene-AERA}"
GITHUB_TOKEN="${GITHUB_TOKEN:-}"
RUNNER_DIR="${RUNNER_DIR:-$HOME/actions-runner}"
RUNNER_LABELS="${RUNNER_LABELS:-aera,linux}"

log() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
die() { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(uname -s)" = "Linux" ] || die "Run this on Linux."
[ -n "$GITHUB_TOKEN" ] || die "Set GITHUB_TOKEN to a token with repo+workflow scope."

# ---------------------------------------------------------------------------
# 1. Build dependencies
# ---------------------------------------------------------------------------
log "Installing build dependencies (sudo required)"
sudo apt-get update
sudo apt-get install -y \
    git curl python3 repo openjdk-17-jdk \
    bc bison build-essential ccache flex g++-multilib gcc-multilib \
    gnupg gperf imagemagick lib32ncurses5-dev lib32readline-dev \
    lib32z1-dev libelf-dev liblz4-tool libncurses5 libncurses5-dev \
    libsdl1.2-dev libssl-dev libxml2 libxml2-utils lzop pngcrush \
    rsync schedtool squashfs-tools xsltproc zip zlib1g-dev jq

# ---------------------------------------------------------------------------
# 2. Download the runner
# ---------------------------------------------------------------------------
log "Resolving latest actions/runner release"
RUNNER_VERSION=$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest \
    | sed -n 's/.*"tag_name": "v\([^"]*\)".*/\1/p')
[ -n "$RUNNER_VERSION" ] || die "Could not resolve the runner version."
log "Runner version: $RUNNER_VERSION"

mkdir -p "$RUNNER_DIR"
cd "$RUNNER_DIR"
if [ ! -f ./config.sh ]; then
    curl -fsSL -o runner.tar.gz \
        "https://github.com/actions/runner/releases/download/v${RUNNER_VERSION}/actions-runner-linux-x64-${RUNNER_VERSION}.tar.gz"
    tar xzf runner.tar.gz
    rm -f runner.tar.gz
fi

# ---------------------------------------------------------------------------
# 3. Registration token
# ---------------------------------------------------------------------------
log "Requesting runner registration token"
REG_TOKEN=$(curl -fsSL -X POST \
    -H "Authorization: Bearer ${GITHUB_TOKEN}" \
    -H "Accept: application/vnd.github+json" \
    "https://api.github.com/repos/${REPO}/actions/runners/registration-token" \
    | sed -n 's/.*"token": "\([^"]*\)".*/\1/p')
[ -n "$REG_TOKEN" ] || die "Could not get a registration token. Check the PAT scopes."

# ---------------------------------------------------------------------------
# 4. Configure + install the service
# ---------------------------------------------------------------------------
log "Configuring runner with labels: $RUNNER_LABELS"
./config.sh \
    --url "https://github.com/${REPO}" \
    --token "$REG_TOKEN" \
    --labels "$RUNNER_LABELS" \
    --unattended \
    --replace

log "Installing runner as a systemd service"
sudo ./svc.sh install "$USER"
sudo ./svc.sh start

log "Runner status"
sudo ./svc.sh status || true

cat <<EOF

Done. The runner is attached to https://github.com/${REPO}
and listens for jobs with labels: ${RUNNER_LABELS}

Trigger a build by pushing to the aera-16.0 branch, or from the Actions tab
(Run workflow -> Build AERA Recovery for selene).

The first run syncs AERA 16 into ~/android/AERA_16.0 and takes hours.
EOF
