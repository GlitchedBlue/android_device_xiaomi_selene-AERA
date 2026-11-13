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

## CI: building on GitHub Actions

`.github/workflows/build-aera.yml` compiles AERA directly on a GitHub-hosted
runner. Crave.io shut down in November 2026, so this is the path the other AERA
device ports use.

Two things make it fit on a stock runner:

1. **`ubuntu-24.04`, not 22.04.** The 24.04 public image has a **145 GB** root
   disk. That is measured, not assumed — a successful AzzyC run shows
   `/dev/root 145G` in its `df -h`. On 22.04 the runner is much smaller, and
   `easimon/maximize-build-space` only pooled it to **84 GB**, i.e. it was
   actively costing space. Do not add it back.
2. **The repository must be public.** A private repo gets a 2 vCPU / 8 GB
   runner and Soong is OOM-killed about 48 minutes into the build. Public gets
   4 vCPU / 16 GB and unlimited Actions minutes. Every AERA port that produces
   images is public for exactly this reason.

`rokibhasansagar/slimhub_actions` deletes the preinstalled SDKs, CLR, Haskell
and CodeQL that this build never touches, and `pierotofy/set-swap-space` adds
30 GB of swap so `-j$(nproc)` on 4 cores survives Soong's peak RSS in 16 GB of
RAM.

### Running

Actions → *Build AERA Recovery (selene)* → **Run workflow**, or push to
`aera-16.0` (the workflow triggers on `device/**` and itself).

The job runs:

```bash
repo init --depth=1 -u https://github.com/AERA-Recovery/android_manifest.git -b aera-16.0
repo sync -c --no-clone-bundle --no-tags --prune -j4
# copy device/xiaomi/selene in, then:
rm -rf .repo            # reclaims ~18 GB before the build
export AERA_BUILD_DEVICE=selene   # must come BEFORE envsetup is sourced
source build/envsetup.sh
lunch twrp_selene-bp2a-eng
mka adbd bootimage      # falls back to recoveryimage
```

Expect **1.5–2.5 hours** per attempt. The sync is the fragile part: it is retried
three times with a 45-minute cap, a heartbeat so the runner never goes silent,
and a free-space guard. `.repo` is deleted before the build because it holds the
history of ~1000 projects and is worthless once the working tree exists.

The sync alone is ~20 minutes and never changes between attempts, so a typo in
the board config costs a full round trip. Check config changes carefully.

Images land in a GitHub **release** tagged `aera-selene-<run id>` and as the
`AERA-selene-<run id>` workflow artifact. The release step has
`fail_on_unmatched_files: true`, so a build that produces nothing fails loudly
instead of publishing an empty release.

### Tuning

| Knob | Where | Why |
| --- | --- | --- |
| `runs-on` | workflow | `ubuntu-24.04` gives a 145 GB disk; 22.04 does not |
| `swap-size-gb` | workflow | 30 GB is what makes `-j$(nproc)` survive Soong on 16 GB |
| `GOMEMLIMIT` | workflow `Build` step | Keeps Soong's Go heap inside the runner's RAM; 12 GiB is safe on 16 GB |
| `target` input | workflow dispatch | `bootimage` (default, selene is recovery-as-boot), `recoveryimage`, `vendorboot` |
