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
# libaera_boot_trim re-exports the missing symbol. The boot HAL's .rc override
# (recovery/root/system/etc/init/android.hardware.boot@1.2-service.rc) LD_PRELOADs
# it from /sbin, scoped to that one service. See bootctl_compat/trim_shim.cpp.
PRODUCT_PACKAGES += libaera_boot_trim

# ---------------------------------------------------------------------------
# Mali GPU backend (stock MediaTek graphics stack)
# ---------------------------------------------------------------------------
# apkreader's mali-testing backend reads these properties at runtime and loads
# the stock driver/HALs through a private linker namespace. The launch script
# (recovery/root/system/bin/selene-gpu-launch.sh) stages the real system/vendor
# libraries into /gpu/lib64; /system/bin/selene-gpu-test.sh is the manual probe.
#
# Verified against the stock V14.0.7.0.TKUMIXM vendor dump (Android 13):
#
#   vendor/lib64/egl/libGLES_mali.so                                  39 MB
#   vendor/lib64/hw/hwcomposer.mt6768.so                             HWC2
#   vendor/lib64/hw/gralloc.default.so     AOSP fbdev gralloc0 (HMI, no gralloc1)
#   vendor/lib64/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so
#   vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so
#
# There is NO gralloc1 module anywhere on the device: gralloc.default.so is the
# generic fbdev gralloc0 module (fb_device_open + ashmem), and the MediaTek
# gralloc.mt6768.so that Android 10/11 trees shipped is gone. The backend calls
# gralloc1_open(), so ro.aera.gpu.gralloc deliberately names the absent module:
# Load() fails, MaliDisplay reports a display-HAL failure, and GpuRenderer
# returns false so AERA keeps the software renderer. Point it at a real gralloc1
# module and the backend initializes without any other change.
#
# Do not point it at gralloc.default.so: opening a gralloc0 device through
# gralloc1_open() hands the backend a mismatched function table.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.aera.gpu.mali_driver=/gpu/stock-vendor/lib64/egl/libGLES_mali.so \
    ro.aera.gpu.mali_libraries=/gpu/lib64 \
    ro.aera.gpu.gralloc=/gpu/stock-vendor/lib64/hw/gralloc.mt6768.so \
    ro.aera.gpu.hwcomposer=/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so \
    ro.aera.gpu.mali_permitted_paths=/gpu:/vendor

$(call inherit-product, $(DEVICE_PATH)/aera_selene.mk)
