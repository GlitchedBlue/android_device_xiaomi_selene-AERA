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

## CI: building on Crave from GitHub Actions

`.github/workflows/crave-selene.yml` compiles **on Crave's build nodes**, not on
the GitHub runner. The runner only installs the Crave CLI and streams a job, so
a stock `ubuntu-latest` machine (4 vCPU / 16 GB / 14 GB disk) is enough — the
~300 GB tree never touches it.

### One-time setup

1. Add two repository secrets (Settings → Secrets and variables → Actions):

   | Secret | Value |
   | --- | --- |
   | `CRAVE_USERNAME` | your `foss.crave.io` username |
   | `CRAVE_TOKEN` | the API token from the Crave dashboard |

2. Pick a **base project**. AERA is Android 16, so use an Android 16 project:

   | Project ID | Base project |
   | --- | --- |
   | `99` | LineageOS 23.2 (default) |
   | `82` | PixelOS 16 |

   The `List Crave projects` step in the run prints every project ID you can
   use. Override the default either with the `project_id` workflow input or a
   repository **variable** named `CRAVE_PROJECT_ID`.

   If you change the project, also set the matching manifest, because Crave
   matches the local `.repo` against the project:

   | Variable | Default |
   | --- | --- |
   | `CRAVE_BASE_MANIFEST` | `https://github.com/accupara/los23.2.git` |
   | `CRAVE_BASE_BRANCH` | `lineage-23.2` |

### Running

Actions → *Build AERA Recovery (selene)* → **Run workflow**, or push to
`aera-16.0`.

The workflow prepares a workspace directory with a shallow `repo init` of the
base project's manifest — `crave run` works out which project a directory
belongs to from that `.repo`, and refuses to start without one
(`could not get project information for <path>`). Then it runs
`scripts/crave-build.sh` on the build node, which follows Crave's documented
*unsupported ROM* recipe:

```bash
repo init -u https://github.com/AERA-Recovery/android_manifest.git -b aera-16.0
/opt/crave/resync.sh
# copy device/xiaomi/selene in
source build/envsetup.sh
lunch twrp_selene-bp2a-eng
mka adbd bootimage
```

The finished images are left in `crave-artifacts/` inside the Crave workspace.
The runner pulls them back with `crave pull` (same path and hostname the job
used) and a separate `publish` job uploads them to a GitHub **release** tagged
`aera-selene-<run id>`.

It never runs `rm -rf *` — Crave bans accounts that do.

The same images are also attached to the run as the `AERA-selene-<run id>`
workflow artifact.

### Token exposure on Crave

Crave shows the full `crave run` command line in its dashboard, so any secret on
that line is readable by everyone on the Crave team. The build job therefore
runs with `permissions: contents: read`: the token the node receives can only
read this private repo and expires with the job. The write-scoped token exists
only in the `publish` job, which stays on GitHub.

### Why not just `ubuntu-latest`?

GitHub-hosted runners have ~14 GB of disk. AERA's manifest is a full Android 16
tree (tens of GB of source plus `out/`), so a local sync dies with
`No space left on device` long before compiling. Crave provides the disk, RAM
and Android build image; GitHub provides the trigger and the artifact storage.
