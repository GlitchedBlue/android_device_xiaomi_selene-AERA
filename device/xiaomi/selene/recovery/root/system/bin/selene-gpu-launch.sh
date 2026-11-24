#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Stage selene's Mali graphics stack before AERA starts.
#
# The graphics blobs live in a blob store on the `rescue` partition
# (/dev/block/by-name/rescue, mmcblk0p50): a plain ext4 volume with no
# encryption and no device-mapper, so it is readable before anything maps
# `super`.
#
# That matters because selene uses dynamic partitions: `vendor` lives inside
# `super`, so there is no usable /vendor until AERA maps the logical partition
# in-process - which happens a second *after* the recovery UI initialises the
# GPU. Reading the blob store removes that ordering dependency entirely.
#
# Blob store layout (written once with `adb push`):
#
#   <rescue>/aera-gpu/vendor/lib64/{*.so, egl/*.so, hw/*.so}
#   <rescue>/aera-gpu/system/lib64/*.so
#
# Nothing on the real system/vendor partitions is modified.
#
# The blob store is used in place: ro.aera.gpu.mali_driver and friends name
# /gpu-rescue/... directly and /gpu/lib64 holds soname links into it. There is
# deliberately no bind mount - see the comment above
# PRODUCT_DEFAULT_PROPERTY_OVERRIDES in device.mk for why.
#
# This script runs from `on post-fs` in init.recovery.mt6768.rc, before init
# starts any service class, and `exec` makes init wait for it. It must NOT exec
# /system/bin/recovery: init starts the recovery service itself.

export PATH=/system/bin:/sbin:/vendor/bin
LOG=/tmp/aera-gpu-boot.log
: > "$LOG"

log() { echo "$*" >> "$LOG"; }

BLOB=/gpu-rescue/aera-gpu

mkdir -p /gpu /gpu-rescue

# Mount the blob store read-only. It is a plain block device, so this works
# regardless of what init has or has not mounted yet.
if ! grep -qs " /gpu-rescue " /proc/mounts; then
    mount -o ro /dev/block/by-name/rescue /gpu-rescue 2>/dev/null &&
        log "mounted rescue -> /gpu-rescue" ||
        log "could not mount rescue"
fi

for f in \
    "$BLOB/vendor/lib64/egl/libGLES_mali.so" \
    "$BLOB/vendor/lib64/hw/hwcomposer.mt6768.so" \
    "$BLOB/vendor/lib64/hw/gralloc.mt6768.so"; do
    if [ -f "$f" ]; then
        log "present: $f"
    else
        log "missing: $f"
    fi
done

if [ -f "$BLOB/vendor/lib64/egl/libGLES_mali.so" ]; then
    sh /system/bin/selene-gpu-libraries.sh >> "$LOG" 2>&1 || log "library staging failed"
    log "staged: $(ls /gpu/lib64 2>/dev/null | wc -l) libraries in /gpu/lib64"
else
    log "blob store empty at $BLOB; GPU backend will fall back to software"
fi

log "done"
