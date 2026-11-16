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
| GPU acceleration | **In progress** — every GPU path is proven on the device; the fix for the black screen is a CI defect that is now corrected (see below) |

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
- **GPU acceleration is in progress.** selene's Android 13 vendor ships gralloc4
  (HIDL `allocator@4.0` + `mapper@4.0`) and **no gralloc1 module**, while the Mali
  backend calls `gralloc1_open()`. `device/xiaomi/selene/gralloc1_compat/` builds
  `libaera_gralloc1.so`, a gralloc1 shim over MediaTek's exported
  `mali_gralloc_buffer_allocate`/`_free`, published as `gralloc.mt6768.so` and
  staged from the blob store like the other vendor libraries. Until it
  initialises, the UI falls back to software rendering.

  The black screen was **not** the shim. The CI build job had no
  `actions/checkout`, so `${GITHUB_WORKSPACE}/patches` never existed, and with
  `globstar nullglob` the patch loop ran zero iterations and stayed green
  (`patches applied: 0`). The shipped `recovery` binary provably contains none
  of `patches/bootable/recovery/*.patch`. Every path is now proven on the device
  with `gralloc1-probe`: hotplug alone leaves display 0 disconnected, while
  vsync + refresh + hotplug + a second hotplug connects it; `AB24` dma-buf
  import renders and presents with real release fences. The build now greps the
  compiled binary for the patch markers and fails instead of publishing another
  unpatched image. Evidence and the full sequence are in `HANDOFF.md`.
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

#### Build cache and runner recipe

None of this is invented here — it is the recipe from two other AERA device
trees, measured on the same manifest and the same runner class (see
[Credits](#credits)):

| piece | what it buys | why it is set this way |
| --- | --- | --- |
| **ccache**, 10 GB budget | build step 2h06m → 1h28m | Soong on android16 ignores `USE_CCACHE`; the compiler wrapper comes from `CC_WRAPPER` in the environment, which is what the Build step exports. Measured **17520/17522 hits (99.99%)** when the sources did not change. The cache owns the whole 10 GB free-tier budget, so the source-cache *save* is disabled and the ccache save runs `if: always()` |
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
├── prebuilt/               # Image.gz, dtb.img, dtbo.img
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
- TeamWin / OrangeFox / LineageOS — upstream recovery work

Current status, evidence and the full list of things learned the hard way are
in [`HANDOFF.md`](HANDOFF.md).
