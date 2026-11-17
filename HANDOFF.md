# Handoff / current status

Last updated: 2026-11-17 (**Mali GPU acceleration works on the device.** Build
`43961488385` (`ca912c22`) produced a `boot.img` whose recovery binary carries
all three GPU patches, and the flashed image composites the AERA UI on the Mali
GPU. The black screen that cost weeks was a CI defect -- the build job never
checked this repository out, so `patches applied: 0`)

## GPU acceleration: works

Flashed, booted and confirmed on the device on 2026-11-17. The AERA UI is
composited by the Mali GPU and presented through the MediaTek composer HAL, which
runs **inside** the recovery process.

### What was built

| | |
| --- | --- |
| run | `43961488385`, commit `ca912c22`, job `133629762686` |
| result | success; Build step 21:55:28 -> 22:28:08 UTC (32m40s, warm ccache) |
| `boot.img` | 67,108,864 B, md5 `440DFF5DFEAD935D5B3101797F40252E` |
| repacked onto the dieD 4.19 kernel | 67,108,864 B, md5 `749DE5C7C8270F2FDF3D81DC8847FE04` |
| `/system/bin/recovery` | 11,053,408 B, md5 `6173f6a3ca442240702639eacb3cece4` |
| rollback | `/sdcard/boot_stock_android.img`, md5 `65275652ea012c5b04f85207c4d23627` |

The repack is required: AERA's `boot.img` carries its own kernel, and only the
dieD 4.19 kernel boots on this device. `repack_died.py` takes the kernel,
recdtbo and dtb from `boot_a.img` and the ramdisk from AERA's image.

### Why the binary is the proof, not the log

```
$ grep -ac "vsync/refresh registration" /system/bin/recovery
1
$ grep -ac "dma-buf import %u failed" /system/bin/recovery
1
```

Both are `Fail()` strings that exist only in the patched source. The workflow
asserts them after the build, so a green run cannot ship an unpatched image
again.

### Why the GPU is the proof, not the fps counter

```
$ grep -aE 'libGLES_mali|hwcomposer|gralloc' /proc/409/maps | awk '{print $6}' | sort -u
/gpu/stock-vendor/lib64/egl/libGLES_mali.so
/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so
/gpu/stock-vendor/lib64/libgralloc_extra.so
/gpu/stock-vendor/lib64/libgralloc_metadata.so
/gpu/stock-vendor/lib64/libgralloctypes.so
/gpu/stock-vendor/lib64/libgralloctypes_mtk.so

$ ls -l /proc/409/fd | grep -a mali
lr-x------ ... 19 -> anon_inode:malitl_409_0xb4000073c26de090
lr-x------ ... 20 -> anon_inode:malitl_409_0xb4000073426dd7d0
lrwx------ ...  8 -> /dev/mali0
```

`/dev/mali0` open in the recovery process, plus two `malitl_*` timeline fences,
means the Mali driver is executing jobs for pid 409 and the process is waiting on
their completion. Nothing else in recovery opens the GPU: the composer HAL drives
the display engine (OVL/MDP), not the shader cores.

The kernel agrees -- the MTK GPU governor is cycling while the UI is touched:

```
[GPU/DVFS][INFO]@__mt_gpufreq_update_max_limited_idx: g_max_limited_idx = 3, g_limiter = 4
[GPU/DVFS][INFO]@__mt_gpufreq_update_max_limited_idx: g_max_limited_idx = 0, g_limiter = -1
```

and the composer reports `Display_0 (type:1) fps:12.2-23.9` under interaction
against `fps:0.99` idle.

### The AeraGpu log lines are missing, and that is expected

`logcat -d -s AeraGpu` returns nothing even though the binary contains the
`"zero-copy native-buffer GPU ready: EGL %d.%d, ..."` string. The main ring
buffer on this build is **256 KiB** and the kernel's GPU-DVFS chatter wraps it in
minutes:

```
main: ring buffer is 256 KiB (253 KiB consumed, 2 MiB readable)
kernel: ring buffer is 256 KiB (209 KiB consumed, 695 KiB readable)
```

Do not read the absence as a fallback. Check `/proc/<pid>/maps` instead.

Also note `cannot find/open a drm device ... Using fbdev graphics` in
`/tmp/recovery.log`: that is TWRP's minui backend drawing the very first frames,
and it is unrelated to the AERA UI path.

### The three defects, in the order they were found

1. **No checkout in the build job.** `${GITHUB_WORKSPACE}/patches` did not exist,
   `globstar nullglob` turned the loop into a no-op, and the step reported
   success. Fixed in `b26df280`: checkout at `path: src`, guards on directory /
   glob / count, and a post-build grep of the binary for the markers.
2. **Display 0 never connected.** `HWCMediator::deviceRegisterCallback()` runs
   `DisplayManager::init()` from the **vsync** callback, so a hotplug delivered
   during init is dropped. Patch `0002` registers vsync, refresh, hotplug, then
   hotplug again, with a bounded 2 s poll behind it.
3. **The composer was unloaded.** Recovery `dlclose()`d the composer HAL after
   init. Patch `0001` keeps it resident.

Plus `0003`, which imports the scanout buffers as dma-bufs
(`EGL_EXT_image_dma_buf_import`, fourcc `AB24`, pitch `stride*4`, offset 0)
instead of copying them, and the `gralloc1_compat` shim, which had to be
arm64-only (`compile_multilib: "64"`, fixed in `deccfd07`) because the 32-bit
build cannot resolve the 64-bit allocator's symbols.

### Flashing, and how to get back

```sh
adb push boot_died_aera_43961488385.img /data/local/tmp/aera_new.img
adb shell dd if=/dev/block/by-name/boot_a of=/sdcard/boot_stock_android.img bs=1M
adb shell dd if=/data/local/tmp/aera_new.img of=/dev/block/by-name/boot_a bs=1M conv=fsync
adb shell md5sum /dev/block/by-name/boot_a      # must be 749de5c7c8270f2fdf3d81dc8847fe04
adb reboot
```

`boot_a` is `mmcblk0p27`. To roll back, `dd` `/sdcard/boot_stock_android.img`
back over it. AVB is not enforced here, so no re-signing is needed.


## Read this first

**The recovery works end to end.** It boots, shows the AERA lock screen,
decrypts `/data` with the PIN, and browses files. Getting there needed **two**
independent fixes, both proven on the device and both now committed:

1. **ABI shim** — the A11 MTK boot-HAL prebuilt could not `dlopen` against A16
   `libbase`. Fixed by `libaera_boot_trim` (run #4).
2. **VINTF declaration** — with the shim the HAL loaded and registered 1.0 but
   was refused on 1.2, so it exited. Declaring 1.0/1.1/1.2 in the vendor
   manifest keeps it up (proven by live patch; committed in `3a901f1`).

- Run #4: id `43566176237`, commit `265c126`, branch `aera-16.0` — **success**
- Flashed and used: `init.svc.boot-hal-1-2` -> `running`, decryption PIN
  accepted, `/data` mounted rw, file manager works, zip installs work.
- So build, boot header, MTK offsets, kernel, init, adbd, keymaster, the ramdisk,
  the boot HAL and decryption are all **correct**.

**Run #4's image is not the finished one.** It still carries the old manifest and
the old *global* `LD_PRELOAD`. Both are fixed on the branch now; the next build
is the first one that needs **no live patching** after flashing.

**The one thing still broken at flash time is the scoped preload**, which is the
whole point of the next build — see "The preload must be scoped" below.

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

### The fix (committed)

A ~10 KB shim re-exporting the one missing symbol, `LD_PRELOAD`ed into the HAL:

| File | Change |
| --- | --- |
| `device/xiaomi/selene/bootctl_compat/trim_shim.cpp` | **new** — `android::base::Trim(const std::string&)` |
| `device/xiaomi/selene/bootctl_compat/Android.bp` | **new** — `cc_library_shared` `libaera_boot_trim`, `stl: "libc++"`, `-fvisibility=default` |
| `device/xiaomi/selene/device.mk` | `PRODUCT_PACKAGES += libaera_boot_trim` |
| `device/xiaomi/selene/recovery/root/system/etc/init/android.hardware.boot@1.2-service.rc` | **new** — override of the boot HAL service, adding `setenv LD_PRELOAD /sbin/libaera_boot_trim.so` |
| `device/xiaomi/selene/recovery/root/init.recovery.mt6768.rc` | global `export LD_PRELOAD` **removed** (see below) |
| `.github/workflows/build-aera.yml` | builds the shim first, `readelf`-asserts the exact mangled symbol, stages it at `recovery/root/sbin/`, and fails the run if the staged root lacks the shim, the `.rc` `setenv`, or the manifest `@1.2` fqname |

### The preload must be scoped (the next build's reason to exist)

Run #4 shipped `export LD_PRELOAD /system/lib64/libaera_boot_trim.so` in
`on init`. That is **wrong in two independent ways**, both seen on the device:

1. **Global scope breaks every dynamic binary once a zip mounts `/system`.**
   A zip installer mounts the system partition over the ramdisk's `/system`, so
   the preload path stops existing and even `getprop` dies:

   ```
   CANNOT LINK EXECUTABLE "getprop": library
       "/system/lib64/libaera_boot_trim.so" not found: needed by main executable
   ```

   That is the gapps installer failing for our reason, not its own.

2. **`/system` is the wrong home for it anyway.** The shim must live somewhere
   a mount can never shadow. `/sbin` is a real rootfs directory (it already
   holds `aerastart.sh`, `busybox`, `setgovernor`) and `/tmp` is tmpfs.

So the preload is now **scoped to the one service that needs it**, via an
override of `android.hardware.boot@1.2-service.rc`, and points at `/sbin`:

```
service boot-hal-1-2 /system/bin/android.hardware.boot@1.2-service
    ...
    setenv LD_PRELOAD /sbin/libaera_boot_trim.so
```

Overriding that `.rc` is safe: it is a plain build-installed file, and the
recovery overlay (`recovery/root/`) is applied after it, so ours wins. The shim's
own `DT_NEEDED` set is only `libc++/libc/libm/libdl`, which resolve from both the
ramdisk and any mounted system, so `/sbin` placement always works.

A missing `LD_PRELOAD` entry is non-fatal to the linker, so this cannot make the
image less bootable: worst case the symbol stays missing and the HAL keeps
crash-looping exactly as before.

- **Overlay copy, not `TARGET_RECOVERY_DEVICE_MODULES`.** That variable does
  not exist in Android 16's build system. `recovery/root/` is the mechanism
  this tree already uses to get the MTK boot HAL into `/system/lib64/hw/`.

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

### Known cosmetic issue: health HAL retry

`recovery` (pid 374) also asks for the health HAL once per second, forever:

```
I recovery: Unable to get AIDL health service, trying HIDL...
servicemanager: Could not find android.hardware.health.IHealth/default in the
    VINTF manifest
W HidlServiceManagement: Waited one second for
    android.hardware.health@2.0::IHealth/default
```

**This does not affect the battery indicator** — the UI reads
`/sys/class/power_supply/battery` directly (verified on-device: `capacity 79`,
`status Charging`, `voltage_now 4219000`, `current_now -252800`, `temp 316`).

It is **not cheaply fixable** and was deliberately left alone. Recovery asks AIDL
`android.hardware.health.IHealth` first, but the ramdisk only ships the HIDL
`android.hardware.health@2.1-service` (which registers 2.1, not 2.0), and there
is no AIDL health service or `.rc` anywhere in the tree. Starting the 2.1
service by hand works (`Hal init done`, `Registered
android.hardware.health@2.1::IHealth/default`, battery `l=80 v=4225 t=31.2`) but
does not satisfy the client. Fixing it means shipping an AIDL health HAL or
patching recovery's own client — out of scope for a cosmetic log line.

## Known limitation: frame rate (~4 fps)

The UI is usable but runs at roughly **4 fps** while animating. This is measured,
not guessed, and it is a property of the AERA UI's software renderer on this
SoC, not a driver, governor or thermal fault.

Measured on the device:

- `CLK_TCK=100`. The UI thread is **tid 399**. While animating it consumes
  30-100 % of one core; the `recovery` main thread is parked in `futex_wait` and
  consumes nothing. (An earlier reading of "44 % on the main thread" was the
  wrong thread and led to the wrong conclusion.)
- The engine's own instrumentation never prints its
  `active animation submitted %.1f fps` line. `TrackSubmittedFrame()` resets its
  window whenever a frame arrives **more than 250 ms** after the previous one,
  so the absence of that line is itself proof that the frame interval exceeds
  250 ms — i.e. under 4 fps.
- All 8 cores sit at max clock and thermals are 31-33 °C, so it is not
  throttling. There is no `/dev/dri`, so there is no DRM/GPU path at all:
  `recovery.log` says `cannot find/open a drm device`.

Why it is slow — the render path does far more work than the panel needs:

1. `DisplayTransform::Adaptive()` hardcodes `logical_width = kDesignWidth`
   (**1440**) and takes the height from `AERA_SCREEN_H`. Screen height is given
   in 1080-wide reference units and is converted to 1440-wide space (x4/3), so
   `AERA_SCREEN_H := 2400` yields a **1440x3200** canvas. That is **4.6 M
   pixels** rendered every frame against a 1080x2400 panel of 2.6 M pixels —
   ~78 % more pixels than are ever displayed.
2. `IsScaled()` is therefore true, so every frame the finished 1440x3200 surface
   is **bilinear-downscaled** to 1080x2400 by `res_scale_surface()`, which is
   libpixelflinger (`GGL_LINEAR`) — a per-pixel software GL loop.
3. Then `gr_blit()` copies 1080x2400 into the minui surface and `gr_flip()`
   memcpys another 10.4 MB into the framebuffer and calls
   `FBIOPUT_VSCREENINFO`.

So each frame is a full-canvas software render plus a software rescale plus two
full-frame copies. `LV_DISPLAY_RENDER_MODE_FULL` means the whole canvas is
redrawn even for a one-widget change.

**Not fixed, deliberately.** The only device-tree lever is
`AERA_UI_ADAPTIVE_RESOLUTION := false`, which switches to native 1080x2400 and
removes the rescale. It is **not safe**: the pages are authored in the 1440-wide
coordinate space with hardcoded sizes, e.g. `decrypt_scene.cpp` does
`lv_obj_set_size(state->keyboard, 1012, 1010)` and
`lv_obj_set_size(key, 224, 224)`. At a 1080-wide canvas those overflow and clip.
Only some scenes use a scaling helper (`lock_scene.cpp` uses `Px(s, 1100)`), so
native mode would break the decrypt/pattern screens specifically — the ones you
cannot get past without. Left as a documented limitation.

**Why it cannot be tested without a build:** `adaptive_resolution` and
`logical_height` arrive as a compile-time `DisplayMetrics` struct passed into
`RunLoop()`; there is no property, file or CLI switch for either. `AERA_SCREEN_H`
cannot be lowered to compensate either — the canvas must stay 1440x3200 to match
the panel's 0.45 aspect, otherwise the X and Y scales diverge and the UI is
squashed.

Cheap things already ruled out, so nobody re-investigates them:

- **Not the display panel.** `rdma0`/`ovl0`/`dsi0` IRQs are frozen when the UI
  is idle (event-driven, no continuous scanout), and `idle_state` is `active`
  while touching.
- **Not `dock_blur=24` / wallpaper blur.** Blur is implemented in the GLSL
  shader path (`u_IsBlur`), which is only used when `gpu_accelerated_` is true.
  There is no GPU here, so blur never runs.
- **Not `FBIOPUT_VSCREENINFO` latency alone.** Single-buffering would remove one
  ioctl and one 10.4 MB copy, but the dominant cost is rendering 4.6 M pixels.

## Storage: the "failing" MicroSD was simply not inserted

A backup failed with `mkdir: '/sdcard1/AERA_write_test': I/O error` and

```
Buffer I/O error on dev mmcblk1p1, logical block 832, async page read
```

which looks exactly like dying media. It is not. Check the block layer before
believing a storage error:

```
$ cat /proc/partitions        # only mmcblk0* — no mmcblk1 at all
$ ls /sys/block/              # no mmcblk1
$ ls /dev/block/ | grep mmc   # mmcblk0* only
$ cat /sys/class/mmc_host/mmc1/device/name   # empty
```

`mmcblk1` was **absent from the block layer entirely** while `/sdcard1` was
still mounted from `/dev/block/mmcblk1p1` via `exfat-fuse`. A mount whose block
device has disappeared returns `EIO` for every access, forever. The card had
been pulled (it was tested in another phone and worked there).

**Lesson: `EIO` on a removable mount is a staleness symptom first and a
media-failure symptom second.** Confirm the device still exists in
`/proc/partitions` before concluding the card is bad. Reseat the card and remount
`/sdcard1`; do not reformat anything on the strength of an `EIO` alone.

Internal `/sdcard` (`/dev/block/dm-7`, f2fs) is unaffected and is a safe backup
target meanwhile.

## MTP cannot work on this kernel (use USB mass storage instead)

`E:[MTP] could not open MTP driver, errno: 2` is not a recovery bug. The kernel
was built without `CONFIG_USB_CONFIGFS_F_MTP`, so the function never exists:

```
$ ls /config/usb_gadget/g1/functions/
ffs.adb  ffs.fastboot  mass_storage.usb0        # no mtp.gs0
$ ls /dev/mtp_usb                               # No such file
$ getprop sys.usb.config                        # mtp,adb  (set, but hollow)
```

`/init.recovery.usb.rc` does `mkdir /config/usb_gadget/g1/functions/mtp.gs0` and
symlinks it to `configs/b.1/f1`; both fail silently because the kernel does not
implement the function. `sys.usb.config=mtp,adb` and `sys.usb.ffs.mtp.ready=1`
are therefore set with nothing behind them. This is a **kernel config** issue
and cannot be fixed from the device tree.

**Workaround: USB mass storage.** `mass_storage.usb0` *is* supported (the
function directory exists and `TARGET_USE_CUSTOM_LUN_FILE_PATH` already points
at `.../mass_storage.usb0/lun.%d/file`), so "Mount -> USB Storage" exposes
`/sdcard` as an ordinary USB drive. Use that instead of MTP for pulling files.

## Verified working on the device

After run #4 (`43566176237`) plus the offline repack described below, all of the
following was exercised on the phone, not inferred:

| Feature | Evidence |
| --- | --- |
| Boot to the AERA UI | `boot_a` runs the patched image, with no live patching |
| `/data` decryption | PIN screen accepted, `/sdcard` browsable |
| Backup | `/sdcard1/AERA/BACKUPS/55ec505f0603/AERA-2026-11-14-12-56-14/boot.emmc.win` (64 MB) + `.sha2` + `recovery.log` |
| Zip flash | `aera flash /sdcard/aera_flash_test.zip` -> `{"code":0}`; the zip's own marker printed `data_mounted=1`, `boot_hal=running`, `AERA FLASH TEST: success` |
| MicroSD | `mmcblk1`, 58.3 GiB, exFAT via `exfat-fuse`, ~20 GB free |
| Boot HAL | `init.svc.boot-hal-1-2=running`, `/sbin/libaera_boot_trim.so` present, shell `LD_PRELOAD` unset, logcat `Using HIDL version 1.2 of IBootControl` |

### The offline boot-image repack (how the two fixes reached the phone without a build)

Run #4's image had the boot-HAL shim but neither the scoped `LD_PRELOAD` nor the
1.2 manifest entry, and there was no broadband to rebuild. Both fixes are pure
ramdisk content, so the boot image was rebuilt locally instead of waiting:

- `build_boot.py` unpacks the ramdisk (`cpio`), applies the `.rc` `setenv`, adds
  the manifest `1.2::IBootControl` entry, drops the shim at `/sbin`, re-packs
  (`cpio` + `gzip`), and rebuilds the Android boot header v2 (page 2048) plus
  the AVB footer.
- AVB does **not** need re-signing: `verifiedbootstate=orange`,
  `veritymode=disabled` and `BOARD_AVB_MAKE_VBMETA_IMAGE_ARGS += --flags 3` mean
  verification is skipped, so the original vbmeta blob is kept and only its
  offset moves.
- Result: `62,566,400` bytes with a `49,761,670` byte ramdisk. `boot_a` is
  64 MB, so there is only ~1.5 MB of headroom. **The 39 MB Mali stack can never
  be embedded in the boot image**; it has to come from the stock vendor
  partition at runtime.
- `boot_a.img` (pristine) is kept next to the patched image for
  `fastboot flash boot boot_a.img`.

### CLI: `aera backup --parts` needs TWRP-style mount points

`aera backup --parts boot` fails with `backup_failed` and produces no log lines,
because the backend validates the name against TWRP mount points. `aera backup
--parts /boot` works. The GUI is unaffected (it passes `/boot`).

## Mali GPU: the stock stack is gralloc4, and the backend needs gralloc1 (verified on the device)

apkreader's `mali-testing` backend is merged and compiles, but it **cannot**
accelerate selene. This is not a configuration mistake; the HAL it calls does
not exist on the device. This was first inferred from the public
`V14.0.7.0.TKUMIXM` dump; it has since been **verified on the live device**, and
the live run corrected two things the dump had wrong.

### Live evidence: the whole vendor stack resolves under AERA 16

`/system/bin/selene-gpu-test.sh` was run under AERA 16 (SDK 36) with the stock
volumes bound under `/gpu`. It stages `/gpu/lib64` and runs `linker64 --list` on
each library, which reports every dependency the linker can actually resolve.

| Path | Size (live) | Result |
| --- | --- | --- |
| `vendor/lib64/egl/libGLES_mali.so` | 39,702,064 | **67 dependencies, zero missing symbols** |
| `vendor/lib64/hw/hwcomposer.mt6768.so` | 1,313,049 | resolves fully, HWC2 (`common.version >> 24 == 2`) |
| `vendor/lib64/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so` | 65,952 | resolves fully, exports `HIDL_FETCH_IAllocator` |
| `vendor/lib64/hw/android.hardware.graphics.mapper@4.0-impl-mediatek.so` | 140,936 | resolves fully, exports `HIDL_FETCH_IMapper` |
| `vendor/lib64/hw/gralloc.default.so` | 23,760 | AOSP fbdev **gralloc0**: `HMI`, `fb_device_open`, `mapFrameBufferLocked`, imports `ashmem_create_region` |

So the open question the probe existed to answer is answered: **an
Android 12/13-era vendor graphics stack does load inside an Android 16 linker
namespace.** `libGLES_mali.so` needs `libnativewindow`, `libged`, `libgpu_aux`,
`libgpud`, `libgralloc_extra`, `android.hardware.graphics.mapper@4.0`,
`libgralloctypes` and `libhidlbase`, and all of them are satisfied.

### The one missing piece: there is no gralloc1 module

The device is **gralloc4/HIDL**, not gralloc1. `find` across both partitions
returns exactly two gralloc files, and both are the same thing:

```
/vendor/lib64/hw/gralloc.default.so     23,760   AOSP fbdev gralloc0
/vendor/lib/hw/gralloc.default.so       11,772   AOSP fbdev gralloc0
```

This was re-checked against the **stock firmware dump**
(`Jiovanni-dump/redmi_selene_dump`, branch
`missi_phoneext4_global-user-13-TP1A.220624.014-V14.0.7.0.TKUMIXM-release-keys`),
because the phone runs a heavily modified vendor and a modified vendor could in
principle have dropped a module that stock ships. It did not: the dump's
10,783-entry tree contains no `gralloc1` provider either, and its
`vendor/lib64/hw/gralloc.default.so` (20,080 B) is the same fbdev gralloc0 —
`fb_device_open`, `mapFrameBufferLocked`, `gralloc_register_buffer`,
`terminateBuffer`, and not one `gralloc1` string. The dump's branch name also
confirms the vendor is Android 13 (`user-13-TP1A.220624.014`); the `:12/` in
`ro.vendor.build.fingerprint` is a Xiaomi labelling quirk, not the API level.

So the absence is a property of **the platform**, not of the modification.

- `/system/lib64/hw/` and `/system/lib/hw/` contain **no gralloc at all** — only
  the audio impls (`audio@2.0` … `audio@7.1`, `audio.sysbta.default.so`). All
  graphics HALs live on `/vendor/lib64/hw`. Do not go looking on `/system`.
- The vendor provides gralloc4 as HIDL passthroughs:
  `allocator@4.0-impl-mediatek.so` + `mapper@4.0-impl-mediatek.so`, and the Mali
  driver is itself a gralloc4 client (it NEEDs `mapper@4.0` and
  `libgralloctypes`). That is what the stock framework uses.
- `gralloc.mt6768.so` was an Android 10/11 MediaTek artifact and is gone.
  `gralloc.default.so` is **not** a substitute: opening the generic fbdev
  gralloc0 module through `gralloc1_open()` hands the backend a mismatched
  function table.

The backend needs exactly this, from `aeraui/core/mali_display.cpp`:

- `gralloc1_open(module, &allocator)` plus `GRALLOC1_FUNCTION_{CREATE_DESCRIPTOR,
  DESTROY_DESCRIPTOR, SET_DIMENSIONS, SET_FORMAT, SET_PRODUCER_USAGE,
  SET_CONSUMER_USAGE, ALLOCATE, GET_STRIDE, RELEASE}`, then `GET_STRIDE` on each
  of 2-3 scanout buffers;
- an HWC2 `hwc2_device_t` whose `common.version >> 24 == 2` (satisfied).

### False lead: the gralloc1 strings are clients, not providers

`libgpud.so` and `sensors.mt6768.so` both contain `cannot open gralloc1 device
(%s)` and `GrallocHalWrapper::mapGralloc1Error`. That is **client** code — the
MTK userspace half of the old gralloc1 API, left behind after the provider was
removed. There is no `HMI`-exporting gralloc1 module on the device for either of
them to open, and neither one is a provider. Do not read those strings as
evidence that a gralloc1 module exists.

### Correction: the `memset_explicit` failure was OrangeFox, not selene

An earlier OrangeFox run reported `libGLES_mali.so` as needing `memset_explicit`,
which would have meant the driver could never load on this tree. That was an
artifact of OFox's ramdisk being Android 12L (SDK 32). **AERA 16's libc exports
`memset_explicit`**, and the driver resolves cleanly under it. Ignore any
OFox-era dependency failure when judging the Mali path.

### Correction: the shim is viable, and here is why the rejection was wrong

An earlier revision of this document rejected a gralloc1 shim on the grounds
that it would have to call `IAllocator::allocate` across an `sp<IAllocator>`
boundary, putting AOSP 16's `libhidlbase` in the same process as the vendor's
Android 12/13 copy. **That is not what a shim has to do.** MediaTek's allocator
impl also exports the plain ARM entry points:

```
_Z28mali_gralloc_buffer_allocateP19buffer_descriptor_tPP16private_handle_t
_Z24mali_gralloc_buffer_freeP16private_handle_t
_Z18allocator_allocatePK19buffer_descriptor_tPP16private_handle_t
_Z35mali_gralloc_derive_format_and_sizeP19buffer_descriptor_t
```

Calling `mali_gralloc_buffer_allocate` is a plain C++ call: no HIDL, no `sp<>`,
no second runtime. And because the shim is `dlopen`ed *into* `aera_mali`, the
libc/libc++ it uses are the ones the namespace already resolves, so there is not
even a libc mismatch to worry about. The handles it returns are MediaTek's own
`private_handle_t`, i.e. exactly what `hwcomposer.mt6768.so` expects to composite
as the client target.

`device/xiaomi/selene/gralloc1_compat/gralloc1_shim.cpp` implements the nine
`GRALLOC1_FUNCTION_*` entries the backend asks for on top of those symbols.

#### The descriptor is not ARM's public one

The public `buffer_descriptor_t` does not match this build, so the layout was
recovered from the vendor binary with `capstone` rather than guessed from a
header. Disassembling `mali_gralloc_buffer_allocate` (`0xb648`) and
`mali_gralloc_derive_format_and_size` (`0x9fc0`):

| Offset | Field | Evidence |
| --- | --- | --- |
| `+0x04` | `width` | `ldp w10, w11, [x0, #4]`; `str w8, [handle, #0x1c]` |
| `+0x08` | `height` | same pair; `str w8, [handle, #0x20]` |
| `+0x10` | `producer_usage` | `ldp x8, x9, [x0, #0x10]` |
| `+0x18` | `consumer_usage` | same pair; OR'd into one usage |
| `+0x20` | `hal_format` (u64) | `ldr w0, [x0, #0x20]` |
| `+0x28` | `layer_count` | read-before-write; `cmp w9, #2` in derive |
| `+0x34` | `format_type` | `ldr w1, [x19, #0x34]`, passed as `select_format`'s `mali_gralloc_format_type` |
| `+0x58` | `size` (u64) | derive writes it; `allocator_allocate` reads it as the ION length |
| `+0x68` | `internal_format` (u64) | `select_format`'s return value |
| `+0x74` | `pixel_stride` (16-px aligned width) | measured, see below |
| `+0x78` | aligned height | measured, see below |

The last two were **wrong in the first cut** — the disassembly suggested
`byte_stride` at `0x74` and `pixel_stride` at `0x78`, but the device says
otherwise:

| Request | `field[0x74]` | `field[0x78]` | `size` |
| --- | --- | --- | --- |
| 1081x100 | **1088** | 100 | 435200 |
| 1440x3200 | **1440** | 3200 | 18432000 |
| 1080x2400 | **1088** | 2400 | 10444800 |

`0x74` is the stride (1081 rounds up to 1088, a multiple of 16) and `0x78` is the
height. The old fallback ("if the value is below the width, use the width")
turned that into a plausible-looking `1081` for a 1081-wide buffer, i.e. a
sheared scanout instead of an error — which is why this had to be measured
rather than reasoned about.

The highest offset the vendor code touches is `0xac`, so the shim hands over a
zeroed `0x200`-byte buffer and pokes only these fields. Nothing else about
`private_handle_t` has to be known, because the vendor fills in its own handle.

Two values cannot be decided from this `.so` alone: `select_format` compares
`format_type` against `1` and skips usage-bit validation when it is `1` (which
reads as "hal_format is already internal", i.e. `0` == usage-based), but the
vendor's descriptor builder lives in a different library; and a fresh gralloc4
allocation sets `layer_count` to `1` while `0` may also work. Instead of paying a
two-hour build per guess, the shim probes both at first use, logs every attempt,
and remembers the winner. `/gpu/gralloc1.conf` pins them without a rebuild:

```
format_type=0
layer_count=1
probe=1
```

#### What the shim learned on the device

Everything is logged to `/tmp/gralloc1.log` (the shim has no liblog dependency
on purpose: the namespace shares only libc/libm/libdl, so a `liblog` `DT_NEEDED`
would have to be staged as well).

Three things came out of running it, and two of them were shim bugs:

1. **`gralloc1_open()` passes `"gralloc"`, not `"gpu0"`.** AOSP 16's inline
   `gralloc1_open()` calls `module->methods->open(module,
   GRALLOC_HARDWARE_MODULE_ID, ...)`, i.e. the string `"gralloc"`;
   `GRALLOC_HARDWARE_GPU0` (`"gpu0"`) is the gralloc0-era name. The shim only
   accepted `"gpu0"`, so every open failed with `unsupported id gralloc` before a
   descriptor was touched. It now accepts both (there is one device behind the
   module).
2. **`0x74` is the stride and `0x78` is the height** — see the table above.
3. **A 1080x2400 RGBA_8888 allocation is correct**: `error=0`, `size=10444800`,
   `internal_format=0x1`, `stride=1088`, and it frees cleanly. `format_type=0`
   and `layer_count=1` are the working pair, so the runtime probe in the shim is
   no longer needed on the happy path (`probe=0` pins them).

All of that was found with **`gralloc1-probe`**, not by rebooting into the
backend. It is built by `.github/workflows/gralloc1-shim.yml` (runner NDK,
~90 s) and pushed to `/data/local/tmp/`, and it does the same nine gralloc1 calls
`mali_display.cpp` does:

```
adb shell 'LD_LIBRARY_PATH=/gpu/lib64:/system/lib64 /data/local/tmp/gralloc1-probe'
adb shell 'LD_LIBRARY_PATH=/gpu/lib64:/system/lib64 /data/local/tmp/gralloc1-probe 1440 3200'
adb shell 'LD_LIBRARY_PATH=/gpu/lib64:/system/lib64 /data/local/tmp/gralloc1-probe --hwc'
adb shell 'LD_LIBRARY_PATH=/gpu/lib64:/system/lib64 /data/local/tmp/gralloc1-probe --hotplug'
```

`--hwc` opens/closes the vendor HWC2 module (the crash-loop path), `--hotplug`
registers hotplug+vsync+refresh, waits for a callback and queries displays 0..3
(`--only-hotplug` restores the old single-callback run), and `--lifecycle` calls
the ordinary display functions one at a time. None of them needs an image build,
and none restarts recovery.

**Do not go back to iterating on the shim through an image build.** The shim is
not in the ramdisk — it is staged into the blob store and named by
`ro.aera.gpu.gralloc` — so `build-aera.yml` no longer triggers on
`device/xiaomi/selene/gralloc1_compat/**` alone. The Soong build in the image is
still the canonical one and still asserts AArch64 / `HMI` exported / no foreign
`DT_NEEDED`, but it is not the iteration loop.

### Live debugging: the driver now loads, and two new facts

Getting from `EGL driver loading failed` to `Mali: display HAL loading failed`
took two findings that are worth keeping:

1. **A staged `libdl_android.so` shadows the process copy.** The namespace error
   was `dlopen failed: library "ld-android.so" not found: needed by
   /gpu/stock-vendor/lib64/libdl_android.so in namespace aera_mali`, because
   `libvndksupport`/`libapexsupport` need `libdl_android`, which in turn needs
   `ld-android.so`, and only libc/libm/libdl are shared with the process. The
   workaround in use is to retag `ld-android.so` from `DT_NEEDED` to `DT_DEBUG`
   in the blob-store copy. **The proper fix** is to add `libdl_android.so` to the
   namespace link list in `mali_display.cpp` (`link(ns, nullptr,
   "libc.so:libm.so:libdl.so:libdl_android.so")`) so the process copy is used and
   no patched stub is needed. Until then, do not remove the patch without also
   changing that line.
2. **The driver's remaining dependencies are all resolvable.** With
   `libdl_android` fixed, `libGLES_mali.so` loads and the error advances to
   `Mali: display HAL loading failed`, i.e. `Initialize()` — which is the
   gralloc1 path.

`hwcomposer.mt6768.so` needs 24 libraries that are not in the 60-library staging
set (the `vendor.mediatek.hardware.pq@2.1`…`@2.15` family, `libhidltransport`,
`libhwbinder`, `libxml2`, `libprocessgroup*`, `libmml`, `libpower`,
`libpqparamparser`, `libcomposer_ext`, `composer_ext@1.0`, `libdrm`, …), all of
which exist on the real vendor partition. They are 29 files / 5 MiB and were
copied into the blob store, then parked in `/gpu-rescue/hold29` again because of
the next point. The two gralloc4 impls need only `libdrm.so` on top of what is
already staged.

**Solved: the Android 12 destructor aborted under Android 16's allocator.**
Once `hwcomposer` loaded, the failure path called `~Impl()`, which `dlclose`d it,
and MTK's Android 12 code freed a pointer whose top-byte tag Android 16's bionic
rejects:

```
F/libc: Pointer tag for 0x... was truncated, see '.../tagged-pointers'
F/libc: Fatal signal 6 (SIGABRT) ... pid (recovery)
```

It was not caused by mounting `/vendor` (mounting the real partition changed
nothing; the `/vendor/etc/cust_pq.xml` warning is benign, that file is absent from
the image too). `mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL,
M_HEAP_TAGGING_LEVEL_NONE)` is enough, and the shim now does it in a constructor —
the shim is loaded before `hwcomposer` in `Initialize()`, so the setting is in
place before the vendor module allocates. A/B proven with
`gralloc1-probe --hwc --keep-tags` on the device:

```
tagging on   -> Pointer tag ... was truncated -> SIGABRT
tagging off  -> open: 0, version 0x2000001 (HWC2: yes), close: 0, dlclose: 0
```

`heap_tagging=1` in `/gpu/gralloc1.conf` restores A16's hardening to reproduce it.

### The next failure: `Mali: primary display discovery failed`

With the shim allocating (`error=0`, `size=10444800`, `stride=1088` for
1080x2400 RGBA_8888) and heap tagging off, the error advances again:

```
W/AeraGpu: Mali: primary display discovery failed
F/libc   : Fatal signal 6 (SIGABRT) ... in tid <worker> (recovery)
```

Two separate defects, both in `mali_display.cpp`:

1. `Initialize()` registers `HWC2_CALLBACK_HOTPLUG` and then checks
   `state.connected` on the very next line, and registering hotplug alone can
   never work on this composer (see the next section).
2. `~Impl()` then `dlclose`s the composer, which runs MTK's destructors and
   aborts. The earlier guess (a vendor worker thread still inside the module) was
   wrong: the whole `Initialize()` runs on recovery's own render thread
   (tid 11651 of pid 11640), the destructors run on that same thread, and the
   vendor frees a pointer Android 16's allocator rejects:

   ```
   I hwcomposer: [ASYNCBLTDEV] ~AsyncBlitDevice
   I scudo     : Scudo ERROR: misaligned pointer when deallocating address 0x...
   ```

   So both defects are in `mali_display.cpp`: not `dlclose`ing a HAL that failed
   to initialize, and not depending on a callback this vendor never sends.

**The display object exists; only its "connected" flag is false.** The composer
says so itself:

```
E hwcomposer: [HWC] displayGetConfigs: the display(0) is not connected
E hwcomposer: [HWC] displayGetConfigs: this display(3) is invalid
```

`HWC2_ERROR_BAD_DISPLAY` for id 0, "invalid" past the end of the table — so ids
0..2 are real `HWCDisplay` objects and 3 is out of range.

### Solved: the composer connects display 0 only after all three callbacks

`--lifecycle` showed that no ordinary display call (`getDisplayType`,
`getDisplayName`, `setPowerMode`, `getActiveConfig`, `getDisplayAttribute`)
connects display 0; they all answer `BAD_DISPLAY`. Reading the vendor composer
found why. `HWCMediator::deviceRegisterCallback` (0x67638) stores the three
callback pointers at `[this+0x130]` (hotplug), `[this+0x140]` (vsync) and
`[this+0x150]` (refresh), and only the *shared tail* at 0x679e0 — reached by all
three branches — checks that all of them are non-null and `[this+0x12c] == 0`,
then builds DisplayManager's 0x98-byte forwarding listener, `setListener()`s it
and calls `DisplayManager::init()` (0x85208). `init()` is what fills in the
display table (and calls the listener's `onHotPlugExt(0, connected=1)`, i.e. the
client hotplug callback, synchronously on the registering thread).

So registering hotplug alone leaves the display table empty forever, and
`getDisplayConfigs(0)` returns `BAD_DISPLAY`. Registering vsync and refresh
first, then hotplug, fixes it — measured with the probe:

```
register: vsync -> 0
register: refresh -> 0
hotplug : display=0 state=1 tid=11842
register: hotplug -> 0
register: connected_right_after=1 callbacks=1
configs : display=0 -> 0, 2 config(s)
```

`gralloc1-probe --hotplug` now registers all three (and `--only-hotplug`
reproduces the old single-callback run), and
`patches/bootable/recovery/0002-mali-register-all-hwc-callbacks.patch` does the
same in `mali_display.cpp`: two no-op vsync/refresh callbacks, registered before
hotplug, then the existing `connected` check passes.

### The whole present path works, and the composer owns the acquire fence

`gralloc1-probe --present` walks everything `Initialize()` and `Present()` do,
in order, with two shim-allocated 1080x2400 buffers and three frames:

```
attr    : 1 -> 0 value=1080        (WIDTH)
attr    : 2 -> 0 value=2400        (HEIGHT)
attr    : 3 -> 0 value=16666667    (VSYNC_PERIOD, 60 Hz)
power   : 0
layer   : create -> 0 id=2
shim    : allocate 0 -> 0 stride=1088
frame 0 : target=0 validate=0 types=0 requests=0
frame 0 : present=0 fence=29
frame 1 : waited for the previous release fence -> 1 revents=0x1
frame 1 : present=0 fence=32
frame 2 : present=0 fence=32
```

Note the attribute enum: 0 is `INVALID`, so 1..5 are WIDTH/HEIGHT/VSYNC_PERIOD/
DPI_X/DPI_Y. Asking for 0 returns `BAD_ATTRIBUTE` (-1), which is correct.

The one thing that would have been wrong is the acquire fence.
`HWCMediator::displaySetClientTarget` (0x6cd20) hands the fd straight to
`HWCBuffer::setAcquireFenceFd`, which `protectedCloseImpl`s the *previous* fd and
stores the new one — no `dup`. So the composer owns the descriptor and
`MaliDisplay::Present()` must not close it, or the composer ends up waiting on a
recycled fd number. mali_display's default is `caller` (close it), so `device.mk`
sets:

```
ro.aera.gpu.hwc_acquire_fence_ownership=composer
```

One cosmetic leftover: the probe (and therefore recovery, which never unloads the
composer) aborts at process exit with

```
FORTIFY: pthread_mutex_lock called on a destroyed mutex
```

because MTK's static destructors run at exit. Recovery does not return from a
reboot, so this only matters if the process is ever exited normally.

### Why `eglCreateImageKHR(EGL_NATIVE_BUFFER_ANDROID)` can never work here

This one cost a while and is worth writing down, because it looks like a shim bug
and is not.

`gralloc1-probe --egl` got as far as EGL 1.4 + GLES 3.2 (`Mali-G52 MC2`,
`OpenGL ES 3.2 v1.r32p1-01eac0...`) and then died inside the driver. The
backtrace put it at `libGLES_mali.so+0x85bcac`, and the faulting instruction is

```
0085bcac  str      x21, [sp, #0x70]
0085bcb0  ldr      x8, [x20]          ; x20 == 0 -> SIGSEGV at address 0
0085bcb8  ldr      x9, [x8, #0xb8]
0085bcc0  blr      x9
```

`x20` is the return value of the call at `0x85bc54`, and the PLT stub at
`0x23bc090` resolves to

```
_ZN7android8hardware8graphics6mapper4V4_07IMapper10getServiceERKNSt3__112basic_stringIcNS5_11char_traitsIcEENS5_9allocatorIcEEEEb
```

so the crash is:

```cpp
static sp<IMapper> mapper = IMapper::getService("default", false);  // 0x85b750
mapper->get(handle, callback);                                     // vtable +0xb8
```

`IMapper::getService()` goes through `hwservicemanager`, which recovery does not
run, so it returns null — and this DDK dereferences it without a check. There is
no fallback branch in the blob: `0x85bbe8`, whose body is just `handle !=
nullptr`, is the entire validation. Two consequences:

- The gralloc1 shim is not at fault. The native-buffer import path is simply
  unavailable in any process without hwservicemanager.
- The earlier experiment that set `ANativeWindowBuffer.common.magic/version`
  "fixed" `EGL_BAD_PARAMETER` only by getting past the *earlier* magic check and
  into this crash. `mali_display.cpp`'s zeroed `magic` was never the problem.

### The fix: import the dma-buf instead

`EGL_EXT_image_dma_buf_import` is in the driver's extension list and takes the
format and pitch from the caller, so the mapper HAL is never consulted.
`patches/bootable/recovery/0003-mali-import-scanout-buffers-as-dma-buf.patch`
switches `GpuRenderer::Initialize()` to it and adds
`MaliDisplay::DescribeBuffer()` for the fd and stride.

Two details that are easy to get wrong:

1. **Which fd.** MTK's `private_handle_t` carries three descriptors and the
   dma-buf is not the first:

   ```
   image 0 : fd[0]=19 -> anon_inode:gralloc_extra
   image 0 : fd[1]=17 -> /dmabuf:Gralloc
   image 0 : fd[2]=-1
   ```

   `DescribeBuffer()` returns `data[1]`.

2. **The fourcc is not the `drm_fourcc.h` spelling.** Mali keys its table on the
   V4L2-style *names*, so `'ABGR'` (0x52474241) is rejected with `EGL_BAD_MATCH`
   while `"AB24"` (0x34324241) is accepted. `gralloc1-probe --egl --formats`
   clears red and then reads the raw dma-buf back, which is the only way to tell
   RGBA from BGRA — green is symmetric under an R/B swap:

   ```
   formats : AB24   0x34324241 -> accepted fbo=0x8cd5 gl=255,0,0,255 raw=255,0,0,255  <- R,G,B,A
   formats : AR24   0x34325241 -> accepted fbo=0x8cd5 gl=255,0,0,255 raw=0,0,255,255
   formats : BG24   0x34324742 -> accepted fbo=0x8cd5 gl=255,0,0,255 raw=255,0,0,255
   formats : XB24   0x34324258 -> accepted fbo=0x8cd5 gl=255,0,0,255 raw=255,0,0,0
   formats : 'ARGB' 0x42475241 -> rejected (0x3009)
   formats : 'ABGR' 0x52474241 -> rejected (0x3009)
   formats : 'RGBA' 0x41424752 -> rejected (0x3009)
   formats : 'BGRA' 0x41524742 -> rejected (0x3009)
   ```

   `AB24` is `ABGR8888`: `A,B,G,R` in a little-endian word, i.e. `R,G,B,A` in
   ascending addresses, which is what `HAL_PIXEL_FORMAT_RGBA_8888` means to the
   composer. `AR24` would have put red in the blue channel.

`gralloc1-probe --egl` now ends with:

```
image 0 : fd[1]=17 -> /dmabuf:Gralloc
image 0 : AB24 -> ok
image 0 : framebuffer 0x8cd5 (complete)
render  : cleared red, glReadPixels -> 255 0 0 255
raw     : mmap fd=17 ok, centre pixel 255 0 0 255
```

so the GPU renders into the scanout buffer, the bytes land where the composer
expects them, and the layout is linear (a tiled or AFBC buffer would not
round-trip through a plain `mmap`). `--egl --native` still runs the old
native-buffer path, which is kept only to reproduce the crash above.

### Patching the Mali backend

The backend lives in `bootable/recovery` on apkreader's `mali-testing` branch,
which is not ours to push to, so fixes are patches in this repo:

```
patches/<project path>/NNNN-name.patch     e.g. patches/bootable/recovery/0001-*.patch
```

`build-aera.yml` applies them in the "Apply local patches" step, right after the
branch switch and the device tree copy. Every patch is `git apply --check`ed
first and the step fails immediately on a context mismatch — the failure mode to
avoid is a two-and-a-half-hour build that silently ships without the fix. A
patch-only change does trigger an image build (`patches/**` is in the workflow's
`paths`), because nothing else can apply it.

Generate them from the exact upstream file rather than by hand: a unified diff
produced by `difflib` against a byte-identical copy of the file at the pinned
commit always applies, and hand-written hunk headers usually do not.


### What is shipped

The backend is compiled in, and `gralloc.mt6768.so` is now provided by this repo
(`libaera_gralloc1`) instead of being absent:

- `ro.aera.gpu.*` point at the stock paths. `ro.aera.gpu.gralloc` names
  `gralloc.mt6768.so` — the shim — which CI publishes as a release asset and which
  is staged from the blob store like the other vendor libraries. If the shim is
  missing, `MaliDisplay::Load()` fails, `Initialize()` reports a display-HAL
  failure, and `GpuRenderer::Initialize()` returns false
  (`gpu_renderer.cpp:121-128`), after which `engine.cpp:293-309` falls back to the
  software renderer — i.e. the old behaviour, so a missing shim degrades instead
  of breaking. Do not point it at `gralloc.default.so`: that is the AOSP fbdev
  gralloc0 module and `gralloc1_open()` hands back a mismatched function table.
- `selene-gpu-launch.sh` is the best-effort `on boot` stager. The by-name nodes
  **do** exist on this device — `/dev/block/by-name/vendor -> /dev/block/dm-5`,
  `system -> dm-2`, `super -> mmcblk0p52` — and `mount -o ro
  /dev/block/by-name/vendor` succeeds, so dynamic partitions are not the obstacle
  they were once assumed to be. It still normally stages nothing, because `on
  boot` runs long before AERA has mapped `dm-5` for `vendor`. Log:
  `/tmp/aera-gpu-boot.log`.
- `selene-gpu-libraries.sh` builds `/gpu/lib64` from the stock
  system+vendor libraries (excluding `libc`/`libm`/`libdl`/`libdl_android`, which
  the namespace links to the recovery process).
- `selene-gpu-test.sh` is the **manual** probe. Run it after mounting `/vendor`:

  ```
  adb shell aera mount /vendor      # or Mount -> vendor in the UI
  adb shell sh /system/bin/selene-gpu-test.sh
  adb pull /tmp/aera-gpu-probe.txt
  ```

  It binds the mounted volumes under `/gpu`, stages `/gpu/lib64`, runs
  `linker64 --list` on the four libraries, and greps the vendor `hw/` directory
  for a gralloc1 provider. Both of those questions are now settled (everything
  resolves; no gralloc1 provider exists), so it is kept as a regression check for
  a future vendor or a future backend.
- The namespace mechanism itself is fine: `android_create_namespace` and
  `android_link_namespaces` are still exported by Android 16's
  `libdl_android.so` (`bionic/libdl/libdl_android.cpp`).

**Bottom line: the missing HAL is no longer missing.** The blocker was always one
thing — the backend wants a gralloc1 module and this vendor ships gralloc4 — and
this repo now supplies one. The driver loads, `libGLES_mali.so` resolves every
dependency, and the only thing left is whether the shim's descriptor matches what
MediaTek's allocator expects, which is a config file, not a rebuild.

There are two paths, and they are no longer mutually exclusive:

1. **The gralloc1 shim (this repo).** `libaera_gralloc1` implements the nine
   `GRALLOC1_FUNCTION_*` entries the backend calls on top of MediaTek's exported
   `mali_gralloc_buffer_allocate`/`_free`. Published by CI as
   `gralloc.mt6768.so`, which is already the path in `ro.aera.gpu.gralloc`, and
   staged from the blob store like the other vendor libraries. Two descriptor
   values (`format_type`, `layer_count`) are probed at runtime and can be pinned
   in `/gpu/gralloc1.conf` without rebuilding.
2. **apkreader adds a gralloc4 path** to `mali_display.cpp` — allocate through
   HIDL `allocator@4.0` and describe/lock through `mapper@4.0`, which is what the
   stock framework does. If he lands it, prefer it: it is the vendor's own
   contract and needs no descriptor guessing. The workflow already tracks his
   `mali-testing` branch head, so it lands with no change on this side.

What is still open is *not* the shim's existence:

- **Pointer-tag abort.** MTK's Android 12 `hwcomposer` destructor frees a tagged
  pointer that Android 16's bionic rejects, so a *failed* `Initialize()` becomes
  a SIGABRT crash loop. A successful `Initialize()` should not reach it. If it
  does, the fix is `mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL,
  M_HEAP_TAGGING_LEVEL_NONE)` and/or not `dlclose`ing a failed HAL, both in
  `mali_display.cpp`.
- **UK version skew.** Userspace is r32p1 (UK 11.31), the dieD kernel reports UK
  11.46. `uku_open` is only reached at `eglInitialize`, i.e. after
  `MaliDisplay::Initialize()` succeeds, so this has never actually been tested
  yet. It is the next unknown in line, not a known failure.
- **The 29 extra libraries.** `hwcomposer.mt6768.so` needs 29 files / 5 MiB that
  are not in the 60-library staging set. They are parked in
  `/gpu-rescue/hold29` and have to be copied back into the blob store before the
  shim can be exercised end to end.

## What is pushed / what is pending

**Everything is committed.** `265c126` shipped the shim, `3a901f1` shipped the
VINTF manifest fix, and the scoped-preload commit ships the rest.

On the branch:

| File | Change |
| --- | --- |
| `.github/workflows/build-aera.yml` | `.repo` cache steps, `GOGC=40` / `GOMEMLIMIT=11GiB` / `GOMAXPROCS=2`, `-j16` sync, `cpio` + `xz-utils`, shim build + symbol assertion, `/sbin` staging, `.rc` `setenv` assertion, manifest `@1.2` assertion |
| `device/xiaomi/selene/BoardConfig.mk` | `BUILD_BROKEN_MISSING_REQUIRED_MODULES := true` |
| `device/xiaomi/selene/aera_selene.mk` | dropped `AERA_WORKAROUND_BACKUP_BUG` / `AERA_OPTIONS_LIST_NUM` |
| `device/xiaomi/selene/device.mk` | `PRODUCT_PACKAGES += libaera_boot_trim` |
| `device/xiaomi/selene/bootctl_compat/trim_shim.cpp` | the shim |
| `device/xiaomi/selene/bootctl_compat/Android.bp` | shim module |
| `device/xiaomi/selene/recovery/root/system/etc/init/android.hardware.boot@1.2-service.rc` | scoped `setenv LD_PRELOAD /sbin/libaera_boot_trim.so` |
| `device/xiaomi/selene/recovery/root/init.recovery.mt6768.rc` | global `export LD_PRELOAD` removed; `on boot` GPU staging hook added |
| `device/xiaomi/selene/recovery/root/vendor/etc/vintf/manifest.xml` | `android.hardware.boot` 1.0 + 1.1 + 1.2 and their fqnames |
| `device/xiaomi/selene/recovery/root/system/bin/selene-gpu-launch.sh` | best-effort `on boot` stager, logs to `/tmp/aera-gpu-boot.log` |
| `device/xiaomi/selene/recovery/root/system/bin/selene-gpu-libraries.sh` | builds the `/gpu/lib64` namespace library directory |
| `device/xiaomi/selene/recovery/root/system/bin/selene-gpu-test.sh` | manual `linker64 --list` dependency probe |
| `device/xiaomi/selene/device.mk` | `ro.aera.gpu.*` properties |
| `HANDOFF.md` | this file |

The workflow also switches `bootable/recovery` to apkreader's `mali-testing`
branch, applies the four LVGL patches, and asserts that `mali_display.cpp`
exists, that `aeraui/Android.bp` lists it, and that all three GPU scripts are
staged executable in the recovery root. The patch step is verified against his
tree: `aeraui/patches/lvgl/` holds exactly the four patches the helper requires
(`0001-enable-arm64-neon`, `0002-skip-disabled-rgb565-neon`,
`0003-initialize-neon-tail-lanes`, `0004-gpu-texture-cache`), and
`prepare-mali-lvgl.py` resolves `--lvgl` to `<tree>/external/lvgl` from
`bootable/recovery/aeraui/tools/`, which is where the manifest puts it.

**Current state: run #7 is flashed and boots.** It is the baseline that proves the
Mali integration is inert rather than harmful. Run #8 (`f8c7e110`) adds the
gralloc1 shim on top and publishes it as `gralloc.mt6768.so`; what remains is
copying it and the 29 parked libraries into the blob store and iterating on
`/gpu/gralloc1.conf` on the device.

Run #7 measured, for future reference: 148 min wall, of which `Cleanup` 4.4,
`Restore source cache` 3.8, `Sync AERA sources` 6.2, and `Build` 133.1. The
`.repo` cache is doing its job — a cold sync alone used to be ~20 min.

## Run history

| Run | Result | Reached | Cause |
| --- | --- | --- | --- |
| #1 `43495414023` | failure, 25 min | `lunch` | `PRODUCT_USE_DYNAMIC_PARTITIONS` in `BoardConfig.mk` -> `cannot assign to readonly variable` |
| #2 `43501925957` | failure, 4 min | deps | `libncurses5` / `lib32ncurses5-dev` do not exist on Ubuntu 24.04; one bad name aborts the whole apt transaction |
| #3 `43503314394` | **success** | boots to recovery, UI blocked | boot-HAL prebuilt ABI break (one symbol) |
| #4 `43566176237` | **success** | decryption PIN screen (after live manifest patch) | shim fixed `dlopen`; VINTF refusal on boot@1.2 remained — manifest fix unpushed |
| #5 `43586256262` | cancelled | — | superseded before the manifest-only image was built; the scoped-preload fix went in instead |
| #6 `43629463339` | cancelled | — | Mali integration commit; cancelled on request to free the runner for OrangeFox testing |
| #7 `43635914131` | **success**, 148 min | image published | first image with the Mali backend compiled in (dormant: no gralloc1); every assertion passed |
| #8 `43774984519` | failure, 60 min | `Build` at ~37 min | runner lost communication; log blob never uploaded |
| #9 `43782632428` | failure, 60 min | `Build` at ~37 min | same |
| #10 `43788533915` | failure, 62 min | `Build` at ~36 min | same |
| #11 `43810934391` | failure, 59 min | `Build` at ~46 min | same, but with `-j3` and `GOMEMLIMIT 6GiB` |
| #12 `43822760838` | cancelled | `Sync` | superseded by the instrumented build; it carried the disk test but no witness |
| #13 `43824966283` | failure, 79 min | `Build` at ~47 min | swap 8 GB, ccache, commit-comment telemetry, disk watchdog. Witness posted twice then went silent |
| #14 `43834716401` | failure, 65 min | `Build` at ~47 min | witnesses moved inside `Build`; sampler hung on a timed-out `sudo dmesg` at t+155s |
| #15 `43835500987` | failure, 20 min | `Build` at 5 min | **runner shutdown signal**, exit 143, during the soong analysis peak |
| #16 `43851152934` | in flight | - | swap back to 30 GB, `GOMEMLIMIT 11GiB`, `-j` default, fixed sampler |

### The runner is being shut down, and the analysis peak is 14 GB (2026-11-16, final)

Run #15 is the first failure that ended normally, so it is the first one whose
log exists. It says plainly what happened:

```
10:16:56 Got signal: terminated
soong bootstrap failed with: signal: terminated
##[error]The runner has received a shutdown signal.
##[error]Process completed with exit code 143.
```

That is a shutdown, not a compile error and not a full disk. It landed five
minutes into `Build`, while soong was still analysing `Android.bp`.

The sampler recorded why the box was that fragile: `soong_build` at **14.9 GB
RSS on a 16 GB machine**, with swap climbing at ~50 MB/s to 5.7 of the 8 GB
available. `MemAvailable` is what matters, not `free`'s "used". The analysis
peak happens in the first five minutes, before any clang runs, so it is not the
compile that needs the memory.

`GOMEMLIMIT` does not help: soong does not set it (checked in
`ui/build/{soong,config}.go`), the variable does reach `soong_build`, but it is
a *soft* limit and the analysis genuinely needs ~14 GB live. There is nothing
to collect, so a lower limit only burns CPU. Do not lower it expecting a win.

**Run #7 is the only green image and the only trustworthy config.** It used
30 GB swap and `GOMEMLIMIT 11GiB`. Every failure since used less of one or the
other. The 8 GB swap experiment was a guess that the vanishing runner was a
full disk; there is no evidence for it and it starved the analysis peak. Both
are restored, `-j3` is dropped, and the only deliberate difference from #7 is
now the time budget.

The ~46 minute band is still not explained by any single mechanism: #8 (~30 GB
swap) and #14 (11 GB swap) both died there, and #7 ran 133 minutes of `Build`
on the same runner class, so no wall clock exists in general. Disk and `out/`
are now sampled every 30 s so that the disk explanation can be confirmed or
killed with numbers rather than with a swap-size guess.

### The sampler must not use subprocess timeouts

The witness in #14 posted at t+0, t+31s, t+62s, t+93s, t+124s, t+155s and then
went silent while the job lived another 46 minutes. Cause: `subprocess.run(
timeout=)` kills only its direct child, and `sudo dmesg | tr | cut` left
grandchildren holding the pipe, so the follow-up `communicate()` blocked
forever. The shell `timeout` binary now goes *inside* the command so the whole
pipeline dies, and mem/swap/load/PSI are read straight from `/proc`, which
still works while the box is thrashing.


### The runner dies ~46 minutes into `Build` (2026-11-16, revised)

The first diagnosis was memory and it was wrong. `-j3` and `GOMEMLIMIT 6GiB`
were applied and the death moved by zero (run #11: 46.3 min, inside the same
46.0-47.7 min band as the `-j$(nproc)` runs). What the numbers say now:

- `Build` dies at 46.0, 46.0, 46.3 and 47.7 minutes: a 3.5% spread. Job totals
  are 59-62 min because GitHub notices about ten minutes late, so the machine
  really dies at ~49-52 min of job time.
- Run #7 was green with a **133 minute** `Build` on the same runner class, so
  there is no job limit and no plain wall clock. Any "60 minute cap" theory,
  including an account-level one, is refuted by that run.
- The runner image did not change either: the newest `ubuntu24` release is
  `ubuntu24/20261111.370`, published Nov 11, three days *before* #7.

**One caveat, do not skip it.** The tight `Build` band only proves a wall clock
if `-j3` actually reached ninja. `mka` forwards unknown flags through
`soong_ui`, and nothing has verified that ninja honoured it. If it did not, then
`-j3` never happened and the band is consistent with a fixed *point in the
build* instead. The sampler's `load` field settles this: ~4 means the cap never
applied, ~3 means it did.

What changed between the green #7 and the first failure #8 — `bde80206..HEAD`
is 38 commits and 26 files, but only these touch the build:

| change | commit |
| --- | --- |
| `libaera_gralloc1` module, an extra `m libaera_gralloc1` **before** `mka`, `PRODUCT_PACKAGES += libaera_gralloc1` | `f8c7e110` |
| patches `0001`/`0002`/`0003` applied to `bootable/recovery` | `da146afe` |
| two props in `device.mk` (gralloc path, acquire-fence ownership) | `da146afe` |

Everything else is `HANDOFF.md`, `README.md`, `shim-headers/` and the probe.
`swap 30->8`, `-j3` and `GOMEMLIMIT` are *after* #8, so they are not the cause.

There is one explanation that needs no commit from us at all: `MANIFEST_BRANCH`
is a moving branch, so `repo sync` can change build input between two runs of
the same SHA. Keep it in mind before blaming a patch.

**Why the log is always 404.** GitHub uploads a job log only when the job ends;
a runner that dies mid-step never uploads, and an in-progress log is 404 as
well. The annotation is the only survivor. A runner that cannot write its own
log is also exactly what a full disk looks like, which is why disk was never
excluded.

**Instruments, added 2026-11-16.** Both are designed to survive the runner:

- a sampler that posts disk, `out/` size, memory, swap, load and the top three
  RSS processes as a **commit comment** every two minutes. Commit comments need
  only `contents: write` and were verified end to end (201 create, 204 delete).
  Check runs were tried first and dropped: creating one needs a GitHub App
  (`403 You must authenticate via a GitHub App`), so it cannot be verified from
  outside a run, and an unverifiable witness is worse than none.
- a watchdog that kills the build below 2 GB free, so a disk death becomes a
  normal failure with a log instead of a vanished runner.

Do not "fix" this by raising `GOMEMLIMIT` again, and do not conclude the patch
is broken when the log is missing — check
`GET /repos/<repo>/check-runs/<job-id>/annotations` first, since that is the
only thing that survives a dead runner.

## Next time you sit down

1. Trigger **one** build from `aera-16.0`. The `Build` step must print
   `shim built: ...`, `boot-HAL shim staged at: ...`, a `readelf` line with
   `_ZN7android4base4Trim...`, `boot HAL preload staged: ...` and
   `manifest declares boot@1.2: ...`. A missing line fails the run on purpose —
   read the `::error::` line.
2. Flash and confirm **without any live patching**:
   - `getprop init.svc.boot-hal-1-2` -> `running`
   - `logcat -d | grep -E "Registered android.hardware.boot@1.2|Using HIDL version 1.2"`
   - `recovery.log` ends at `I:Is encrypted, do decrypt page first`
   - the PIN screen appears and `/data` decrypts.
3. Confirm the preload regression is gone: flash any zip and check that the
   installer's `getprop`/`sh` do **not** die with
   `CANNOT LINK EXECUTABLE ... libaera_boot_trim.so not found`.
4. Exercise the GPU path:
   - `getprop ro.aera.gpu.gralloc` -> `gralloc.mt6768.so` (the shim; absent from
     the ramdisk on purpose, it is staged from the blob store).
   - `logcat -d -b all -v time -s AeraGpu | tail -1` after
     `setprop ctl.restart recovery` (~17-22 s) is the fast loop — no flash needed.
     `Mali HWC2 GPU ready` means the shim worked.
   - `cat /tmp/gralloc1.log` shows every descriptor probe attempt; pin the winner
     in `/gpu/gralloc1.conf` (`format_type`, `layer_count`, `probe`).
   - `ls -l /system/bin/selene-gpu-*.sh` -> all three present and executable.
5. If a backup fails with `EIO`, check `/proc/partitions` for `mmcblk1` before
   blaming the card — a removed card leaves a stale mount that EIOs forever
   (see "Storage" below). Internal `/sdcard` is a safe target meanwhile.


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
- **UI CPU is software rendering, and it is the frame-rate limit.** Measured on
  the device with `CLK_TCK=100`: the UI thread is **tid 399** (not the `recovery`
  main thread, which sits in `futex_wait`), it runs at 30-100 % of one core while
  animating, all 8 cores ramp to max (1.8 GHz little / 2.0 GHz big) and thermals
  stay at 31-33 °C. So it is **not** a governor or thermal problem, and do not
  "fix" it by pinning the governor. An earlier note in this file claimed ~44 % on
  the main thread and concluded "not render cost" — that sampled the wrong
  thread. See "Known limitation: frame rate" for the real numbers.
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

## 2026-11-16: why the GPU never appeared (the build compiled the wrong tree)

The shipped `/system/bin/recovery` contains **none** of
`patches/bootable/recovery/*.patch`, and that is not a guess: the binary embeds
`.gnu_debugdata`, so it can be asked which functions it has. It exports
`_ZN6aeraui11MaliDisplay4Impl7HotplugEPvmi` (pristine) but neither `Impl::Vsync`
(patch 0002) nor `MaliDisplay::DescribeBuffer` (patch 0003); none of the patches'
log strings are in it; and its `.text` is byte-identical to the image built the
day before those patches existed.

The reason is in the job log of run `43899667091`:

```
##[group]Run cd "${WORKDIR}"
root="${GITHUB_WORKSPACE}/patches"
shopt -s globstar nullglob
...
patches applied: 0
```

The build job has **no `actions/checkout` step**. It creates `${WORKDIR}`,
`repo init`/`repo sync`s there, and clones this repository only into `/tmp/dt` to
copy `device/xiaomi/selene` out of it. The patch loop pointed at
`${GITHUB_WORKSPACE}/patches`, a directory that never existed, and with
`globstar nullglob` an empty glob is a zero-iteration loop: the step printed
`patches applied: 0`, the following `git diff --stat` printed nothing, and the
job stayed green.

That explains the device exactly. Without 0002 the composer is asked for hotplug
alone, never marks display 0 connected, and `MaliDisplay::Initialize()` returns
`primary display discovery failed`; without 0001 the composer is still
`dlclose`d, so that failure turns into `~AsyncBlitDevice` -> `Scudo ERROR:
misaligned pointer` and a recovery restart every ~5 s (black screen).

### Fix (commit `b26df280`)

- `actions/checkout@v4` with `path: src`; the device tree **and** the patches
  both come from that one checkout, so the separate `/tmp/dt` clone is gone.
- The patch step refuses to run if `src/patches` is missing, materialises the
  glob into an array and fails if it is empty, compares applied count against
  file count, greps the patched tree for every marker, and after the build greps
  the produced `recovery` binary for `vsync/refresh registration` and
  `dma-buf import %u failed`.
- 0002 gained the second half of the sequence the probe measured: register
  vsync, refresh, hotplug, **then hotplug again** (DisplayManager's synthetic
  "display 0 connected" is delivered to a callback registered *after* `init()`
  has run), plus a bounded 2 s poll instead of a single test.

A new concurrency group (`aera-selene-v2`, commit `00a98c62`) was needed because
run `43936328986` -- the pre-fix build -- ignored three cancellation requests and
held the old group for over an hour.

## 2026-11-16: the probe re-proves every GPU path on the device

Rebuilt from the `gralloc1-shim` CI artifact and run in the failing recovery
(`LD_LIBRARY_PATH=/gpu/lib64:/gpu/stock-vendor/lib64:/system/lib64`):

| probe | result |
| --- | --- |
| `--hotplug --only-hotplug` (what the shipped binary does) | `connected_right_after=0`, `waited 5000 ms connected=0`, display `~0ull`, `configs: display=0 -> 2, 0 config(s)` |
| `--hotplug` (vsync + refresh + hotplug + hotplug again) | `hotplug : display=0 state=1`, `connected=1 callbacks=2 display=0`, `configs: display=0 -> 0, 2 config(s)` |
| `--egl` (dma-buf import, patch 0003) | `AB24 -> ok`, `framebuffer complete`, `render: glReadPixels -> 255 0 0 255`, `raw: mmap fd=17 centre pixel 255 0 0 255` |
| `--present` | layer created, 3 frames `present=0` with release fences 29 and 32, `fences: 0 count=0` |

Two details worth keeping:

- The fourcc matrix fails on the first buffer (`AB24/AR24/... -> 0x3003`) and
  succeeds on the next one. The first import attempt leaves the driver unhappy;
  the code that matters (a real frame) imports fine. Do not read the matrix as a
  format list.
- Mounting `/vendor` from the rescue partition (bind mount of
  `/gpu/stock-vendor`) changes nothing for the EGL import; `cust_pq.xml` is
  simply absent from that image and the warning is harmless.

Both probe runs end in `SIGABRT` at **process exit** (`~AsyncBlitDevice` via
`libutils`, `pthread_mutex_lock called on a destroyed mutex`). That is the
vendor's Android 12 destructor under Android 16's allocator, i.e. exactly what
patch 0001 exists to avoid: recovery must never unload the composer, and it never
exits on its own.

## Run history (2026-11-16)

| run | commit | outcome |
| --- | --- | --- |
| `43899667091` | `b0944852` | first image with zram + ccache; **`patches applied: 0`**; failed only on `libaera_gralloc1.so was not produced` |
| `43936328986` | `b6d4bff2` | pre-fix patch step, ignored three cancels, group moved to v2 and abandoned |
| `43944418037` | `b26df280` | queued behind the stuck run |
| `43946297647` | `00a98c62` | first build with the checkout fix, the extended 0002 and the binary marker assertion |

## Device state

- `boot_a` holds the **stock Android boot image** again, md5
  `65275652ea012c5b04f85207c4d23627` (verified on the partition after `dd`).
  `boot_stock_1116.img` is also at `/data/local/tmp/boot_stock.img`.
- The failing AERA candidate is `boot_died_aera_43936328986_src.img` locally
  (md5 `77bccad4bce17a837d3973d43f0d8481`), ready to flash back for comparison.
- The blob store survived: `/gpu-rescue/aera-gpu` (partition `mmcblk0p50`).
- `/sdcard` is empty in recovery because userdata is not mounted there; the stock
  image had to be pushed to `/data/local/tmp` instead.

## Next

1. When `43946297647` lands green, check the log for `patch markers verified in
   the working tree`, `patch marker present: vsync/refresh registration` and
   `patches applied: 3`. If any of those is missing the build now fails instead
   of shipping another unpatched image.
2. Pull only `boot.img` from the release, `python repack_died.py`, flash
   `boot_a`, verify md5, reboot to recovery.
3. Expect in logcat: `[HWC] Register vsync callback`, `Register refresh
   callback`, `hotplug : display=0 state=1`, then a real frame. Then run
   `--present` and `--egl` from the probe for the end-to-end check.

## 2026-11-16: the shim was being built 32-bit

Run `43936328986` finished its build and then failed its own assertion:

```
gralloc shim built: out/soong/.intermediates/device/xiaomi/selene/gralloc1_compat/
  libaera_gralloc1/android_arm_armv8-2a_shared/unstripped/libaera_gralloc1.so
##[error]libaera_gralloc1.so is not an AArch64 object
  Class: ELF32   Machine: ARM
```

`device/xiaomi/selene/gralloc1_compat/Android.bp` had no arch restriction, so
Soong built both ABIs and the assertion's `find ... -print -quit` picked the
32-bit one. A 32-bit shim can never be loaded by the 64-bit recovery, so this is
a real defect in the tree. Fixed in `deccfd07` with `compile_multilib: "64"`,
and the workflow now scans every `libaera_gralloc1.so` and uses the AArch64 one
instead of trusting `find` order.

That run also reported the cache state: **17520 of 17522 cacheable calls were
ccache hits (99.99%)**, because both images so far were built from
byte-identical *unpatched* sources. Once the patches actually apply, only the
touched files miss, so the compile phase should be minutes and soong analysis
(~25 min) dominates.
