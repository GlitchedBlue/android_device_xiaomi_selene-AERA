# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0
#
# Product-side AERA configuration for the Xiaomi Redmi 10 (selene).
#
# Placement matters (AERA wiki, "Build configuration"):
#   * BoardConfig.mk      -> hardware, early build/Soong settings
#   * product make files  -> product behaviour and feature defaults
#   * vendorsetup.sh      -> environment exports, inside the device guard
# A flag in the wrong file is either a hard make error (readonly product vars)
# or silently ignored.

# The stock 4.14 MTK kernel has no usable WLAN firmware/libs in the recovery
# ramdisk. Disable WLAN until a working bring-up is done.
#
# Verified against the consumer: aera_config.mk maps this verbatim to
# OF_ENABLE_WLAN, and aera_defaults.go tests `getMakeVars(...) == "1"`, so
# "0" really does keep WLAN off.
AERA_ENABLE_WLAN := 0

# Deliberately not set (confirmed by AzzyC, the AERA peridot maintainer):
#
#   AERA_WORKAROUND_BACKUP_BUG
#       The workaround was merged into OrangeFox and no longer needs a flag.
#       aera_build.mk still carries the remnant -- it defaults
#       OF_WORKAROUND_BACKUP_BUG to 1 when unset -- so setting it changed
#       nothing either way.
#
#   AERA_OPTIONS_LIST_NUM
#       Still mapped (aera_config.mk:46 -> aera_build.mk:724) but the AERA UI
#       does not rely on it. Left at the built-in default.
