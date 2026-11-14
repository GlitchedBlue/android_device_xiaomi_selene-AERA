#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Manual probe for selene's stock Mali graphics stack.
#
# Run this AFTER /vendor is mounted (Mount menu, or `aera mount /vendor`):
#
#     adb shell sh /system/bin/selene-gpu-test.sh
#     adb pull /tmp/aera-gpu-probe.txt
#
# The boot-time hook (selene-gpu-launch.sh) cannot do this job: selene uses
# dynamic partitions, so /vendor lives inside `super` and has no /dev/block
# by-name node until something maps it. AERA maps logical partitions in-process
# when it mounts a volume, which happens long after `on boot`.
#
# This script mounts the stock partitions read-only under /gpu, stages their
# libraries into /gpu/lib64 exactly the way selene-gpu-launch.sh does, then asks
# the dynamic linker to resolve each HAL library. Nothing is modified and
# nothing is started.

export PATH=/system/bin:/sbin:/vendor/bin

OUT=/tmp/aera-gpu-probe.txt
: > "$OUT"
log() { echo "$*" >> "$OUT"; }

log "== selene GPU probe =="
log "date: $(date)"
log "ro.aera.gpu.mali_driver=$(getprop ro.aera.gpu.mali_driver)"
log "ro.aera.gpu.gralloc=$(getprop ro.aera.gpu.gralloc)"
log "ro.aera.gpu.hwcomposer=$(getprop ro.aera.gpu.hwcomposer)"

log ""
log "== kernel GPU nodes =="
ls -l /dev/mali0 /dev/graphics/fb0 2>&1 | sed 's/^/  /' >> "$OUT"
[ -r /sys/class/misc/mali0/device/gpuinfo ] &&
    log "gpuinfo: $(cat /sys/class/misc/mali0/device/gpuinfo)"

log ""
log "== block devices =="
ls -l /dev/block/by-name/ 2>/dev/null | grep -E "super|system|vendor|product" | sed 's/^/  /' >> "$OUT"
ls -l /dev/block/mapper/ 2>/dev/null | sed 's/^/  /' >> "$OUT"

mkdir -p /gpu /gpu/stock-system /gpu/stock-vendor

mount_ro() {
    target=$2
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

# /vendor is normally already mounted by AERA; use it as-is rather than
# remounting the block device.
if [ -d /vendor/lib64 ] && ! grep -qs " /gpu/stock-vendor " /proc/mounts; then
    mount --bind /vendor /gpu/stock-vendor 2>/dev/null &&
        log "bound /vendor -> /gpu/stock-vendor"
fi
[ -d /gpu/stock-vendor/lib64 ] ||
    mount_ro "/dev/block/mapper/vendor /dev/block/by-name/vendor" /gpu/stock-vendor

if [ -d /system/lib64 ] && [ ! -d /gpu/stock-system/system/lib64 ]; then
    mount --bind /system /gpu/stock-system 2>/dev/null &&
        log "bound /system -> /gpu/stock-system"
fi

log ""
log "== stock graphics pieces =="
for f in \
    /gpu/stock-vendor/lib64/egl/libGLES_mali.so \
    /gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so \
    /gpu/stock-vendor/lib64/hw/gralloc.mt6768.so \
    /gpu/stock-vendor/lib64/hw/gralloc.default.so \
    /gpu/stock-vendor/lib64/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so \
    /gpu/stock-vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so; do
    if [ -f "$f" ]; then
        log "present: $f ($(stat -c %s "$f") bytes)"
    else
        log "MISSING: $f"
    fi
done

if [ -d /gpu/stock-vendor/lib64 ] && [ -d /gpu/stock-system/system/lib64 ]; then
    sh /system/bin/selene-gpu-libraries.sh >> "$OUT" 2>&1 ||
        log "library staging failed"
else
    log "stock system/vendor unavailable; cannot stage /gpu/lib64"
    log "probe finished (incomplete)"
    echo "wrote $OUT"
    exit 1
fi

probe() {
    log ""
    log "---- linker64 --list $1 ----"
    LD_LIBRARY_PATH=/gpu/lib64:/gpu/stock-vendor/lib64:/gpu/stock-system/system/lib64 \
        /system/bin/linker64 --list "$1" 2>&1 | sed 's/^/  /' >> "$OUT"
}

probe /gpu/stock-vendor/lib64/egl/libGLES_mali.so
probe /gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so
probe /gpu/stock-vendor/lib64/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so
probe /gpu/stock-vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so

log ""
log "== gralloc1 module search =="
# AERA's Mali backend needs a gralloc1 module (gralloc1_open + the
# GRALLOC1_FUNCTION_* table). selene's stock vendor ships only AOSP's fbdev
# gralloc0 module (gralloc.default.so) and the HIDL allocator@4.0/mapper@4.0
# implementations, so this is expected to come back empty.
found=0
for f in /gpu/stock-vendor/lib64/hw/*.so; do
    [ -f "$f" ] || continue
    if grep -qa "gralloc1_open\|GRALLOC1_FUNCTION" "$f" 2>/dev/null; then
        log "gralloc1 candidate: $f"
        found=1
    fi
done
[ "$found" = 0 ] && log "no gralloc1 module on vendor"

log ""
log "probe finished"
echo "wrote $OUT"
