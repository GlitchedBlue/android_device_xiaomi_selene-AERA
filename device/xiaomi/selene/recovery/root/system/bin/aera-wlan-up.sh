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

# Writing 1 to /dev/wmtWifi asks the WLAN driver to power the WiFi function and
# register wlan0. Until the launcher has finished its patch download this would
# inject a WMT command into the STP stream the launcher is using, so wait for
# the driver.ready property it sets at the end instead of guessing a sleep.
power_on_wifi() {
    local i=0
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
    /vendor/bin/wmt_loader
    log "wmt_loader rc=$?"
    wait_for /dev/stpwmt 50 || log "no /dev/stpwmt after wmt_loader"

    /vendor/bin/wmt_launcher -p /vendor/firmware/ -o 1 &
    log "wmt_launcher started"

    # 30 s is generous: the patch download plus calibration took ~15 s on the
    # first cold boot and less afterwards.
    i=0
    while [ "$i" -lt 300 ]; do
        [ "$(getprop vendor.connsys.driver.ready)" = "yes" ] && break
        sleep 0.1
        i=$((i + 1))
    done
    log "launcher ready after $((i / 10)) s"

    wait_for /dev/wmtWifi 50 || log "no /dev/wmtWifi after launcher"

    if power_on_wifi 150; then
        log "wlan0 registered"
    else
        log "wlan0 did not appear at boot"
    fi
    exit 0
fi

# On demand. If the boot pass worked this returns almost immediately.
wait_for /dev/wmtWifi 200 || log "on-demand: no /dev/wmtWifi"
if ! power_on_wifi 150; then
    log "on-demand: no wlan0"
    exit 1
fi

mkdir -p /tmp/recovery/sockets
chmod 777 /tmp/recovery/sockets

/system/bin/ifconfig wlan0 up
setprop wifi.driver.ready true
setprop ctl.start wpa_supplicant
log "wpa_supplicant start requested"
