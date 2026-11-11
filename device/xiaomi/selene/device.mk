# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0
#
# Product-side configuration for the Xiaomi Redmi 10 (selene).

DEVICE_PATH := device/xiaomi/selene

# ---------------------------------------------------------------------------
# Android 16 prebuilt validation
# ---------------------------------------------------------------------------
# The recovery ramdisk ships the stock MediaTek keymaster/gatekeeper/boot HAL
# binaries, which are built for 4 KiB pages. Android 16 otherwise rejects any
# prebuilt ELF that is not 16 KiB-page aligned.
PRODUCT_CHECK_PREBUILT_MAX_PAGE_SIZE := false

# ---------------------------------------------------------------------------
# A/B / virtual A/B
# ---------------------------------------------------------------------------
AB_OTA_POSTINSTALL_CONFIG += \
    RUN_POSTINSTALL_system=true \
    POSTINSTALL_PATH_system=system/bin/otapreopt_script \
    FILESYSTEM_TYPE_system=ext4 \
    POSTINSTALL_OPTIONAL_system=true

ENABLE_VIRTUAL_AB := true
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota.mk)

PRODUCT_USE_DYNAMIC_PARTITIONS := true

# ---------------------------------------------------------------------------
# API / VNDK level of the installed vendor image
# ---------------------------------------------------------------------------
PRODUCT_SHIPPING_API_LEVEL := 30
PRODUCT_TARGET_VNDK_VERSION := 31

# ---------------------------------------------------------------------------
# fastbootd
# ---------------------------------------------------------------------------
# AERA compiles fastbootd into the recovery binary through
# librecovery_fastboot when AERA_INCLUDE_FASTBOOTD is set. There is no
# separate `fastbootd` module in AERA 16, so do not add one to
# PRODUCT_PACKAGES here.

# ---------------------------------------------------------------------------
# Namespaces
# ---------------------------------------------------------------------------
PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH)

$(call inherit-product, $(DEVICE_PATH)/aera_selene.mk)
