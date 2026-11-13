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

# Items per page in the native options list.
AERA_OPTIONS_LIST_NUM := 6

# Work around the known backup failure on this device class.
# The backend already defaults this to 1 when unset, so this is explicit
# rather than load-bearing.
AERA_WORKAROUND_BACKUP_BUG := 1
