# AOSP headers for the gralloc1 shim

`device/xiaomi/selene/gralloc1_compat/gralloc1_shim.cpp` is built two ways:

| Where | How | Purpose |
| --- | --- | --- |
| `build-aera.yml` | Soong, via `device.mk` | the canonical build, inside the AOSP 16 tree |
| `gralloc1-shim.yml` | NDK clang, ~1 min | iterate on the shim without a 2.5 h image build |

The second one has no AOSP tree, and the NDK does not ship libhardware, so the
headers the shim includes are pinned here. They are the AOSP 16 originals,
unmodified, Apache-2.0:

| File | Source |
| --- | --- |
| `hardware/hardware.h` | `platform/hardware/libhardware`, `include_all/hardware/` |
| `hardware/gralloc.h` | same |
| `hardware/gralloc1.h` | same |
| `hardware/fb.h` | same (included by `gralloc.h`) |
| `cutils/native_handle.h` | `platform/system/core`, `libcutils/include/cutils/` |
| `system/graphics.h` | `platform/system/core`, `libsystem/include/system/` |
| `system/graphics-base.h` | same (included by `graphics.h`) |
| `system/graphics-base-v1.{0,1,2}.h` | same (included by `graphics-base.h`) |
| `system/graphics-sw.h` | same (included by `graphics.h`) |

Fetched from `refs/tags/android-16.0.0_r1` with the googlesource `?format=TEXT`
endpoint, which serves base64. Note that at that tag `include/hardware/*.h` are
one-line forwarding stubs to `include_all/`; the real content is the `include_all`
copy, which is what is stored here.

These are API headers: `gralloc1.h` has been frozen since Android 8 and only
gained the `GRALLOC1_CAPABILITY_*` constants since. Refresh them only if the
shim starts using a newer symbol.
