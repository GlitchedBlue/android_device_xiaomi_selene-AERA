#!/bin/bash
#
# AERA Recovery environment for the Xiaomi Redmi 10 (selene).

FDEVICE="selene"

aera_get_target_device() {
    local chkdev
    chkdev=$(echo "$BASH_SOURCE" | grep -w "$FDEVICE")
    if [ -n "$chkdev" ]; then
        AERA_BUILD_DEVICE="$FDEVICE"
    else
        chkdev=$(set | grep BASH_ARGV | grep -w "$FDEVICE")
        [ -n "$chkdev" ] && AERA_BUILD_DEVICE="$FDEVICE"
    fi
}

if [ -z "$1" ] && [ -z "$AERA_BUILD_DEVICE" ]; then
    aera_get_target_device
fi

if [ "$1" = "$FDEVICE" ] || [ "$AERA_BUILD_DEVICE" = "$FDEVICE" ]; then
    export LC_ALL="C"
    export AERA_PRODUCT_PREFIX=AERA
    export AERA_BUILD_STATUS=Unofficial
    export AERA_BUILD_TYPE=Alpha
    export AERA_MAINTAINER="AERA Selene Port"
    # selene ships a stock Android 11 vendor image under an Android 16 recovery,
    # so the vendor/treble compatibility assertion would always fail.
    export AERA_NO_TREBLE_COMPATIBILITY_CHECK=1
    export AERA_VANILLA_BUILD=1
    export AERA_ENABLE_ROOT_MANAGER=0
    export AERA_ALLOW_EARLY_SETTINGS_LOAD=1
    # AERA_DELETE_AROMAFM / AERA_DELETE_MAGISK_ADDON were OrangeFox-era switches
    # and are dead in the Android 16 backend (nothing in aera_config.mk,
    # aera_build.mk or AERA_A16.sh reads them), so they are not set here.
    export AERA_USE_UPDATED_MAGISKBOOT=1
    export AERA_USE_BUSYBOX_BINARY=1
    export AERA_USE_DATE_BINARY=1
    export AERA_USE_GREP_BINARY=1
    export AERA_USE_SED_BINARY=1
    export AERA_USE_TAR_BINARY=1
    export AERA_USE_XZ_UTILS=1
    export AERA_USE_ZSTD_BINARY=1
    export AERA_USE_FSCK_EROFS_BINARY=1
    export AERA_SETTINGS_ROOT_DIRECTORY=/data/recovery
    export AERA_MISCELLANEOUS_ROOT_DIRECTORY=/sdcard

    # Xiaomi Redmi 10 (selene / selenes)
    export TARGET_DEVICE_ALT="selene,selenes"
    export AERA_TARGET_DEVICES="$TARGET_DEVICE_ALT"

    unset AERA_MAINTAINER_PATCH_VERSION
fi
