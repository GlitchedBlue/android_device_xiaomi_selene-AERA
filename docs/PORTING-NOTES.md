# Porting notes — AERA on selene

## What was adapted

The tree combines two sources:

| Source | What it provides |
| --- | --- |
| `AERA-Recovery/android_device_xiaomi_chiron-AERA` | AERA device-tree format, `AERA_*` build flags, product/lunch layout, output naming |
| `PitchBlackRecoveryProject/android_device_xiaomi_selene-pbrp` | selene hardware: kernel/DTB/DTBO, MTK HALs, fstab, `twrp.flags`, init scripts, crypto properties |

## Key mappings

| AERA flag | Value for selene | Why |
| --- | --- | --- |
| `BOARD_USES_RECOVERY_AS_BOOT` | `true` | selene has no `recovery` partition; recovery lives in `boot` |
| `AB_OTA_UPDATER` | `true` | A/B slots (`_a` / `_b`) |
| `ENABLE_VIRTUAL_AB` | `true` | stock firmware is virtual A/B |
| `PRODUCT_USE_DYNAMIC_PARTITIONS` | `true` | `super` holds system/vendor/product |
| `AERA_INCLUDE_CRYPTO*` | `true` | FBE v2 + metadata encryption |
| `AERA_INCLUDE_FBE_METADATA_DECRYPT` | `true` | `keydirectory=/metadata/vold/metadata_encryption` |
| `AERA_USE_FSCRYPT_POLICY` | `1` | the vendor fstab uses `fileencryption=...:v2` |
| `AERA_ENABLE_WLAN` | `0` | no usable WLAN firmware/libs in the recovery ramdisk |
| `AERA_UI_LEGACY_KGSL_GPU` | **not set** | that is a Qualcomm KGSL path; selene is MTK/Mali with fbdev |
| `AERA_SKIP_ADDITIONAL_FSTAB` | **not set** | the vendor `fstab.mt6768` is needed for decryption |

## Hardware details used

- Platform: `mt6768`, boot header v2, kernel base `0x40078000`,
  page size `2048`, ramdisk offset `0x07c08000`, tags offset `0x0bc08000`.
- Boot image size: 64 MiB (`0x4000000`).
- Super partition: 9,126,805,504 bytes.
- Display: 1080x2400, 440 dpi, `/sys/class/leds/lcd-backlight/brightness`.
- Keymaster 4.1 (`beanpod`), gatekeeper 1.0, MicroTrust TEE (`teei_daemon`).
- USB: MTK `musb-hdrc` + configfs; custom `init.recovery.usb.rc`.

## Known risks / TODOs

1. **Not compiled yet.** The tree has never been through an AERA build. Expect
   missing-package or flag-name fixes on the first run.
2. **Kernel age.** AERA targets modern kernels. The selene prebuilt is a 4.14
   MTK kernel. Core recovery should boot, but AERA's browser/plugin sandbox
   (which relies on newer kernel features) probably will not.
3. **MTK HAL packages.** The PBRP tree references packages such as
   `android.hardware.boot@1.2-mtkimpl`, `bootctrl.mt6768.recovery`, and
   `libhealthd.mt6768`. Those come from a MediaTek device tree that AERA does
   not sync. This port uses the **prebuilt** keymaster/gatekeeper/TEE binaries
   already in `recovery/root` and relinks their AOSP library dependencies
   (`android.hardware.keymaster@4.0/4.1`, `libkeymaster4*`, boot HIDL 1.0/1.1/
   1.2) into the ramdisk. Boot-control and health HAL services are **not**
   started: AERA's recovery init does not import `/vendor/etc/init`, and the
   MTK `boot-hal`/`health-hal` packages are unavailable. As a result, slot
   switching inside recovery/fastbootd may fail; flashing to the current slot,
   backup/restore, and decryption are unaffected. This is the first thing to
   revisit once the device boots.
4. **Prebuilt HAL ABI.** The MTK HAL binaries were built for Android 11/12.
   They link against platform libraries in the AERA 16 ramdisk. If a service
   crashes, check `logcat`/`dmesg` for missing symbols and add the required
   library to `AERA_RECOVERY_ADDITIONAL_RELINK_LIBRARY_FILES` in
   `BoardConfig.mk`.
5. **No WLAN.** `AERA_ENABLE_WLAN := 0`. Wi-Fi would need the MTK WLAN
   firmware, driver modules, and `wpa_supplicant` in the ramdisk.
6. **Decryption.** Metadata encryption uses `dm-default-key` + keymaster 4.1.
   If `/data` will not decrypt, verify that `/vendor/etc/fstab.mt6768` is
   present in the installed vendor image and that the keymaster service starts.
   The beanpod service is started by `init.recovery.mt6768.rc` once
   `teei_daemon` sets `vendor.sys.listener.registered`.
7. **`PRODUCT_TARGET_VNDK_VERSION := 31`.** Kept from the PBRP tree to match the
   Android 11 vendor. `prebuilts/vndk/v31` is present in the AERA manifest, so
   the build has the matching snapshot. Remove it if a future AERA manifest
   drops v31.
8. **fastbootd is not a separate binary.** AERA 16 builds fastbootd into the
   recovery binary (`librecovery_fastboot`) when `AERA_INCLUDE_FASTBOOTD` is
   set. Do not add `fastbootd` to `PRODUCT_PACKAGES`; that module does not
   exist in AERA 16.

## Next steps for a real bring-up

1. Run the build and fix compile errors.
2. Flash and capture `recovery.log`, `logcat`, and `dmesg`.
3. Verify in this order: display, touch, ADB, partition mount, backup/restore,
   decrypt, MTP, sideload.
4. Only after that, enable WLAN and other optional features.
