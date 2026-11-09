# Building AERA Recovery for selene

## Host requirements

AERA is a full Android 16 source build. It **cannot** be compiled on Windows
natively.

| Item | Minimum | Recommended |
| --- | --- | --- |
| OS | Ubuntu 22.04 | Ubuntu 24.04 |
| CPU | 8 cores | 16+ cores |
| RAM | 16 GB | 32 GB |
| Disk | 300 GB free (SSD) | 400 GB |
| Swap | 16 GB | 32 GB |

Windows users: use **WSL2 with Ubuntu 22.04/24.04** and keep the source on the
Linux filesystem (`~/android/...`), not under `/mnt/c`, or the build will be
extremely slow.

### Install the build packages (Ubuntu)

```bash
sudo apt update
sudo apt install -y git curl python3 repo openjdk-17-jdk \
  bc bison build-essential ccache flex g++-multilib gcc-multilib \
  gnupg gperf imagemagick lib32ncurses5-dev lib32readline-dev \
  lib32z1-dev libelf-dev liblz4-tool libncurses5 libncurses5-dev \
  libsdl1.2-dev libssl-dev libxml2 libxml2-utils lzop pngcrush \
  rsync schedtool squashfs-tools xsltproc zip zlib1g-dev
```

## 1. Sync the AERA manifest

```bash
mkdir -p ~/android/AERA_16.0
cd ~/android/AERA_16.0
repo init -u https://github.com/AERA-Recovery/android_manifest.git -b aera-16.0
repo sync -c --force-sync --no-clone-bundle --no-tags -j$(nproc)
```

The first sync is large (tens of GB).

## 2. Install this device tree

Copy `device/xiaomi/selene` into the checkout:

```bash
mkdir -p device/xiaomi
rsync -a --delete /path/to/aera-selene/device/xiaomi/selene/ device/xiaomi/selene/
```

If the tree lives in its own git repo, use `local_manifests/selene.xml`
instead (replace `YOUR_GITHUB_USER`).

## 3. Build

```bash
source build/envsetup.sh
lunch twrp_selene-bp2a-eng
mka adbd bootimage -j$(nproc --all)
```

`bp2a` is AERA's default Android 16 release name; the three-part lunch form is
`<product>-<release>-<variant>`.

### Output

```
out/target/product/selene/AERA-R1.0-Alpha-Unofficial-selene.img
out/target/product/selene/boot.img
```

Because selene is **recovery-as-boot**, the recovery image is the boot image.

## 4. Rebuild

```bash
source build/envsetup.sh
lunch twrp_selene-bp2a-eng
mka adbd bootimage -j$(nproc --all)
```

Use `mka clean` (or delete `out/target/product/selene`) after changing
`BoardConfig.mk` or the prebuilts.

## Troubleshooting

| Symptom | Fix |
| --- | --- |
| `Unable to find product twrp_selene` | Device tree is not at `device/xiaomi/selene`, or `AndroidProducts.mk` was not picked up. Re-run `source build/envsetup.sh`. |
| Missing `Image.gz` / `dtb.img` | The `prebuilt/` folder is empty. Re-copy the tree or pull the files from the PBRP selene repo. |
| `BOARD_KERNEL_SEPARATED_DTBO` errors | Ensure `prebuilt/dtbo.img` exists and `BOARD_PREBUILT_DTBOIMAGE` points at it. |
| Out of memory / `ninja` killed | Lower `-j` (e.g. `-j4`) and add swap. |
| `repo: command not found` | Install the `repo` launcher (see the AOSP docs) or `apt install repo`. |

## CI

`.github/workflows/build.yml` is provided for a **self-hosted** Linux runner.
GitHub-hosted runners do not have enough disk or RAM for a full AERA build.
