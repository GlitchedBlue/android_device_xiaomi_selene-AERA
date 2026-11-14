# Handoff / current status

Last updated: 2026-11-14 (run #4 flashed; both root causes proven on-device; VINTF fix unpushed)

## Read this first

**The recovery UI works.** Run #4's image boots, and once the boot-HAL service
was made to stay up, recovery reached the decryption PIN screen. Getting there
needed **two** independent fixes, both now proven on the device:

1. **ABI shim** — the A11 MTK boot-HAL prebuilt could not `dlopen` against A16
   `libbase`. Fixed by `libaera_boot_trim` + `LD_PRELOAD` (**shipped in run #4**).
2. **VINTF declaration** — with the shim, the HAL loaded and registered 1.0 but
   was refused on 1.2, so it exited. Declaring 1.0/1.1/1.2 in the vendor
   manifest makes it stay up (**patched live, still unpushed**).

- Run #4: id `43566176237`, commit `265c126`, branch `aera-16.0` — **success**
- Flashed. `boot-hal-1-2` is now `running` (was crash-looping every 5 s), and
  `recovery.log` (24 KB, up from 5.6 KB) ends at
  `I:Is encrypted, do decrypt page first` — the PIN screen.
- So build, boot header, MTK offsets, kernel, init, adbd, keymaster, the ramdisk
  and the boot HAL are all **correct**.

**A fresh flash of run #4 regresses**: that image still carries the old manifest,
so the HAL dies again. The fix lives in
`recovery/root/vendor/etc/vintf/manifest.xml` locally and must be pushed.

## The actual bug (root-caused, not a guess)

`boot-hal-1-2` crash-loops roughly every 5 s, forever:

```
E HidlServiceManagement: Failed to dlopen android.hardware.boot@1.0-impl-1.2-mtkimpl.so:
    dlopen failed: cannot locate symbol
    "_ZN7android4base4TrimERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE"
init: Service 'boot-hal-1-2' (pid N) exited with status 1
I hwservicemanager: Since android.hardware.boot@1.0::IBootControl/default is not
    registered, trying to start it as a lazy HAL
I HidlServiceManagement: getService: Trying again for
    android.hardware.boot@1.0::IBootControl/default...
```

That mangled name is `android::base::Trim(std::string const&)`. **Android 13
replaced it with a `std::string_view` overload**, so the Android 11 MediaTek
prebuilt cannot be dlopen()ed against Android 16's `libbase`.

Recovery calls `IBootControl::getService()` during startup, `hwservicemanager`
retries the lazy HAL forever, and the retry loop is **infinite** — so the UI
thread never draws. `recovery` sits in `futex_wait_queue_me` with one
`HwBinder:367_1` thread parked in `binder_ioctl_write_read`. It is a wait, not
a crash.

Scope was measured, not assumed. Pulling `bootimpl.so` plus all 13 of its
dependency libraries off the phone and resolving every undefined symbol:

```
undefined symbols in impl : 160
symbols available in deps  : 6102
== UNRESOLVED (would fail dlopen) ==
    _ZN7android4base4TrimERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE
unresolved count: 1
```

**One symbol out of 160.** The failing `.so` on the device is byte-identical
(SHA256 `63191FE1...CC168`) to
`device/xiaomi/selene/recovery/root/system/lib64/hw/android.hardware.boot@1.0-impl-1.2-mtkimpl.so`.

### What it is NOT

- **Not decryption / FBE / keymaster.** `I:Can't probe device /dev/block/mmcblk0p53`
  is *expected*: `/data` is metadata-encrypted (`dm-default-key`), so blkid
  cannot read a raw f2fs signature. `dm-7` is already named `userdata`.
  Decryption never got a chance to run — the process blocks before it.
- **Not `AERA_USE_FSCRYPT_POLICY`**, not the theme, not the UI, not the kernel.
- **Not `AB_OTA_UPDATER` / `ENABLE_VIRTUAL_AB`.** selene is genuinely A/B:
  `boot_a`/`boot_b`, `vbmeta_system_a/b`, `ro.boot.slot_suffix=_a`. The
  original PBRP tree sets both flags identically. **Do not change them.**

### The fix (implemented locally, unpushed)

A ~10 KB shim re-exporting the one missing symbol, `LD_PRELOAD`ed into the HAL:

| File | Change |
| --- | --- |
| `device/xiaomi/selene/bootctl_compat/trim_shim.cpp` | **new** — `android::base::Trim(const std::string&)` |
| `device/xiaomi/selene/bootctl_compat/Android.bp` | **new** — `cc_library_shared` `libaera_boot_trim`, `stl: "libc++"`, `-fvisibility=default` |
| `device/xiaomi/selene/device.mk` | `PRODUCT_PACKAGES += libaera_boot_trim` |
| `device/xiaomi/selene/recovery/root/init.recovery.mt6768.rc` | `export LD_PRELOAD /system/lib64/libaera_boot_trim.so` in `on init` |
| `.github/workflows/build-aera.yml` | builds the shim first, `readelf`-asserts the exact mangled symbol, copies it onto the recovery overlay, and fails the run if the staged recovery root lacks it |

Two mechanism choices worth keeping:

- **`export`, not a service-scoped `setenv`.** The boot HAL's service
  definition is a build-installed `.rc` we do not own, so overriding it would
  race the installer. `export` is already used one line above for
  `LD_LIBRARY_PATH`, and it provably reaches services: keymaster has a hard
  `DT_NEEDED` on `libTEECommon.so` (which lives in `/vendor/lib64`), has **no
  RUNPATH**, and loads fine — only the global `export LD_LIBRARY_PATH` can be
  putting `/vendor/lib64` on its search path.
- **Overlay copy, not `TARGET_RECOVERY_DEVICE_MODULES`.** That variable does
  not exist in Android 16's build system. `recovery/root/` is the mechanism
  this tree already uses to get the MTK boot HAL into `/system/lib64/hw/`.

A missing `LD_PRELOAD` entry is non-fatal to the linker, so this cannot make
the image less bootable than it already is: worst case the symbol stays missing
and the HAL keeps crash-looping exactly as before.

## The second bug: the VINTF manifest (proven by live patch)

With the shim in place the `dlopen` error disappeared and the service exit code
changed from 1 to 0 — but it still did not stay up:

```
E HidlServiceManagement: Service android.hardware.boot@1.2::IBootControl/default
    must be in VINTF manifest in order to register/get.
E LegacySupport: Could not register service
    android.hardware.boot@1.0::IBootControl/default (-2147483648).
```

`hwservicemanager` refuses any `registerAsService()` whose fqname is not in the
device manifest. Our `recovery/root/vendor/etc/vintf/manifest.xml` declared only:

```xml
<name>android.hardware.boot</name>
<version>1.0</version>
...
<fqname>@1.0::IBootControl/default</fqname>
```

The MTK implementation registers **both** 1.0 and 1.2. The 1.2 registration is
rejected, the process exits, and recovery keeps retrying 1.0 against a service
that never survives. Fix — add 1.1 and 1.2:

```xml
<version>1.0</version>
<version>1.1</version>
<version>1.2</version>
...
<fqname>@1.0::IBootControl/default</fqname>
<fqname>@1.1::IBootControl/default</fqname>
<fqname>@1.2::IBootControl/default</fqname>
```

**Validated live, not just theorised.** `/` is a writable `rootfs` in recovery,
so the manifest was pushed to `/vendor/etc/vintf/manifest.xml` and
`hwservicemanager` restarted (it is not `critical`; `adbd` survives). After the
restart:

```
I HidlServiceManagement: Registered android.hardware.boot@1.2::IBootControl/default
I LegacySupport: Registration complete for android.hardware.boot@1.0::IBootControl/default.
I recovery: Using HIDL version 1.2 of IBootControl
I:Is encrypted, do decrypt page first
```

`init.svc.boot-hal-1-2` -> `running`. **Both fixes are required**: the shim alone
gets the lib loaded, the manifest alone would still fail `dlopen`.

### Manifest audit: no third bug of this class

Checked every `interface` declared in the ramdisk's `.rc` files against the
manifest, and grepped the running device for refusals:

- Only `android.hardware.boot@1.2` was ever refused. Nothing else.
- `gatekeeper@1.0` (manifest line 100) and `keymaster@4.1` (line 150) are
  declared. `health` and `fastboot` are absent, but nothing is refused for them:
  `fastbootd` is `stopped` (normal outside fastbootd mode) and gatekeeper /
  keymaster are lazy HALs that start on demand. Do **not** add speculative
  declarations — the empirical list of refused fqnames is empty apart from boot.

## What is pushed / what is pending

Run #4 (`265c126`) shipped the shim half. These are **already on the branch**:

| File | Change |
| --- | --- |
| `.github/workflows/build-aera.yml` | `.repo` cache steps, `GOGC=40` / `GOMEMLIMIT=11GiB` / `GOMAXPROCS=2`, `-j16` sync, `cpio` + `xz-utils`, shim build + symbol assertion + recovery-root verification |
| `device/xiaomi/selene/BoardConfig.mk` | `BUILD_BROKEN_MISSING_REQUIRED_MODULES := true` |
| `device/xiaomi/selene/aera_selene.mk` | dropped `AERA_WORKAROUND_BACKUP_BUG` / `AERA_OPTIONS_LIST_NUM` |
| `device/xiaomi/selene/device.mk` | `PRODUCT_PACKAGES += libaera_boot_trim` |
| `device/xiaomi/selene/bootctl_compat/trim_shim.cpp` | the shim |
| `device/xiaomi/selene/bootctl_compat/Android.bp` | shim module |
| `device/xiaomi/selene/recovery/root/init.recovery.mt6768.rc` | `export LD_PRELOAD` for the shim |

Still **pending** (this is the next push, which triggers run #5):

| File | Change |
| --- | --- |
| `device/xiaomi/selene/recovery/root/vendor/etc/vintf/manifest.xml` | declare `android.hardware.boot` 1.1 + 1.2 and their fqnames |
| `.github/workflows/build-aera.yml` | fail the run if the staged manifest lacks `@1.2::IBootControl/default` |
| `HANDOFF.md` | this file |

## Run history

| Run | Result | Reached | Cause |
| --- | --- | --- | --- |
| #1 `43495414023` | failure, 25 min | `lunch` | `PRODUCT_USE_DYNAMIC_PARTITIONS` in `BoardConfig.mk` -> `cannot assign to readonly variable` |
| #2 `43501925957` | failure, 4 min | deps | `libncurses5` / `lib32ncurses5-dev` do not exist on Ubuntu 24.04; one bad name aborts the whole apt transaction |
| #3 `43503314394` | **success** | boots to recovery, UI blocked | boot-HAL prebuilt ABI break (one symbol) |
| #4 `43566176237` | **success** | decryption PIN screen (after live manifest patch) | shim fixed `dlopen`; VINTF refusal on boot@1.2 remained — manifest fix unpushed |

## Next time you sit down

1. Push the pending change set above (it triggers run #5).
2. In run #5's log, the `Build` step should print `shim built: ...`,
   `boot-HAL shim staged at: ...`, a `readelf` line with
   `_ZN7android4base4Trim...`, and `manifest declares boot@1.2: ...`. Any
   missing line fails the run on purpose — read the `::error::` line.
3. Flash and confirm **without any live patching**:
   - `getprop init.svc.boot-hal-1-2` -> `running`
   - `logcat -d | grep -E "Registered android.hardware.boot@1.2|Using HIDL version 1.2"`
   - `recovery.log` ends at `I:Is encrypted, do decrypt page first`
   - the PIN screen appears.
4. Anything after that (actual decryption, WLAN, browser sandbox) is ordinary
   debugging — see "Open risk: decryption".

If a failure traces to keymaster / relink / FBE, see "Open risk: decryption".


### How to push (no git/gh installed locally)

All pushes go through the GitHub REST Git Data API from PowerShell using the PAT
in `.gh_token`: create blobs (base64) -> create tree with `base_tree` = current
HEAD tree -> create commit -> `PATCH /git/refs/heads/aera-16.0`. Only the files
listed under "Still pending" need to go in the tree.

Gotcha that cost time: build the tree URL by string concatenation, not
`"$api/git/trees/$head?recursive=1"` — PowerShell mangles the `?` and GitHub
returns 404.

## Pipeline as it stands

| Piece | File |
| --- | --- |
| Workflow (compiles on the runner) | `.github/workflows/build-aera.yml` |
| Device tree | `device/xiaomi/selene/` |
| Docs | `docs/BUILDING.md` |

Sequence: `slimhub` cleanup -> 30 GB swap -> deps + `repo` -> restore `.repo`
cache -> `repo init --depth=1` -> `repo sync` with retries -> save cache ->
copy device tree -> `rm -rf .repo` -> `export AERA_BUILD_DEVICE=selene` ->
`source build/envsetup.sh` -> `lunch twrp_selene-bp2a-eng` ->
`mka adbd bootimage` (falls back to `recoveryimage`) -> release upload.

Workflow validated with `yaml.safe_load`: 14 steps, order correct:

```
Restore source cache -> Sync AERA sources -> Save source cache
  -> Add the device tree -> Drop .repo -> Build -> Collect images
  -> Upload workflow artifact -> Publish the release -> Report failure
```

## Facts learned the hard way (do not regress)

- **The repository must stay public.** A private repo gets a 2 vCPU / 8 GB
  runner and Soong is OOM-killed ~48 minutes in. The AERA port
  `HIZVOICE/...camellia-AERA` is private and went 0-for-16. Public also means
  unlimited Actions minutes.
- **Run on `ubuntu-24.04` and do NOT use `maximize-build-space`.** Measured:
  24.04 gives `/dev/root 145G`, 110 GB free. On 22.04 the same action pooled us
  down to 84 GB with 25 GB free. It was costing us 61 GB.
- **30 GB of swap** (`pierotofy/set-swap-space`) is what lets the build survive
  Soong's peak RSS in 16 GB of RAM.
- **`soong_build` is silent for 5-15 minutes.** It is single-threaded graph
  analysis and prints nothing until it finishes. That is not a hang. ILYESwsa
  measured it OOM-killed at ~13.6 GB RSS on this runner class.
- **Product variables are readonly in `BoardConfig.mk`.** `product_config.mk`
  runs before `board_config.mk` and calls `readonly-product-vars`, so anything
  `PRODUCT_*` assigned in a board config is a hard make error. It belongs in the
  product make file (`device.mk`).
- **`rm -rf .repo` before building** reclaims ~18 GB. `.repo` holds the history
  of ~1000 projects and is useless once the working tree exists.
- **`AERA_BUILD_DEVICE` must be exported before `source build/envsetup.sh`**
  (AERA wiki). Setting it inside the vendorsetup guard is too late.
- The AERA manifest is ~989 projects; `remove-minimal.xml` removes 601 of them.
  Composition: `default.xml` -> `twrp-default.xml` -> `aera.xml` +
  `remove-minimal.xml`.
- `selene` is recovery-as-boot (`BOARD_USES_RECOVERY_AS_BOOT := true`), so the
  target is `bootimage`, not `recoveryimage`.
- The lunch combo is `twrp_selene-bp2a-eng`. There is no `aera_selene` product
  and `twrp_selene-eng` is not a valid combo (AERA 16 needs the `bp2a` release
  name).
- MTK boot offsets verified against the original PBRP selene tree:
  `BOARD_RAMDISK_OFFSET 0x07c08000` / `BOARD_KERNEL_TAGS_OFFSET 0x0bc08000`,
  `BOARD_KERNEL_BASE 0x40078000`, pagesize 2048, header version 2. All correct,
  no change needed.
- **Android 13 changed `android::base::Trim` to take `std::string_view`.** Any
  Android 11/12 vendor prebuilt that imports the old `std::string const&`
  overload will fail `dlopen` on Android 16 with
  `cannot locate symbol "_ZN7android4base4TrimERKNSt3__112basic_string..."`.
  Check every prebuilt in `recovery/root` for this before blaming recovery code.
  `libaera_boot_trim` (see above) is the shim for the boot HAL.
- **A `dlopen` failure of a HIDL service is not survivable in recovery.** The
  `hwservicemanager` lazy-HAL retry and `BootControlClient::WaitForService()`
  are both infinite, so a missing HAL blocks the UI forever instead of
  degrading. A crash-looping service in `dmesg` is therefore a top-priority
  symptom, not noise.
- **`TARGET_RECOVERY_DEVICE_MODULES` does not exist in Android 16.** To get a
  file into the recovery ramdisk, either use `PRODUCT_PACKAGES` with
  `recovery_available: true`, or drop it on the `recovery/root/` overlay (which
  is what this tree does, and what the MTK HAL prebuilts rely on).
- **init `export` reaches service processes.** Proven: keymaster has a hard
  `DT_NEEDED` on `libTEECommon.so` (in `/vendor/lib64`), has no RUNPATH, and
  only the global `export LD_LIBRARY_PATH` in `init.recovery.mt6768.rc` can be
  putting that directory on its search path. Use `export` when the target
  service's `.rc` is build-installed and you cannot own it.
- **`hwservicemanager` enforces the VINTF manifest on `registerAsService()`.**
  A HIDL service whose fqname is not declared is refused with
  `must be in VINTF manifest in order to register/get`, and the process then
  exits — often with status **0**, which looks like success in `logcat`. The
  manifest is parsed at `hwservicemanager` startup, so a live edit only takes
  effect after `setprop ctl.restart hwservicemanager` (it is not `critical`).
  Declare every version the implementation registers, not just the oldest.

## Source caching: measured, not guessed

From ILYESwsa's `pbrp_device_oppo_f11` run #8 (same manifest, same runner class):

```
Cache Size: ~17932 MB (18802899874 B)
Cache restored successfully

actions/cache/restore   3m39s
repo sync --force-sync  3m46s
total                  ~7m30s      (vs ~20m cold)
```

Two things worth remembering because they are counter-intuitive:

- An **18.8 GB** cache entry restores fine despite the 10 GB free-tier figure.
  Do not "fix" it by trimming the cached path.
- `--force-sync` does **not** re-download; the objects are already in
  `.repo/projects`. It only re-materialises the working tree.

First run with the cache steps pays a one-time cost: cold 20 min sync, then an
~18 GB cache upload. Only the second and later runs get the 12 minutes back.
The save is `continue-on-error: true` so a cache failure can never fail a build,
and it must run **before** `Drop .repo` or it caches a deleted directory.

## Config audit against the AERA reference (2026-10-15, 149 flags)

Every `AERA_*` in the tree was checked for placement and for a real consumer in
`aera_config.mk` / `aera_build.mk` / `AERA_A16.sh`.

| Flag | Was | Now |
| --- | --- | --- |
| `PRODUCT_USE_DYNAMIC_PARTITIONS` | `BoardConfig.mk` (hard error) | `device.mk` only |
| `AERA_SCREEN_H` / `AERA_STATUS_H` | `aera_selene.mk` | `BoardConfig.mk` |
| `AERA_MAINTAINER` / `AERA_NO_TREBLE_COMPATIBILITY_CHECK` | `aera_selene.mk` | `vendorsetup.sh`, inside the device guard |
| `AERA_USE_FSCRYPT_POLICY` | `1` (= v1) | `2` (= v2, matches our `:v2` fstab) |
| `AERA_DELETE_AROMAFM` / `AERA_DELETE_MAGISK_ADDON` | exported | removed — dead in A16 |
| `AERA_WORKAROUND_BACKUP_BUG` / `AERA_OPTIONS_LIST_NUM` | set | removed (AzzyC: merged into OFox / UI does not rely on it) |

Note: `AERA_SCREEN_H` / `AERA_STATUS_H` / `AERA_MAINTAINER` are placed in the
product `.mk` by *both* AzzyC (peridot) and ILYESwsa (F11), while the reference
page says `BoardConfig.mk` / `vendorsetup.sh`. Both work — board config is
parsed earlier, so it is the safer of the two. Do not treat the reference as
wrong, but do not treat the product-mk placement as broken either.

`BUILD_BROKEN_MISSING_REQUIRED_MODULES` was verified legal before adding it: it
is on AOSP 16's whitelist (`build/make/core/board_config.mk:181`), and an
unlisted `BUILD_BROKEN_*` is itself a build error.

## Checked: the WLAN switch is correct

`aera_selene.mk` sets `AERA_ENABLE_WLAN := 0`. `aera_config.mk` maps it verbatim
to `OF_ENABLE_WLAN`, and `aera_defaults.go` tests the literal `"1"`:

```go
if getMakeVars(ctx, "OF_ENABLE_WLAN") == "1" {
    aeraFlags = append(aeraFlags, "-DOF_ENABLE_WLAN")
}
```

So `"0"` really does keep WLAN off. Contrast with `AERA_USE_FSCRYPT_POLICY`,
where `1` *does* mean v1. The semantics are per-flag; always check the consumer.

## Open risk: decryption

**Not the current blocker.** Run #3 proved the hang happens *before* decryption
runs: recovery blocks in `IBootControl::getService()` while the boot HAL
crash-loops. `I:Can't probe device /dev/block/mmcblk0p53` is expected for a
metadata-encrypted `/data` and is not an error. Revisit this section only if
decryption itself fails *after* the UI appears.

We build with full crypto on (`AERA_INCLUDE_CRYPTO`, `_FBE`,
`_FBE_METADATA_DECRYPT`, `AERA_USE_FSCRYPT_POLICY := 2`, MTK keymaster prebuilts
plus a hand-written `AERA_RECOVERY_ADDITIONAL_RELINK_LIBRARY_FILES` list).

ILYESwsa ships `AERA_INCLUDE_CRYPTO := false` with the comment *"the first goal
is only to boot"*. If a build failure traces back to keymaster / relink / FBE,
turn crypto off, get a booting image, then re-enable it as a second iteration.
A recovery you can boot and flash from beats a perfect one that never compiles.

## Secrets

- `.gh_token` in this folder is a personal PAT used for the REST pushes.
  **Revoke it when the project is done.**
