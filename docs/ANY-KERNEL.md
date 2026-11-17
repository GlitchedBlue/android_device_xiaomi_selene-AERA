# Running AERA on any kernel

AERA's recovery is a userspace. It talks to the kernel through standard
interfaces -- block devices, FUSE, FunctionFS, netlink, DRM -- so the same
ramdisk boots on 4.14 and 4.19 kernels, on a stock Xiaomi kernel or a custom
one. This is the "any kernel" path: keep the kernel you want, replace the
ramdisk.

`tools/repack-aera.py` does that.

## What the kernel actually has to provide

Four things matter. Everything else (MTP, adb, flashing, exFAT, `/data`,
screenshots) already works on a stock 4.14 kernel -- verified on
`4.14.186-perf-gd9116d4b45d4`.

| Requirement | Why | How to check |
| --- | --- | --- |
| Mali DDK at least as new as the vendor userspace | `libGLES_mali.so` calls into `/dev/mali0`; a kbase older than the userspace's is rejected with `Kernel DDK version %s` + `EINVAL`, and there is no software fallback | `dmesg \| grep 'Kernel DDK version'` |
| A WLAN driver that registers `wlan0` | `wlan0` must exist before `wpa_supplicant` can do anything | `ls /sys/class/net` |
| FunctionFS (`CONFIG_USB_CONFIGFS_F_FS`) | MTP and fastbootd are FunctionFS gadgets, not the kernel `f_mtp`/`f_fastboot` functions | `mount \| grep functionfs` |
| exFAT in the kernel (`CONFIG_EXFAT_FS`) or FUSE | AERA sets `TW_NO_EXFAT_FUSE := true`, so an exFAT card needs the kernel driver | `grep exfat /proc/filesystems` |

### The GPU version rule

The vendor userspace on selene is Mali **r32p1, UK version 11.31**. The kernel
DDK must be **>= 11.31**:

| Kernel DDK | UK version | Result |
| --- | --- | --- |
| `r26p0` (`mali_bifrost/mali-r25p0`) | 11.27 | `EINVAL`, no GPU |
| `r32p1` (`mali_valhall/mali-r32p1`) | 11.38 | works |

`/dev/mali0` will exist and the kernel logs a successful probe even when the
DDK is too old -- the failure only shows up when the userspace opens the
device. So check the version string, not the device node.

### WiFi: in-tree or matching modules

`wlan0` comes from MediaTek's `gen4m` driver. It is either linked into the
kernel (`CONFIG_MTK_COMBO_WIFI=y` plus `CONFIG_WLAN_DRV_BUILD_IN=y`) or built
as modules (`wmt_drv.ko`, `wmt_chrdev_wifi.ko`, `wlan_drv_gen4m.ko`). Modules
only load if their vermagic matches the running kernel, so a `.ko` from another
build is useless.

`aera-wlan-up.sh` handles both: it runs the connsys power sequence, waits for
`Calibration end`, then writes `1` to `/dev/wmtWifi`. If `wlan0` still does not
appear it tries `insmod` on those three modules from
`/gpu-rescue/aera-modules`, `/vendor/lib/modules` and `/system/lib/modules`,
logging and continuing when a module is rejected.

## Repacking

```sh
python tools/repack-aera.py \
    --donor stock-boot.img \
    --aera  AERA-selene-dualboot-dieD419.img \
    --out   AERA-selene-my-kernel.img \
    --pad   67108864
```

`--donor` supplies the kernel, the device tree, the dtbo and the load
addresses. `--aera` supplies only the ramdisk. The donor's cmdline is kept
unless `--cmdline` overrides it.

If the kernel is not inside a boot image yet -- an AnyKernel3 zip ships a raw
`Image`, a kernel CI artifact ships `Image.gz` -- pass it separately. A raw
`Image` is gzipped for you:

```sh
python tools/repack-aera.py --donor stock-boot.img --kernel Image \
    --aera AERA-selene-dualboot-dieD419.img --out out.img --pad 67108864
```

`--pad 67108864` matches selene's 64 MiB boot partition. The image does not
have to be padded to flash, but the release images are, and a padded image is
easier to spot-check with `md5sum`.

## Flashing

selene cannot `fastboot boot` -- the bootloader only flashes. Keep a known-good
image and flash it back if the new one does not boot:

```sh
fastboot flash boot_a AERA-selene-my-kernel.img
fastboot reboot recovery
adb shell uname -r
```

`boot_a` is `mmcblk0p27`. `AERA-selene-dualboot-dieD419.img` is the rollback
image for the current 4.19 ROM; it boots normal boot into the ROM and recovery
into AERA.

## Verified combinations

| Kernel | WiFi | GPU | Notes |
| --- | --- | --- | --- |
| dieD `4.19.325-cip136-st20-dieD` (shipped) | yes | yes (`r32p1`, UK 11.38) | default release kernel |
| stock `4.14.186-perf-gd9116d4b45d4` | yes | no (`r26p0`, UK 11.27) | UI, touch, MTP, fastbootd, exFAT, `/data` all fine |
| `4.14.357+17-LittleSpammyMailman-2026.8.30-r` | yes | no (`r26p0`, UK 11.27) | in-tree `gen4m`; `wpa_cli ping` -> `PONG`, scan returns APs |

A 4.14 kernel can run AERA fine, but a **4.19 vendor ROM** will not boot on a
4.14 kernel: init comes up and `system_server` never starts. That is a vendor
problem, not an AERA one -- use the 4.14 image only with a 4.14 vendor.

See `KERNEL-4.14.md` for the source-level fix that gives a 4.14 kernel the
`r32p1` DDK.
