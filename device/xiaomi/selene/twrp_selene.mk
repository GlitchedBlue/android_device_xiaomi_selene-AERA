# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0

PRODUCT_RELEASE_NAME := selene

$(call inherit-product, $(SRC_TARGET_DIR)/product/base.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)
$(call inherit-product, vendor/twrp/config/common.mk)
$(call inherit-product, device/xiaomi/selene/device.mk)

PRODUCT_DEVICE := selene
PRODUCT_SHIPPING_API_LEVEL := 30
PRODUCT_NAME := twrp_selene
PRODUCT_BRAND := Xiaomi
PRODUCT_MODEL := Redmi 10
PRODUCT_MANUFACTURER := Xiaomi

# Stock global firmware: V12.5.20.0.RKUMIXM (Android 11)
PRODUCT_BUILD_PROP_OVERRIDES += \
    PRIVATE_BUILD_DESC="selene-user 11 RKQ1.201217.002 V12.5.20.0.RKUMIXM release-keys"

BUILD_FINGERPRINT := Xiaomi/selene/selene:11/RKQ1.201217.002/V12.5.20.0.RKUMIXM:user/release-keys
