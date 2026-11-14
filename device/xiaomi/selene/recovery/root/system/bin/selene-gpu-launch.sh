#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Stage selene's stock Mali graphics stack before AERA starts, when the stock
# partitions happen to be reachable that early.
#
# selene ships an Android 13 MediaTek vendor image (mt6768, Mali-G52 MC2).
# AERA 16 cannot link those prebuilts into its own process, so the Mali backend
# (aeraui/core/mali_display.cpp) loads them through a private linker namespace.
# The backend needs a gralloc1 module and an HWC2 module; if either is missing,
# initialization fails and AERA keeps the software renderer.
#
# IMPORTANT: selene uses dynamic partitions. `vendor` lives inside `super`, so
# there is no /dev/block/by-name/vendor until something maps the logical
# partition, and AERA only does that in-process when it mounts a volume - well
# after `on boot`. So on this device this hook normally finds nothing and exits
# silently; the Mali backend then logs a driver-loading failure and falls back.
# Use /system/bin/selene-gpu-test.sh (run manually after /vendor is mounted) for
# the real diagnostic.
#
# The stock partitions are mounted read-only under /gpu. Nothing on the real
# partitions is modified.
#
# This script runs from `on boot` in init.recovery.mt6768.rc. It must NOT exec
# /system/bin/recovery: init starts the recovery service itself after the boot
# actions, so this script only stages files and exits.

export PATH=/system/bin:/sbin:/vendor/bin
LOG=/tmp/aera-gpu-boot.log
: > "$LOG"

mkdir -p /gpu /gpu/stock-system /gpu/stock-vendor

log() { echo "$*" >> "$LOG"; }

mount_ro() {
    target="$2"
    if grep -qs " $target " /proc/mounts; then
        log "already mounted: $target"
        return 0
    fi
    for device in $1; do
        [ -b "$device" ] || continue
        if mount -o ro "$device" "$target" 2>/dev/null; then
            log "mounted $device -> $target"
            return 0
        fi
    done
    log "could not mount $target"
    return 1
}

# If recovery's own init already mounted the real volumes, reuse them.
if [ -d /vendor/lib64 ]; then
    mount --bind /vendor /gpu/stock-vendor 2>/dev/null &&
        log "bound /vendor -> /gpu/stock-vendor"
fi
[ -d /gpu/stock-vendor/lib64 ] ||
    mount_ro "/dev/block/mapper/vendor /dev/block/by-name/vendor" /gpu/stock-vendor

if [ -d /system/lib64 ] && [ ! -d /gpu/stock-system/system/lib64 ]; then
    mount --bind /system /gpu/stock-system 2>/dev/null &&
        log "bound /system -> /gpu/stock-system"
fi
[ -d /gpu/stock-system/system/lib64 ] ||
    mount_ro "/dev/block/mapper/system /dev/block/by-name/system" /gpu/stock-system

for f in \
    /gpu/stock-vendor/lib64/egl/libGLES_mali.so \
    /gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so \
    /gpu/stock-vendor/lib64/hw/gralloc.mt6768.so; do
    if [ -f "$f" ]; then
        log "present: $f"
    else
        log "missing: $f"
    fi
done

if [ -d /gpu/stock-vendor/lib64 ] && [ -d /gpu/stock-system/system/lib64 ]; then
    sh /system/bin/selene-gpu-libraries.sh >> "$LOG" 2>&1 || log "library staging failed"
else
    log "stock system/vendor not reachable at boot; GPU backend will fall back to software"
fi

log "done"
