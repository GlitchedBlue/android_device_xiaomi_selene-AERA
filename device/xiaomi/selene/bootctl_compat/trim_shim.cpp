/*
 * Copyright (C) 2026 AERA Recovery Project contributors
 * SPDX-License-Identifier: Apache-2.0
 *
 * ABI shim for the stock MediaTek boot-control HAL prebuilt.
 *
 * The recovery ramdisk ships
 *   /system/lib64/hw/android.hardware.boot@1.0-impl-1.2-mtkimpl.so
 * which was built against the Android 11 libbase. Android 13 replaced
 *
 *   std::string android::base::Trim(const std::string&)
 *
 * with a std::string_view overload, so the old symbol is gone from libbase on
 * Android 16 and the HAL cannot be dlopen()ed:
 *
 *   E HidlServiceManagement: Failed to dlopen
 *       android.hardware.boot@1.0-impl-1.2-mtkimpl.so:
 *       dlopen failed: cannot locate symbol
 *       "_ZN7android4base4TrimERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE"
 *
 * That is the ONLY symbol of the 160 the prebuilt imports which libbase on
 * Android 16 no longer exports (verified by resolving every undefined symbol
 * against the device's own library set), so re-exporting it is enough to make
 * the prebuilt load unchanged.
 *
 * The library is LD_PRELOADed into boot-hal-1-2 by its .rc override (see
 * recovery/root/system/etc/init/android.hardware.boot@1.2-service.rc) and is
 * staged in /sbin so a zip mounting over /system cannot hide it.
 * A missing LD_PRELOAD entry is not fatal to the linker, so this cannot make
 * the image less bootable than it already is: worst case the symbol stays
 * missing and the HAL keeps crash-looping exactly as before.
 *
 * Behaviour matches libbase: strip " \t\n\v\f\r" from both ends, return a copy.
 * The mangled name must stay
 *   _ZN7android4base4TrimERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE
 * or the prebuilt will not bind to it. The workflow asserts this with readelf.
 */

#include <string>

namespace android {
namespace base {

std::string Trim(const std::string& s) {
    static constexpr const char kSpace[] = " \t\n\v\f\r";

    const size_t begin = s.find_first_not_of(kSpace);
    if (begin == std::string::npos) {
        // Entirely whitespace (or empty): libbase returns an empty string.
        return std::string();
    }
    const size_t end = s.find_last_not_of(kSpace);
    return s.substr(begin, end - begin + 1);
}

}  // namespace base
}  // namespace android
