# Copyright (C) 2026 AERA Recovery Project contributors
# SPDX-License-Identifier: Apache-2.0

AERA_MAINTAINER := AERA Selene Port
AERA_SCREEN_H := 2400
AERA_STATUS_H := 100
AERA_OPTIONS_LIST_NUM := 6
AERA_WORKAROUND_BACKUP_BUG := 1
AERA_NO_TREBLE_COMPATIBILITY_CHECK := 1

# The stock 4.14 MTK kernel has no usable WLAN firmware/libs in the recovery
# ramdisk. Disable WLAN until a working bring-up is done.
AERA_ENABLE_WLAN := 0
