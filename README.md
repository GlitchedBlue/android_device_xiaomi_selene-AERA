# AERA Recovery for Xiaomi Redmi 10 (selene)

An AERA Android 16 device tree for the **Xiaomi Redmi 10 / Redmi 10 Prime**
(codename `selene` / `selenes`).

This is a **bring-up port**, not a finished release. It adapts the official
AERA device-tree format (from the Mi MIX 2 `chiron` tree) to selene hardware,
using the working PBRP selene tree as the hardware reference.

```
Device      Xiaomi Redmi 10 (selene)
SoC         MediaTek MT6768 / Helio G88
CPU         arm64, 2x Cortex-A75 + 6x Cortex-A55
Display     1080 x 2400, 440 dpi, fbdev
Kernel      4.14 (prebuilt, from the working selene boot image)
Slots       A/B + virtual A/B
Partitions  dynamic (super: system, vendor, product)
Recovery    recovery-as-boot (no recovery partition; flash boot.img)
Crypto      FBE v2 + metadata encryption (keymaster 4.1 / beanpod)
```

## Status

| Area | State |
| --- | --- |
| Device tree structure | Done |
| BoardConfig / product makefiles | Done |
| Kernel / DTB / DTBO prebuilts | Pulled from the PBRP selene tree |
| MTK recovery HALs (keymaster, gatekeeper, TEE) | Pulled from the PBRP selene tree |
| Compile | **Not yet verified** — needs a Linux/AERA checkout |
| Boot / touch / decrypt | **Untested** |

AERA is built for modern devices with newer kernels. selene is a legacy
MediaTek device with a 4.14 vendor kernel, so some AERA features (the browser
sandbox, newer fscrypt policies) may not work. See
[`docs/PORTING-NOTES.md`](docs/PORTING-NOTES.md).

## Layout

```
device/xiaomi/selene/
├── Android.bp / Android.mk / AndroidProducts.mk
├── BoardConfig.mk          # MTK, A/B, dynamic partitions, AERA_* flags
├── device.mk               # virtual A/B, VNDK, namespaces
├── twrp_selene.mk          # product definition (twrp_selene)
├── aera_selene.mk          # AERA UI/device options
├── system.prop             # MTK crypto properties
├── vendorsetup.sh          # AERA build environment
├── prebuilt/               # Image.gz, dtb.img, dtbo.img
└── recovery/root/          # fstab, twrp.flags, MTK init/HALs, vintf
```

## Build

On Linux (Ubuntu 22.04/24.04, ~300 GB free, 16-32 GB RAM):

```bash
./build-aera-selene.sh
```

Or manually, inside an AERA Android 16 checkout:

```bash
# device/xiaomi/selene must exist in the checkout
source build/envsetup.sh
lunch twrp_selene-bp2a-eng
mka adbd bootimage -j$(nproc --all)
```

The image is written to `out/target/product/selene/AERA-R1.0-Alpha-Unofficial-selene.img`.

See [`docs/BUILDING.md`](docs/BUILDING.md) for prerequisites and
[`docs/FLASHING.md`](docs/FLASHING.md) for flashing.

## Credits

- AERA Recovery Project — device-tree format and recovery source
- PitchBlackRecoveryProject — selene hardware tree (kernel, HALs, fstab, init)
- TeamWin / OrangeFox / LineageOS — upstream recovery work
