# Flashing AERA on the Redmi 10 (selene)

> **Read this first.** selene stores recovery in the **boot** partition. The
> AERA image replaces the boot image, so the phone will not boot the installed
> ROM until you restore a boot image that matches that ROM. Keep backups.

## Before you start

1. **Unlock the bootloader** (Mi Unlock). An unlocked bootloader is required.
2. **Back up the stock boot image.** The firmware package used for this port is
   `selene_global_images_V12.5.20.0.RKUMIXM`; its `images/boot.img` is a known
   good restore point.
3. Install platform-tools (`adb`, `fastboot`) and confirm the device is visible:
   ```bash
   adb devices
   adb reboot bootloader
   fastboot devices
   ```
4. Know the current slot:
   ```bash
   fastboot getvar current-slot
   ```

## Flash

The AERA output is a boot image:

```
out/target/product/selene/AERA-R1.0-Alpha-Unofficial-selene.img
```

Flash it to the active slot:

```bash
fastboot flash boot AERA-R1.0-Alpha-Unofficial-selene.img
```

or to both slots explicitly:

```bash
fastboot flash boot_a AERA-R1.0-Alpha-Unofficial-selene.img
fastboot flash boot_b AERA-R1.0-Alpha-Unofficial-selene.img
```

If the device refuses to boot because of verified boot, disable verification
with the stock vbmeta image from the same firmware package:

```bash
fastboot --disable-verity --disable-verification flash vbmeta images/vbmeta.img
```

## Boot into recovery

Because recovery is in `boot`:

- From Android: `adb reboot recovery`
- From fastboot: `fastboot reboot` then hold **Volume Up** while booting
- Power off, then hold **Volume Up + Power**

## Restore the stock boot image

```bash
fastboot flash boot images/boot.img
```

or flash the boot image from the ROM you are running. This is required to boot
the system again after using recovery-in-boot.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Black screen, no ADB | Kernel/DTB mismatch, or ramdisk too large for the 64 MiB boot partition |
| Boots to system instead of recovery | Wrong slot flashed, or the boot image was not replaced |
| Stuck at splash | Missing HAL libs; capture `recovery.log` and `logcat` |
| `/data` will not mount | Decryption/keymaster issue; see `docs/PORTING-NOTES.md` |
| Fastboot rejects the image | AVB/verification still enabled; flash the stock `vbmeta.img` with verification disabled |
