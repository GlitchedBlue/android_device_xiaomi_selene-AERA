# Flashing AERA on the Redmi 10 (selene)

> **Read this first.** selene stores recovery in the **boot** partition. The
> release image is a **dual-boot image**: it carries the system's kernel and a
> hybrid ramdisk, so the phone still boots the installed ROM normally and only
> starts AERA when the bootloader asks for recovery. You do not have to restore
> a boot image to get back into Android.
>
> Keep a backup of the stock boot image anyway. It is the one thing that always
> gets you out of a bad flash.

## Before you start

1. **Unlock the bootloader** (Mi Unlock). An unlocked bootloader is required.
2. **Back up the stock boot image.** The firmware package used for this port is
   `selene_global_images_V12.5.20.0.RKUMIXM`; its `images/boot.img` is a known
   good restore point. If your ROM is not that package, use the boot image from
   the ROM you are actually running.
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

or, from the CI release, `boot.img`.

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

## Boot into Android

Nothing special. Power on, or let the flash reboot it, and the installed ROM
comes up. The image is a drop-in replacement for the stock boot image.

## Boot into recovery

Because recovery is in `boot`, the bootloader has to be told to ask for it:

- From Android: `adb reboot recovery`
- From fastboot: `fastboot reboot` then hold **Volume Up** while booting
- Power off, then hold **Volume Up + Power**

The device reports `ro.boot.mode=recovery` when this works. Booting normally
afterwards is just a reboot — AERA does not replace or uninstall anything.

## How the two live in one image

`boot_a` holds the system's kernel plus a hybrid ramdisk: AERA's recovery
ramdisk at the root, and the system's `/first_stage_ramdisk` (the fstab and
`e2fsck`) alongside it.

- **Normal boot.** init bind-mounts the system's `/first_stage_ramdisk` over `/`
  before first stage, so the system's own init and fstab run and the ROM comes
  up. AERA's files are still there, just not the ones first stage reads.
- **Recovery boot.** The mode check in init sends it to AERA, which uses the
  recovery ramdisk at the root as usual.

The first-stage fstab is the system's, not AERA's. AERA's recovery fstab uses
`avb=vbmeta_system` with `avb_keys=/avb/q-g` and lists only ext4; the system's
lists erofs then ext4 with no AVB, which is what first-stage `fs_mgr` needs to
mount both HyperOS (erofs) and MIUI (ext4) systems.

## Restore the stock boot image

```bash
fastboot flash boot images/boot.img
```

or flash the boot image from the ROM you are running. Do this if the phone will
not boot at all, or if you are going back to a plain stock boot image.

## Troubleshooting

| Symptom | Likely cause |
| --- | --- |
| Black screen, no ADB | Kernel/DTB mismatch, or ramdisk too large for the 64 MiB boot partition |
| Boots Android when you wanted recovery | The bootloader did not ask for recovery; use `adb reboot recovery` rather than a plain reboot |
| Boots AERA on a normal boot | The hybrid ramdisk lost its `first_stage_ramdisk`, so first stage never handed over to the system |
| Stuck at splash | Missing HAL libs; capture `recovery.log` and `logcat` |
| `/data` will not mount | Decryption/keymaster issue; see `docs/PORTING-NOTES.md` |
| Fastboot rejects the image | AVB/verification still enabled; flash the stock `vbmeta.img` with verification disabled |

### The image is close to the partition limit

The dual-boot image is about 63.15 MiB of the 64 MiB `boot` partition. That
leaves under 1 MiB of ramdisk headroom, so adding large files to
`recovery/root` can push it over and produce a black screen. Check the size
before flashing a locally built image.
