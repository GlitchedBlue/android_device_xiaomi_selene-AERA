#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build /gpu/lib64 from the stock libraries in the blob store on `rescue`.
#
# The Mali backend puts this directory on its private linker namespace search
# path, so the vendor EGL driver and HALs resolve their own matching Android 12
# dependencies instead of AERA's Android 16 ones. Only libc/libm/libdl are
# deliberately not linked: the driver namespace is linked to the recovery
# process for exactly those three, and the driver must use the process copy of
# them or its malloc/file state would diverge from AERA's.
#
# Every link points into /gpu-rescue, which is listed in
# ro.aera.gpu.mali_permitted_paths. bionic checks the *realpath* of a library
# against the namespace's permitted paths, so a link that leaves those paths is
# refused even though the path it was opened by is allowed.

set -e
mkdir -p /gpu/lib64

BLOB_VENDOR=${AERA_GPU_VENDOR:-/gpu-rescue/aera-gpu/vendor}
BLOB_SYSTEM=${AERA_GPU_SYSTEM:-/gpu-rescue/aera-gpu/system}

for directory in \
    "$BLOB_SYSTEM/lib64" \
    "$BLOB_SYSTEM/lib64/vndk-31" \
    "$BLOB_SYSTEM/lib64/vndk-sp-31" \
    "$BLOB_VENDOR/lib64"; do
    [ -d "$directory" ] || continue
    for library in "$directory"/*.so; do
        [ -f "$library" ] || continue
        name=${library##*/}
        case "$name" in
            libc.so|libdl.so|libm.so) continue ;;
        esac
        ln -sf "$library" "/gpu/lib64/$name"
    done
done

# libGLES_mali.so is the combined EGL/GLES driver; expose the usual sonames.
for library in libGLESv1_CM.so libGLESv2.so libEGL.so; do
    ln -sf "$BLOB_VENDOR/lib64/egl/libGLES_mali.so" "/gpu/lib64/$library"
done

# hw_get_module() resolves HAL modules under <search path>/hw, and the gralloc1
# shim looks for the MediaTek allocator next to itself. Put the modules where
# both can find them.
mkdir -p /gpu/lib64/hw
for module in gralloc.mt6768.so hwcomposer.mt6768.so \
              android.hardware.graphics.allocator@4.0-impl-mediatek.so \
              android.hardware.graphics.mapper@4.0-impl-mediatek.so; do
    [ -f "$BLOB_VENDOR/lib64/hw/$module" ] || continue
    ln -sf "$BLOB_VENDOR/lib64/hw/$module" "/gpu/lib64/hw/$module"
done
