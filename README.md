# AERA Recovery for Xiaomi Redmi 10 (selene)

An AERA (Android 16) device tree for the **Xiaomi Redmi 10 / Redmi 10 Prime**
(codename `selene` / `selenes`), built as a **boot image** — selene has no
recovery partition, so recovery lives in `boot`.

```
Device      Xiaomi Redmi 10 (selene)
SoC         MediaTek MT6768 / Helio G88
CPU         arm64, 2x Cortex-A75 + 6x Cortex-A55
Display     1080 x 2400, 440 dpi, fbdev (no DRM)
Kernel      4.14 (prebuilt, from the stock selene boot image)
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
| Kernel / DTB / DTBO prebuilts | From the PBRP selene tree |
| MTK recovery HALs (keymaster, gatekeeper, TEE) | From the PBRP selene tree |
| Compile | **Works** — GitHub Actions, run #3 onward |
| Boot / UI / touch | **Works** |
| FBE decryption | **Works** (run #4 + the VINTF fix) |
| Backup / restore / zip install | **Works** |
| GPU acceleration | **Blocked** — the vendor has no gralloc1 module |

### Verified on the device

- boots straight to the AERA UI; touch, terminal and file manager work;
- `/data` decrypts with the PIN (`I:Is encrypted, do decrypt page first` →
  decrypt page → PIN accepted → `/data` mounted rw);
- backup produces a real `boot.emmc.win` (64 MB) plus `.sha2` and
  `recovery.log`; zip install returns `{"code":0}` and the zip's own success
  marker;
- the MTK boot-control HAL stays up (`init.svc.boot-hal-1-2` → `running`), so
  slot and wipe operations work;
- USB mass storage works.

### Known limitations

- **~4 fps.** The UI runs on the software renderer because the Mali backend
  cannot initialise — see below. It rasterises 1440×3200 and downscales to
  1080×2400, which is ~1.8× more pixels than the screen has.
- **No MTP.** The kernel is built without `CONFIG_USB_CONFIGFS_F_MTP`. Use
  *Mount → USB Storage* instead.
- **No GPU.** selene's Android 13 vendor ships gralloc4 (HIDL
  `allocator@4.0` + `mapper@4.0`) and **no gralloc1 module**, while the Mali
  backend needs `gralloc1_open()`. The backend is compiled in and stays
  dormant; the UI falls back to software. This is true of the stock firmware
  too, not just of a modified vendor.
- **`aera backup --parts` needs TWRP-style mount points**: `--parts boot` fails
  silently, `--parts /boot` works. The GUI is unaffected.

## Build

### GitHub Actions (how images are actually produced)

The workflow in [`.github/workflows/build-aera.yml`](.github/workflows/build-aera.yml)
builds on `ubuntu-24.04` with 30 GB of swap and publishes the image as a
release. Trigger it from *Actions → Build AERA Recovery (selene) → Run
workflow*, target `bootimage`.

The repository **must stay public**: a private repo gets a 2 vCPU / 8 GB runner
and Soong is OOM-killed about 48 minutes in.

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

`twrp_selene` is the product name inherited from the reference tree; the UI is
AERA. See [`docs/BUILDING.md`](docs/BUILDING.md).

## Flash

```bash
fastboot flash boot_a boot.img     # slot _a; use boot_b on the other slot
fastboot reboot
```

AVB is **not** enforced on this bootloader (`verifiedbootstate=orange`,
`veritymode=disabled`), so the image does not need re-signing. The stock boot
image is a valid rollback: `fastboot flash boot_a <stock-boot>.img`.

See [`docs/FLASHING.md`](docs/FLASHING.md).

## Two non-obvious fixes this port needs

Both are committed and both are asserted by the workflow, because either one
silently bricks the boot HAL:

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
├── prebuilt/               # Image.gz, dtb.img, dtbo.img
└── recovery/root/          # fstab, twrp.flags, MTK init/HALs, vintf,
                            # scoped boot-HAL .rc, GPU staging scripts
```

## Credits

- AERA Recovery Project — device-tree format and recovery source
- [apkreader](https://github.com/apkreader) — stock-driver Mali GPU backend
  (`mali-testing`)
- PitchBlackRecoveryProject — selene hardware tree (kernel, HALs, fstab, init)
- TeamWin / OrangeFox / LineageOS — upstream recovery work

Current status, evidence and the full list of things learned the hard way are
in [`HANDOFF.md`](HANDOFF.md).
