# Handoff / current status

Last updated: 2026-11-14 (recovery boots, decrypts and browses files; fixes
committed; run #7 is the first image with the Mali backend compiled in — dormant,
because the vendor has no gralloc1)

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

### Why a gralloc1 shim was rejected

Both vendor HIDL impls export their passthrough fetchers, so a shim looked
possible. It is not worth it:

- The shim would be compiled against AOSP 16 headers and link AOSP 16's
  `libhidlbase`, while the vendor impls were built against Android 13's. Calling
  `IAllocator::allocate` across an `sp<IAllocator>` boundary would put **two
  copies of the HIDL runtime in one process**. That is the same ABI class of
  failure as the `android::base::Trim` bug that broke the boot HAL, and it
  cannot be validated without burning a two-hour build per attempt.
- The driver's own dependency list proves the *vendor* side is a gralloc4 client
  (`mapper@4.0` + `libgralloctypes`). Mixing a gralloc1 shim in front of it does
  not reproduce what the vendor framework does.

### What is shipped instead

The backend is compiled in and stays dormant, which costs nothing at runtime and
means a future gralloc1 module only needs a property change:

- `ro.aera.gpu.*` point at the stock paths. `ro.aera.gpu.gralloc` deliberately
  names the **absent** `gralloc.mt6768.so`, so `MaliDisplay::Load()` fails,
  `MaliDisplay::Initialize()` reports a display-HAL failure, and
  `GpuRenderer::Initialize()` returns false (`gpu_renderer.cpp:121-128`), after
  which `engine.cpp:293-309` falls back to the software renderer. Do not point it
  at `gralloc.default.so`.
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

**Bottom line: do not expect `Mali HWC2 GPU ready` on selene with the current
backend.** The UI stays on the software renderer at ~4 fps. The blocker is one
missing HAL, not a driver problem, and there are exactly two ways it clears:

1. **apkreader adds a gralloc4 path** to `mali_display.cpp` — allocate through
   HIDL `allocator@4.0` and describe/lock through `mapper@4.0`, which is what the
   stock framework does. This is the correct fix and needs no shim. The
   workflow already tracks his `mali-testing` branch head, so it lands with no
   change on this side.
2. **A gralloc1 module appears** — it will not, on this vendor image.

A gralloc1 shim over the HIDL impls stays rejected for the reason above.

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

Pending: **the build itself.** Run #7 is the first image that contains the Mali
integration at all — every earlier image predates it.

## Run history

| Run | Result | Reached | Cause |
| --- | --- | --- | --- |
| #1 `43495414023` | failure, 25 min | `lunch` | `PRODUCT_USE_DYNAMIC_PARTITIONS` in `BoardConfig.mk` -> `cannot assign to readonly variable` |
| #2 `43501925957` | failure, 4 min | deps | `libncurses5` / `lib32ncurses5-dev` do not exist on Ubuntu 24.04; one bad name aborts the whole apt transaction |
| #3 `43503314394` | **success** | boots to recovery, UI blocked | boot-HAL prebuilt ABI break (one symbol) |
| #4 `43566176237` | **success** | decryption PIN screen (after live manifest patch) | shim fixed `dlopen`; VINTF refusal on boot@1.2 remained — manifest fix unpushed |
| #5 `43586256262` | cancelled | — | superseded before the manifest-only image was built; the scoped-preload fix went in instead |
| #6 `43629463339` | cancelled | — | Mali integration commit; cancelled on request to free the runner for OrangeFox testing |
| #7 `43635914131` | running | — | first image with the Mali backend compiled in (dormant: no gralloc1) |

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
4. Confirm the Mali integration is at least inert, not harmful:
   - `getprop ro.aera.gpu.gralloc` -> `gralloc.mt6768.so` (absent on purpose)
   - `logcat -d -b all -s AeraGpu` -> the backend reports the failed load, and
     the UI is still the software renderer (~4 fps). `Mali HWC2 GPU ready` here
     would mean a gralloc1 provider appeared out of nowhere; treat it as a
     surprise, not a win.
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
