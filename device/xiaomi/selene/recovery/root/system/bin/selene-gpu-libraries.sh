#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build /gpu/lib64 from the stock Android 12 system/vendor libraries.
#
# The Mali backend puts this directory on its private linker namespace search
# path, so the vendor EGL driver and HALs resolve their own matching Android 12
# dependencies instead of AERA's Android 16 ones. libc/libm/libdl/libdl_android
# are deliberately not linked: the driver namespace is linked to the recovery
# process for exactly those four.

set -e
mkdir -p /gpu/lib64

for directory in \
    /gpu/stock-system/system/lib64 \
    /gpu/stock-system/system/lib64/vndk-31 \
    /gpu/stock-system/system/lib64/vndk-sp-31 \
    /gpu/stock-vendor/lib64; do
    [ -d "$directory" ] || continue
    for library in "$directory"/*.so; do
        [ -f "$library" ] || continue
        name=${library##*/}
        case "$name" in
            libc.so|libdl.so|libm.so|libdl_android.so) continue ;;
        esac
        ln -sf "$library" "/gpu/lib64/$name"
    done
done

# libGLES_mali.so is the combined EGL/GLES driver; expose the usual sonames.
for library in libGLESv1_CM.so libGLESv2.so libEGL.so; do
    ln -sf /gpu/stock-vendor/lib64/egl/libGLES_mali.so "/gpu/lib64/$library"
done

# hw_get_module() resolves HAL modules under /vendor/lib64/hw. Point those
# names at the stock modules, which are opened inside the Mali namespace.
mkdir -p /vendor/lib64/hw
for module in gralloc.mt6768.so hwcomposer.mt6768.so; do
    if [ -f "/gpu/stock-vendor/lib64/hw/$module" ]; then
        ln -sf "/gpu/stock-vendor/lib64/hw/$module" "/vendor/lib64/hw/$module"
    fi
done
