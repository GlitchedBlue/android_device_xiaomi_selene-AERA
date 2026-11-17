# AERA Recovery for Xiaomi Redmi 10 (selene)

An AERA (Android 16) device tree for the **Xiaomi Redmi 10 / Redmi 10 Prime**
(codename `selene` / `selenes`), built as a **boot image** — selene has no
recovery partition, so recovery lives in `boot`.

```
Device      Xiaomi Redmi 10 (selene)
SoC         MediaTek MT6768 / Helio G88
CPU         arm64, 2x Cortex-A75 + 6x Cortex-A55
Display     1080 x 2400, 440 dpi, fbdev (no DRM)
Kernel      4.19.325 (dieD prebuilt; the stock 4.14 cannot run this userspace)
Slots       A/B + virtual A/B
Partitions  dynamic (super: system, vendor, product)
Recovery    recovery-as-boot (no recovery partition; flash boot.img)
Crypto      FBE v2 + metadata encryption (keymaster 4.1 / beanpod)
Vendor      MIUI 14, V14.0.7.0.TKUMIXM, Android 13
```

## Status

This tree **builds, boots and works**. It is a bring-up port, not a release.

| Area | State |
| --- | --- |
| Device tree structure | Done |
| BoardConfig / product makefiles | Done |
| Kernel / DTB / DTBO prebuilts | dieD 4.19.325 (GPU + WiFi capable) — see *Kernel and vendor: what is actually required* |
| MTK recovery HALs (keymaster, gatekeeper, TEE) | From the PBRP selene tree |
| Compile | **Works** — GitHub Actions, run #3 onward |
| Boot / UI / touch | **Works** |
| FBE decryption | **Works** (run #4 + the VINTF fix) |
| Backup / restore / zip install | **Works** |
| GPU acceleration | **Works** — run `43961488385` (`ca912c22`); the Mali backend composites the AERA UI on the device (see below) |
| MTP | **Works** — FunctionFS (`ffs.mtp`), no kernel `f_mtp` needed |
| exFAT MicroSD | **Works** — kernel `exfat` driver; `exfat-fuse` is broken on this kernel |
| WiFi | **Works** — MTK connsys bring-up + AERA's own `wpa_supplicant` (`9040fb34`); scans and returns APs |
| Screenshots | **Works** — captured from the HWC2 client target, not the never-drawn software canvas (`ed66222e`) |
| Dual boot | **Works** — the release `boot.img` boots the system normally and AERA on `adb reboot recovery` |

### Verified on the device

- boots straight to the AERA UI; touch, terminal and file manager work;
- `/data` decrypts with the PIN (`I:Is encrypted, do decrypt page first` →
  decrypt page → PIN accepted → `/data` mounted rw);
- backup produces a real `boot.emmc.win` (64 MB) plus `.sha2` and
  `recovery.log`; zip install returns `{"code":0}` and the zip's own success
  marker;
- the MTK boot-control HAL stays up (`init.svc.boot-hal-1-2` → `running`), so
  slot and wipe operations work;
- the UI is composited on the Mali GPU: `/dev/mali0` open in the recovery
  process, `libGLES_mali.so` mapped, composer in-process, 12-24 fps while
  touching the screen;
- MTP enumerates as `USB\VID_2717&PID_FF48` (WPD device "Redmi 10"); Explorer
  shows *Internal Storage* and *MicroSD*, and a file copied from Windows landed
  in `/data/media/0` with matching contents;
- the MicroSD card mounts with the kernel exFAT driver (`/sdcard1` is
  `type exfat`, not `fuseblk`) and reads a 2.2 MB JPEG at ~12 MB/s;
- USB mass storage works.

### MTP and exFAT

Both problems looked like kernel gaps and neither was one.

**MTP** is FunctionFS, not the kernel `f_mtp` function. Neither the stock 4.14
nor the dieD 4.19 kernel has `CONFIG_USB_F_MTP`, but both have
`CONFIG_USB_CONFIGFS_F_FS`, and AERA's MTP server already prefers
`/dev/usb-ffs/mtp/ep0` over `/dev/mtp_usb`. `init.recovery.usb.rc` therefore
mounts a `functionfs` instance named `mtp`, links `ffs.mtp` into
`configs/b.1/f1`, and advertises the Xiaomi VID/PID `0x2717`/`0xff48`. No kernel
change was needed; the 4.14 idea was a dead end.

**exFAT** was the opposite: the kernel driver works and the userspace one does
not. `exfat-fuse` mounts and lists directories, but every read returns zero
bytes on this kernel (`head -c 64 | wc -c` → 33, `dd` → 0 bytes), so Windows
MTP copies of card files failed with `0x80040005`. `CONFIG_EXFAT_FS` is built
into the dieD kernel, and `mount -t exfat` reads and writes correctly, so the
tree sets `TW_NO_EXFAT_FUSE := true` and `partition.cpp` calls `mount(2)`
directly. Note that `AERA_INCLUDE_FUSE_EXFAT` is a **no-op** — nothing in the
AERA 16 recovery tree reads it; `TW_NO_EXFAT_FUSE` is the real switch.

### WiFi

WiFi is the MediaTek connsys combo, and the kernel already has it: the dieD
4.19 kernel sets `CONFIG_MTK_COMBO_WIFI=y` and `CONFIG_WLAN_DRV_BUILD_IN=y`, the
platform device `18000000.wifi` is in the DTB, and the driver binds as soon as
the connectivity SoC is powered. What was missing was the userspace power
sequence, which nothing in recovery runs:

```
wmt_loader     powers the connectivity SoC, creates /dev/stpwmt
wmt_launcher   downloads the ROM patch, runs calibration, keeps STP alive
/dev/wmtWifi   writing 1 makes the WLAN driver register wlan0
```

`system/bin/aera-wlan-up.sh` runs that once at boot as a background oneshot, so
a tap on *Start WLAN* only has to do `ifconfig wlan0 up` and start
`wpa_supplicant` inside `wlan.cpp`'s 20 s window.

Three defects, each of which failed silently:

1. **Missing connsys firmware.** The ramdisk only had the WIFI and patch-MCU
   blobs. Without `soc1_0_ram_bt_1a_1_hdr.bin` and
   `soc1_0_ram_mcu_1a_1_hdr.bin` (and `BT_FW.cfg` / `wifi.cfg`, which the
   launcher reads next to them) the patch download never completes: the SoC
   stays at `CPUPCR 0x00000000` and every STP receive times out. All four are
   copied from the stock vendor's `/firmware`, unmodified.
2. **Wrong readiness signal.** `vendor.connsys.driver.ready` is already `yes` at
   boot, so powering WLAN on at that point injects a WMT command into the STP
   stream `wmt_launcher` is still using. The core asserts
   (`wmt_core_trigger_assert: Send Assert command !`) and every later
   `WIFI_write` fails with `WMT turn on WIFI fail!` until reboot. The script
   waits for the launcher's own `Calibration end` line in dmesg instead.
3. **`wpa_supplicant` pointed at a file that does not exist.** The service used
   `/vendor/bin/hw/wpa_supplicant`; AERA builds its own into `/system/bin`.
   `wlan.cpp` waits for `/tmp/recovery/sockets/wlan0` and a PING/PONG, which is
   exactly where `ctrl_interface=wlan0` puts the socket once `-O` points there.

Verified on the device: `wlan0` UP, `wpa_cli ping` → `PONG`, scan returns APs,
DHCP lease, `ping 8.8.8.8`, DNS resolution and an HTTP fetch through a phone
hotspot. 5 GHz is supported (25 channels advertised). WPA3/SAE is untested and
not in the driver's advertised `key_mgmt` list, so WPA3-only networks may fail;
WPA2-PSK works. Mobile data does not: there is no RIL/netd/telephony stack in
recovery, so the `ccmni*` interfaces cannot be dialled.

### Kernel and vendor: what is actually required

AERA itself is kernel-agnostic and boots on both kernels selene has. What
differs is three kernel capabilities. Measured from the configs embedded in the
two kernels, not from documentation:

| | 4.14.186 (PBRP prebuilt) | 4.19.325 (dieD) |
| --- | --- | --- |
| AERA boots | yes | yes |
| MTP | `CONFIG_USB_CONFIGFS_F_FS=y` | same |
| exFAT SD | `CONFIG_EXFAT_FS=y` | same |
| Screenshots | yes (software canvas is drawn) | yes |
| **GPU** | **no** — Mali DDK `r26p0` (UK 11.27) vs userspace `r32p1` (UK 11.31); the DDK version check fails | yes (UK 11.46) |
| **WiFi** | **no** — no `CONFIG_WLAN_DRV_BUILD_IN`; the WLAN driver is a module and this ROM ships no `.ko` files at all | yes (built in) |

So a 4.14 user gets MTP, exFAT and screenshots, but software rendering and no
WiFi. Getting both on 4.14 means rebuilding the kernel with
`CONFIG_WLAN_DRV_BUILD_IN=y` **and** a Mali kbase whose UK version is at least
11.31 — the 4.14 source carries r26p0, so that part is a kbase backport, not a
config change. There is no userspace shortcut: the only Mali userspace on the
device is r32p1, and the gralloc1 shim is written against MediaTek's r32p1
gralloc.

The vendor is not the variable. The GPU path uses the **stock vendor's own
files**, byte for byte (`libGLES_mali.so` and `hwcomposer.mt6768.so` have the
same md5 in `/vendor` and in the staged blob store), and the WLAN firmware comes
from that vendor's `/firmware`. Nothing in the vendor is patched or replaced.
The one non-stock file is `gralloc.mt6768.so`, which the vendor does not ship at
all — selene's Android 13 vendor has only `gralloc.default.so` — and it is the
gralloc1 shim, which `dlsym`s MediaTek's exported allocator out of the stock
library. The libs are staged on the `rescue` partition so they are available
before `/vendor` is mounted; if the real `/vendor` is up early it is bound
instead.

### Dual boot: AERA and the system from one image

The release image is built this way by default, so it can be flashed as a
drop-in replacement for the stock boot image: boot normally and the ROM comes up,
ask for recovery and AERA does.

One boot image holds both. `boot_a` carries the system's kernel and a hybrid
ramdisk: AERA's recovery ramdisk at the root, and the system's
`/first_stage_ramdisk` (fstab plus `e2fsck`) alongside it.

The handoff is stock AOSP behaviour, not a patch. When the bootloader is **not**
asking for recovery, init bind-mounts `/first_stage_ramdisk` over `/`, mounts
the real `/system` from there, and execs the system's own `/system/bin/init`
second stage — so the ROM runs with its own init, its own rc files and its own
properties. When the bootloader *is* asking for recovery (`androidboot.mode=recovery`,
set by `adb reboot recovery` or the key combo), no bind mount happens and AERA
runs from the ramdisk root as usual.

Two things decide whether this works, and both cost a bootloop to learn:

1. **The first-stage fstab must be the system's, not AERA's.** AERA's fstab is a
   recovery fstab: `avb=vbmeta_system`, `avb_keys=/avb/q-g`, ext4 only. The
   system's is hand-tuned for selene — erofs first, ext4 second, no AVB — which
   is what lets first-stage fs_mgr mount HyperOS (erofs) and MIUI (ext4) alike.
2. **A boot v2 header stores no DTB offset.** The DTB always follows the second
   stage and the recovery DTBO, each page aligned. Deriving it from
   `recovery_dtbo_offset` is wrong whenever that field is 0 — which it is for the
   system's image, since it has no recovery DTBO. The result is the boot header
   itself (magic `414e4452`, "ANDR") written into the DTB slot, and then nothing
   boots at all: no system, no recovery, just a loop. Assert the DTB magic
   (`d00dfeed`) when repacking.

Sizes are the other constraint. AERA's ramdisk compresses to ~53.4 MB and the
system's to ~38 MB, so the merged image is 63.15 MiB of the 64 MiB partition —
it fits, but a merged AERA + OrangeFox ramdisk does not.

Verified on the device: normal boot reaches `sys.boot_completed=1` on the ROM,
and `adb reboot recovery` comes up with `ro.boot.mode=recovery` and
`init.svc.recovery=running`. AERA keeps its GPU stack, exFAT SD, MTP and WLAN in
the hybrid.

One diagnostic that does **not** work on this kernel: pstore. The ramoops
console region is full (262132 of 262144 bytes) and this kernel's ramoops has no
erase support, so `rm /sys/fs/pstore/*` only removes the entries — the region is
never zeroed and the same old record is re-exported on every boot. A failed boot
leaves no new log there. Reproduce failures by eye, or add your own logging.

### Known limitations

- **Do not re-enable `exfat-fuse`.** It silently returns zero bytes for every
  read on this kernel, which breaks MTP copies and zip installs from the card.
  The build must keep `TW_NO_EXFAT_FUSE := true`.
- **Mobile data does not work in recovery.** The `ccmni*` interfaces exist, but
  there is no RIL, netd or telephony stack to dial them. WiFi is the only
  network path.
- **The UI is rasterised at 1440×3200 and downscaled to 1080×2400** — ~1.8× more
  pixels than the screen has. That is an AERA-side constant, not a driver issue.
- **`aera backup --parts` needs TWRP-style mount points**: `--parts boot` fails
  silently, `--parts /boot` works. The GUI is unaffected.

### GPU acceleration

Working since run `43961488385` (`ca912c22`). On the device the recovery process
holds `/dev/mali0` open with Mali timeline fences, maps `libGLES_mali.so` and the
MediaTek composer, and the kernel's GPU governor cycles under interaction while
the composer reports 12-24 fps against 1 fps idle — i.e. frames are being
submitted to the GPU and nothing redraws when idle.

Three independent defects had to be fixed, and each one failed **silently**:

1. **The build never applied the patches.** The job had no `actions/checkout`, so
   `${GITHUB_WORKSPACE}/patches` never existed; with `globstar nullglob` the loop
   ran zero iterations and stayed green (`patches applied: 0`). The shipped
   binary provably contained none of `patches/bootable/recovery/*.patch`. See
   fix 3 [below](#three-non-obvious-fixes-this-port-needs).
2. **Display 0 was never connected.** Registering `HWC2_CALLBACK_HOTPLUG` alone
   leaves it disconnected: `HWCMediator::deviceRegisterCallback()` runs
   `DisplayManager::init()` from the **vsync** callback, so the hotplug that
   arrives during init is lost. The patch registers vsync, refresh, hotplug and
   then hotplug again, with a bounded 2 s poll as a floor.
3. **The composer was being unloaded.** Recovery `dlclose()`d the composer HAL
   after init, taking the display with it. Patch `0001` keeps it resident.

Scanout buffers are imported as dma-bufs (`EGL_EXT_image_dma_buf_import`, fourcc
`AB24`) instead of copied. `device/xiaomi/selene/gralloc1_compat/` supplies the
gralloc1 module the Mali backend calls `gralloc1_open()` for — selene's Android
13 vendor ships only gralloc4 — by `dlsym`ing MediaTek's exported
`mali_gralloc_buffer_allocate`/`_free`. It is 64-bit only: the 32-bit build
cannot see the 64-bit allocator.

The `AeraGpu` INFO lines are not in `logcat` on a running device because the main
ring buffer is only 256 KiB and the kernel's GPU-DVFS chatter fills it within
minutes. `/proc/<pid>/maps` and the open `malitl_*` fence fds are the reliable
witness. Evidence and the full sequence are in `HANDOFF.md`.

## Build

### GitHub Actions (how images are actually produced)

The workflow in [`.github/workflows/build-aera.yml`](.github/workflows/build-aera.yml)
builds on `ubuntu-24.04` with 30 GB of swap and publishes the image as a
release. Trigger it from *Actions → Build AERA Recovery (selene) → Run
workflow*, target `bootimage`.

The repository **must stay public**: a private repo gets a 2 vCPU / 8 GB runner
and Soong is OOM-killed about 48 minutes in.

#### Build cache and runner recipe

None of this is invented here — it is the recipe from two other AERA device
trees, measured on the same manifest and the same runner class (see
[Credits](#credits)):

| piece | what it buys | why it is set this way |
| --- | --- | --- |
| **ccache** | build step 2h06m → 1h28m | Soong on android16 ignores `USE_CCACHE`; the wrapper comes from `CC_WRAPPER` in the environment, which the Build step exports explicitly. Measured **17520/17522 hits (99.99%)** when the sources did not change, 1.1 GB of a 4 GB cap, 1.05 GB archive. The ccache save runs `if: always()` and the source-cache *save* is disabled so the two never fight over the 10 GB free-tier budget |
| **`.repo` source cache** | ~12 min per attempt | restore 18.8 GB in 3m39s + sync 3m46s ≈ 7m30s, against ~20 min cold. It is an 18.8 GB entry and still restores fine — do not "fix" it by trimming the path |
| **zram + 30 GB swapfile** | keeps Soong alive | Soong analysis peaks at ~30 GB live on a 16 GB runner. zram (lz4, ~2.5×) is swapon'd at priority 100 so it fills first and the swapfile only catches what will not compress. The 30 GB swap is what actually survives; `GOMEMLIMIT` does not |
| `GOGC=40` / `GOMEMLIMIT=11GiB` / `GOMAXPROCS=2` | bounds Soong's Go heap | 14 GiB was tried and made no difference — the working set is genuinely ~30 GB |
| `ubuntu-24.04` + slimhub | 145 GB root disk | 22.04 with `easimon/maximize-build-space` pooled down to 84 GB, i.e. it cost space |

Retry comes from outside the job (`schedule:` cron + a `gate` job), because a
failed job cannot restart itself.

### Locally

Inside an AERA Android 16 checkout (~300 GB free, 16–32 GB RAM):

```bash
./build-aera-selene.sh
```

or manually:

```bash
source build/envsetup.sh
lunch twrp_selene-bp2a-eng
mka adbd bootimage -j$(nproc --all)
```

One thing to do first: the kernel prebuilt ships split (see *Dual boot*), so join
it once before the first build. The script checks the size and md5.

```bash
device/xiaomi/selene/prebuilt/assemble-kernel.sh
```

`twrp_selene` is the product name inherited from the reference tree; the UI is
AERA. See [`docs/BUILDING.md`](docs/BUILDING.md).

## Flash

```bash
fastboot flash boot_a boot.img     # slot _a; use boot_b on the other slot
fastboot reboot
```

The image is dual-boot, so `fastboot reboot` brings up the installed ROM. For
AERA, `adb reboot recovery`, or hold **Volume Up** while powering on.

AVB is **not** enforced on this bootloader (`verifiedbootstate=orange`,
`veritymode=disabled`), so the image does not need re-signing. The stock boot
image is a valid rollback: `fastboot flash boot_a <stock-boot>.img`.

See [`docs/FLASHING.md`](docs/FLASHING.md).

## Three non-obvious fixes this port needs

All are committed and asserted by the workflow, because each one silently
produces a broken image:

1. **ABI shim for the boot-control HAL.** The prebuilt
   `android.hardware.boot@1.0-impl-1.2-mtkimpl.so` imports
   `android::base::Trim(std::string const&)`, which no longer exists in
   Android 16's `libbase`. `bootctl_compat/trim_shim.cpp` provides it as
   `libaera_boot_trim.so`, preloaded **only** into `boot-hal-1-2` from `/sbin`
   (a global `LD_PRELOAD`, or a `/system` path, breaks `getprop`/`sh` as soon as
   a zip mounts `/system`).
2. **VINTF manifest.** The HAL registers 1.0, 1.1 and 1.2, and
   `hwservicemanager` refuses any fqname the vendor manifest does not declare.
   The overlay manifest declares all three; without `@1.2` the HAL is rejected,
   exits, and recovery blocks forever in `IBootControl::getService()`.
3. **The build has to check this repository out.** The GPU patches in
   `patches/bootable/recovery/` are applied to the AERA tree at build time, so
   the job needs a checkout to read them from. It did not have one: the glob was
   empty, `globstar nullglob` made the loop a no-op, and the job reported
   success while shipping a recovery binary with no patches in it — a black
   screen that looked like a driver bug for days. The patch step now hard-fails
   on a missing directory, an empty glob or a count mismatch, and the compiled
   binary is grepped for the patch markers after the build.

## Layout

```
device/xiaomi/selene/
├── Android.bp / Android.mk / AndroidProducts.mk
├── BoardConfig.mk          # MTK, A/B, dynamic partitions, AERA_* flags
├── device.mk               # virtual A/B, VNDK, namespaces, ro.aera.gpu.*
├── twrp_selene.mk          # product definition (twrp_selene)
├── aera_selene.mk          # AERA UI/device options
├── system.prop             # MTK crypto properties
├── vendorsetup.sh          # AERA build environment
├── bootctl_compat/         # the libbase Trim shim
├── gralloc1_compat/        # gralloc1-on-mali_gralloc shim + gralloc1-probe
├── prebuilt/               # Image.gz (shipped split as .00/.01), dtb.img,
│                           # dtbo.img, assemble-kernel.sh
└── recovery/root/          # fstab, twrp.flags, MTK init/HALs, vintf,
                            # scoped boot-HAL .rc, GPU staging scripts
```

## Credits

- AERA Recovery Project — device-tree format and recovery source
- [apkreader](https://github.com/apkreader) — stock-driver Mali GPU backend
  (`mali-testing`)
- [AzzyC](https://github.com/AzzyC) — [Action-OFRP-Builder](https://github.com/AzzyC/Action-OFRP-Builder).
  The runner recipe this workflow is based on, and the ccache setup that takes
  the build step from 2h06m to 1h28m: `ubuntu-24.04` with its 145 GB root disk,
  slimhub, 30 GB swap, and ccache wired in through `CC_WRAPPER`.
- [ILYESwsa](https://github.com/ILYESwsa) — [pbrp_device_oppo_f11](https://github.com/ILYESwsa/pbrp_device_oppo_f11).
  The `.repo` source-cache measurements (18.8 GB restored in 3m39s, sync 3m46s,
  ~12 min saved per attempt) and the `GOGC` / `GOMEMLIMIT` / `GOMAXPROCS` values
  used here — the same manifest on the same runner class.
- PitchBlackRecoveryProject — selene hardware tree (kernel, HALs, fstab, init)
- [dieD](https://github.com/dieD) — the 4.19.325 kernel prebuilt. It is the only
  selene kernel with `CONFIG_WLAN_DRV_BUILD_IN` and a Mali DDK new enough for
  the stock r32p1 userspace, so it is what makes both WiFi and GPU acceleration
  possible, and it boots the stock system unchanged.
- TeamWin / OrangeFox / LineageOS — upstream recovery work

Current status, evidence and the full list of things learned the hard way are
in [`HANDOFF.md`](HANDOFF.md).
