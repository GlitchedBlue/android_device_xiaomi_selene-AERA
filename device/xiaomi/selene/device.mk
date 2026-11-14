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

# ---------------------------------------------------------------------------
# Boot-control HAL ABI shim
# ---------------------------------------------------------------------------
# recovery/root ships the Android 11 MediaTek boot HAL prebuilt, which imports
# android::base::Trim(std::string const&). Android 13 replaced that overload
# with a std::string_view one, so the prebuilt cannot be dlopen()ed on Android
# 16 and boot-hal-1-2 crash-loops while recovery waits forever in
# IBootControl::getService().
#
# libaera_boot_trim re-exports the missing symbol; init.recovery.mt6768.rc
# LD_PRELOADs it into the HAL. See bootctl_compat/trim_shim.cpp.
PRODUCT_PACKAGES += libaera_boot_trim

$(call inherit-product, $(DEVICE_PATH)/aera_selene.mk)
