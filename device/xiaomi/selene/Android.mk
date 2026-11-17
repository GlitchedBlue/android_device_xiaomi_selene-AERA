# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0

LOCAL_PATH := $(call my-dir)

ifeq ($(TARGET_DEVICE),selene)
include $(call all-subdir-makefiles,$(LOCAL_PATH))

# boot_a is 64 MiB and the AERA ramdisk already fills ~60 MiB of it. WLAN adds
# wpa_supplicant, wpa_cli, dhcptool, their libraries and the MTK connsys
# firmware, and AERA's networking stack also packs a 4.8 MiB incompressible
# rclone executable for the NAS transport. Drop rclone to stay inside the
# partition; WiFi itself does not use it. Same trade-off chiron makes.
SELENE_RCLONE_OUT := $(TARGET_RECOVERY_ROOT_OUT)/system/bin/rclone
SELENE_RAMDISK_TRIM_STAMP := $(TARGET_RECOVERY_ROOT_OUT)/.selene-ramdisk-trim

$(SELENE_RAMDISK_TRIM_STAMP): $(SELENE_RCLONE_OUT)
	@echo "selene: omitting rclone to stay within the 64 MiB boot partition"
	@rm -f $(SELENE_RCLONE_OUT) && touch $@

$(PRODUCT_OUT)/obj/PACKAGING/recovery_intermediates/ramdisk_files-timestamp: \
    $(SELENE_RAMDISK_TRIM_STAMP)
endif
