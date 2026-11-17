#!/system/bin/sh
#
# MTK connsys / WLAN bring-up for the Redmi 10 (MT6768, selene).
#
# Run as an init oneshot service, in one of two modes:
#
#   --boot      after `on boot`: power connsys, download the patch, run
#               calibration, then turn the WLAN function on so wlan0 exists
#   (no args)   when AERA's UI sets sys.aera.wlan.up=1: make sure wlan0 is
#               there, bring it up and start wpa_supplicant
#
# The kernel side is already built in; this is only the vendor power sequence.
#
# Ordering matters and the failure mode is silent: writing 1 to /dev/wmtWifi
# before the launcher has finished its ROM patch download injects a WMT command
# into the STP stream the launcher is using. The core then asserts
# (`wmt_core_trigger_assert: Send Assert command !`) and every later WIFI_write
# fails with `WMT turn on WIFI fail!` until the next reboot. The old script
# waited on vendor.connsys.driver.ready, which the driver sets long before the
# patch download finishes -- it was already "yes" at boot, so the script powered
# WLAN on immediately and killed the chip. Wait for the launcher's own
# "Calibration end" instead.

LOG=/tmp/recovery.log

log() {
    echo "AERA WLAN: $*" >> "$LOG"
}

# wait_for <path> <tenths of a second>
wait_for() {
    local i=0
    while [ ! -e "$1" ] && [ "$i" -lt "$2" ]; do
        sleep 0.1
        i=$((i + 1))
    done
    [ -e "$1" ]
}

# wait_calibration <seconds>: the launcher prints "Calibration end" once the
# ROM patch is downloaded and the chip is ready for WLAN.
wait_calibration() {
    local i=0
    while [ "$i" -lt "$1" ]; do
        dmesg | grep -q 'Calibration end' && return 0
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# Power the connectivity SoC, start the launcher and wait for calibration.
# bringup [seconds]: the boot pass allows 90 s, the on-demand path less.
bringup() {
    local wait_s="${1:-90}"
    /vendor/bin/wmt_loader
    log "wmt_loader rc=$?"
    wait_for /dev/stpwmt 50 || log "no /dev/stpwmt after wmt_loader"

    /vendor/bin/wmt_launcher -p /vendor/firmware/ -o 1 &
    log "wmt_launcher started"

    if wait_calibration "$wait_s"; then
        log "calibration done"
        sleep 2
        return 0
    fi
    log "no calibration after $wait_s s"
    return 1
}

# Some kernels build the WLAN driver as modules instead of linking it in (no
# CONFIG_WLAN_DRV_BUILD_IN). The connsys power sequence still succeeds and
# /dev/wmtWifi still accepts the power-on, but nothing registers a netdev, so
# wlan0 never appears and the failure looks identical to a broken power
# sequence. Load what the kernel shipped, in dependency order, from the rescue
# blob store first and then the vendor/system module dirs.
#
# insmod is allowed to fail: a .ko built for another kernel fails with
# "invalid module format" and there is nothing to do about it here, but the
# in-tree case needs no module at all, so log and carry on either way.
load_wlan_modules() {
    local dir mod
    for dir in /gpu-rescue/aera-modules /vendor/lib/modules /system/lib/modules; do
        [ -d "$dir" ] || continue
        for mod in wmt_drv.ko wmt_chrdev_wifi.ko wlan_drv_gen4m.ko; do
            [ -f "$dir/$mod" ] || continue
            grep -q "^${mod%.ko} " /proc/modules && continue
            insmod "$dir/$mod" 2>>"$LOG"
            log "insmod $dir/$mod rc=$?"
        done
    done
    : > /tmp/.aera-wlan-modules-tried
}

# Writing 1 to /dev/wmtWifi asks the WLAN driver to power the WiFi function and
# register wlan0. Do not write it again once wlan0 exists: a second power-on
# request on an already-running function is rejected and logs
# `WMT turn on WIFI fail!`, which reads like a failure in every later log.
power_on_wifi() {
    local i=0
    [ -e /sys/class/net/wlan0 ] && return 0
    [ -e /tmp/.aera-wlan-modules-tried ] || load_wlan_modules
    while [ "$i" -lt "$1" ]; do
        if [ -e /dev/wmtWifi ]; then
            echo 1 > /dev/wmtWifi
            [ -e /sys/class/net/wlan0 ] && return 0
        fi
        sleep 0.2
        i=$((i + 1))
    done
    [ -e /sys/class/net/wlan0 ]
}

if [ "$1" = "--boot" ]; then
    # Let the kernel's connectivity driver finish probing. Running wmt_loader
    # in the first second of boot fails the STP handshake.
    sleep 10
    if bringup; then
        if power_on_wifi 150; then
            log "wlan0 registered"
        else
            log "wlan0 did not appear at boot"
        fi
    else
        log "boot bring-up failed"
    fi
    exit 0
fi

# On demand. wlan.cpp gives this 20 s (it waits for init.svc.wpa_supplicant to
# reach "running"), so nothing here may block longer than that. When the boot
# pass worked, wlan0 already exists and this is just ifconfig + the service
# start. When it did not, ask the driver once more: the launcher may still be
# alive and only the power-on request was lost.
if [ ! -e /sys/class/net/wlan0 ]; then
    log "on-demand: wlan0 missing, asking the driver again"
    power_on_wifi 50
fi

if [ ! -e /sys/class/net/wlan0 ]; then
    # Last resort: the boot pass never got the chip up. This takes ~15 s at
    # best and will miss AERA's window, but a late supplicant is still better
    # than none, and the next tap finds the interface already there.
    log "on-demand: full bring-up"
    bringup 20 && power_on_wifi 50
fi

if [ ! -e /sys/class/net/wlan0 ]; then
    log "on-demand: no wlan0"
    exit 1
fi

mkdir -p /tmp/recovery/sockets
chmod 777 /tmp/recovery/sockets

/system/bin/ifconfig wlan0 up
setprop wifi.driver.ready true
setprop ctl.start wpa_supplicant
log "wpa_supplicant start requested"
