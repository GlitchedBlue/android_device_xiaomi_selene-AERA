// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
//
// Standalone probe for the AERA gralloc1 shim.
//
// mali_display.cpp only reaches gralloc1 after hwcomposer loads, and in this
// vendor stack a *failed* hwcomposer init aborts the process (Android 12
// destructor + Android 16 heap tags). That makes the allocate path expensive to
// debug: every attempt costs a recovery restart, and a restart in the middle of
// it costs a crash loop.
//
// This binary exercises the same path in isolation: dlopen the shim,
// gralloc1_open(), then the exact nine functions and usage bits mali_display.cpp
// uses, allocate one buffer, ask for its stride, release it. One adb command,
// no restart, and the shim's own log is printed at the end.
//
// It is built by .github/workflows/gralloc1-shim.yml (NDK, seconds), NOT by
// Soong -- it is a debugging tool, not part of the image.
//
// Usage on the device:
//
//   LD_LIBRARY_PATH=/gpu/lib64:/vendor/lib64:/system/lib64 \
//     /data/local/tmp/gralloc1-probe [shim.so] [width] [height]
//
//   LD_LIBRARY_PATH=/gpu/lib64:/vendor/lib64:/system/lib64 \
//     /data/local/tmp/gralloc1-probe --hwc [hwcomposer.mt6768.so]
//
//   LD_LIBRARY_PATH=/gpu/lib64:/vendor/lib64:/system/lib64 \
//     /data/local/tmp/gralloc1-probe --hotplug [hwcomposer.mt6768.so]
//
//   LD_LIBRARY_PATH=/gpu/lib64:/vendor/lib64:/system/lib64 \
//     /data/local/tmp/gralloc1-probe --lifecycle [--no-teardown]
//
//   LD_LIBRARY_PATH=/gpu/lib64:/vendor/lib64:/system/lib64 \
//     /data/local/tmp/gralloc1-probe --present [hwcomposer.so] [gralloc.so] [w] [h]
//
//   LD_LIBRARY_PATH=/gpu/lib64:/vendor/lib64:/system/lib64 \
//     /data/local/tmp/gralloc1-probe --egl [libGLES_mali.so] [gralloc.so] [w] [h]
//
// The second form is the crash-loop test: it opens and closes the vendor HWC2
// module without recovery being involved. The third registers the hotplug
// callback, waits for it, and asks the composer about displays 0..3, which is
// what settles why mali_display.cpp reports "primary display discovery failed".
// The fourth walks getDisplayType/getDisplayName/setPowerMode/getActiveConfig/
// getDisplayAttribute one at a time and re-asks getDisplayConfigs() after each,
// because this composer keeps a per-display "connected" flag that nothing seems
// to set at open time.
//
// LD_LIBRARY_PATH must contain the staged Android 12 libraries, because the
// shim dlopens the vendor allocator impl, whose own DT_NEEDED (libhidlbase,
// libion, libdrm, ...) have to resolve to the vendor copies.

#include <android/dlext.h>
#include <dlfcn.h>
#include <errno.h>
#include <malloc.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <hardware/hardware.h>
#include <hardware/hwcomposer2.h>

namespace {

// Identical to kProducerUsage / kConsumerUsage in mali_display.cpp. If these
// differ, the probe is not testing what the backend will do.
const uint64_t kProducerUsage = GRALLOC1_PRODUCER_USAGE_GPU_RENDER_TARGET |
                                GRALLOC1_PRODUCER_USAGE_CPU_WRITE_OFTEN |
                                GRALLOC1_PRODUCER_USAGE_CPU_READ_OFTEN;
const uint64_t kConsumerUsage = GRALLOC1_CONSUMER_USAGE_HWCOMPOSER |
                                GRALLOC1_CONSUMER_USAGE_CLIENT_TARGET |
                                GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN;

// The shim writes its probe attempts here (it has no liblog on purpose).
void PrintShimLog() {
  FILE* file = fopen("/tmp/gralloc1.log", "r");
  if (file == nullptr) {
    printf("\n(no /tmp/gralloc1.log -- the shim never ran)\n");
    return;
  }
  printf("\n--- /tmp/gralloc1.log ---\n");
  char line[1024];
  while (fgets(line, sizeof(line), file) != nullptr) printf("  | %s", line);
  fclose(file);
}

#define LOOKUP(type, name)                                              \
  type name = reinterpret_cast<type>(                                   \
      device->getFunction(device, GRALLOC1_FUNCTION_##name));            \
  if (name == nullptr) {                                                \
    printf("getFunction(%s) returned null\n", #name);                    \
    PrintShimLog();                                                     \
    return 1;                                                           \
  }

// --hwc: load the vendor HWC2 module and open/close it the way
// mali_display.cpp does at the end of Initialize() and in ~Impl(). That is the
// sequence that aborted recovery (an Android 12 destructor freeing a pointer
// whose top-byte tag Android 16's bionic rejects), so running it in a throwaway
// process says whether the crash loop is still waiting before recovery has to
// find out the hard way.
int RunHwcProbe(const char* path) {
  printf("hwc     : %s\n", path);
  void* library = dlopen(path, RTLD_NOW);
  if (library == nullptr) {
    printf("dlopen  : %s\n", dlerror());
    return 1;
  }
  auto* module = static_cast<hw_module_t*>(dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  if (module == nullptr) {
    printf("dlsym   : %s not found: %s\n", HAL_MODULE_INFO_SYM_AS_STR, dlerror());
    return 1;
  }
  printf("module  : %s (by %s)\n", module->name, module->author);
  if (module->methods == nullptr || module->methods->open == nullptr) {
    printf("no methods->open\n");
    return 1;
  }
  hw_device_t* device = nullptr;
  // mali_display.cpp passes HWC_HARDWARE_COMPOSER. Spelled out so the probe
  // does not need hardware/hwcomposer.h.
  const int rc = module->methods->open(module, "composer", &device);
  printf("open    : %d device=%p\n", rc, static_cast<void*>(device));
  if (rc != 0 || device == nullptr) {
    printf("hwcomposer open failed\n");
    return 1;
  }
  // hw_device_t *is* the common part: tag/version/module/close are its own
  // members, there is no nested `common`.
  printf("version : %#x (HWC2: %s)\n", device->version,
         (device->version >> 24) == 2 ? "yes" : "NO");
  printf("close   : %d\n", device->close(device));
  printf("dlclose : %d\n", dlclose(library));
  printf("survived the destructor path\n");
  return 0;
}

// ---------------------------------------------------------------------------
// --hotplug: the question mali_display.cpp gets wrong on this vendor stack
// ---------------------------------------------------------------------------
// Initialize() registers the HWC2 hotplug callback and then immediately checks
// whether anything is connected:
//
//   register_callback(composer, HWC2_CALLBACK_HOTPLUG, &state, Hotplug);
//   if (!state.connected.load()) return Fail("primary display discovery");
//
// On selene that check fails every time, which leaves three possibilities, and
// they need different fixes:
//
//   * MTK delivers the callback synchronously from register_callback() -- then
//     the check would pass, so it is not this;
//   * MTK delivers it later, from its own worker thread -- then the fix is to
//     wait for it instead of failing on the first look;
//   * MTK never delivers it until something asks about the display (a "first
//     commit"-style composer) -- then the fix is to query the display directly.
//
// One run answers all three: register, poll for a few seconds, and meanwhile ask
// getDisplayConfigs() about display ids 0..3, which is what a client that does
// not trust hotplug would do. It then tears down exactly like ~Impl()
// (unregister, close, dlclose), because that is the other half of the crash:
// recovery aborts on a *worker* thread, i.e. while the vendor module is being
// unloaded underneath it.
//
// Since then the composer has been reverse-engineered far enough to know that
// registering HOTPLUG alone can never work: HWCMediator::deviceRegisterCallback
// builds DisplayManager's listener and runs DisplayManager::init() only after
// hotplug, vsync and refresh are all registered. So the default is now to
// register all three (and then hotplug a second time, which makes MTK resend a
// synthetic "display 0 connected"). --only-hotplug reproduces the original
// single-callback run.
volatile int g_hotplug_count = 0;
volatile int g_hotplug_connected = 0;
volatile unsigned long long g_hotplug_display = ~0ull;
volatile int g_vsync_count = 0;
volatile int g_refresh_count = 0;

void HotplugCallback(hwc2_callback_data_t, hwc2_display_t display, int32_t state) {
  __atomic_add_fetch(&g_hotplug_count, 1, __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_hotplug_display, static_cast<unsigned long long>(display),
                   __ATOMIC_SEQ_CST);
  __atomic_store_n(&g_hotplug_connected, state == HWC2_CONNECTION_CONNECTED ? 1 : 0,
                   __ATOMIC_SEQ_CST);
  printf("hotplug : display=%llu state=%d tid=%d\n",
         static_cast<unsigned long long>(display), static_cast<int>(state),
         static_cast<int>(gettid()));
}

void VsyncCallback(hwc2_callback_data_t, hwc2_display_t display, int64_t timestamp) {
  const int n = __atomic_add_fetch(&g_vsync_count, 1, __ATOMIC_SEQ_CST);
  if (n <= 3) {
    printf("vsync   : display=%llu ts=%lld tid=%d\n",
           static_cast<unsigned long long>(display),
           static_cast<long long>(timestamp), static_cast<int>(gettid()));
  }
}

void RefreshCallback(hwc2_callback_data_t, hwc2_display_t display) {
  const int n = __atomic_add_fetch(&g_refresh_count, 1, __ATOMIC_SEQ_CST);
  if (n <= 3) {
    printf("refresh : display=%llu tid=%d\n",
           static_cast<unsigned long long>(display), static_cast<int>(gettid()));
  }
}

int RunHotplugProbe(const char* path, bool teardown, bool all_three) {
  printf("hwc     : %s\n", path);
  void* library = dlopen(path, RTLD_NOW);
  if (library == nullptr) {
    printf("dlopen  : %s\n", dlerror());
    return 1;
  }
  auto* module = static_cast<hw_module_t*>(dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  if (module == nullptr || module->methods == nullptr || module->methods->open == nullptr) {
    printf("dlsym/open: %s\n", module == nullptr ? dlerror() : "no methods->open");
    return 1;
  }
  hw_device_t* base = nullptr;
  if (module->methods->open(module, "composer", &base) != 0 || base == nullptr) {
    printf("hwcomposer open failed\n");
    return 1;
  }
  auto* device = reinterpret_cast<hwc2_device_t*>(base);
  printf("version : %#x (HWC2: %s)\n", device->common.version,
         (device->common.version >> 24) == 2 ? "yes" : "NO");

  auto reg = reinterpret_cast<HWC2_PFN_REGISTER_CALLBACK>(
      device->getFunction(device, HWC2_FUNCTION_REGISTER_CALLBACK));
  auto configs = reinterpret_cast<HWC2_PFN_GET_DISPLAY_CONFIGS>(
      device->getFunction(device, HWC2_FUNCTION_GET_DISPLAY_CONFIGS));
  printf("lookup  : register_callback=%p get_display_configs=%p\n",
         reinterpret_cast<void*>(reg), reinterpret_cast<void*>(configs));
  if (reg == nullptr) {
    printf("no register_callback\n");
    return 1;
  }

  // Exactly what Initialize() does, on the caller's thread.
  //
  // One caveat, reverse-engineered out of hwcomposer.mt6768.so: registering
  // HOTPLUG alone is *not* enough. HWCMediator::deviceRegisterCallback only
  // builds the DisplayManager listener and runs DisplayManager::init() once
  // hotplug, vsync and refresh are all non-null (the check is in the shared
  // tail at 0x679e0, so the order does not matter). Until that happens the
  // display table is never filled in and display 0 stays "not connected".
  //
  // --only-hotplug keeps the old single-callback behaviour for comparison.
  int rc = 0;
  if (all_three) {
    rc = reg(device, HWC2_CALLBACK_VSYNC, nullptr,
             reinterpret_cast<hwc2_function_pointer_t>(VsyncCallback));
    printf("register: vsync -> %d\n", rc);
    rc = reg(device, HWC2_CALLBACK_REFRESH, nullptr,
             reinterpret_cast<hwc2_function_pointer_t>(RefreshCallback));
    printf("register: refresh -> %d\n", rc);
    rc = reg(device, HWC2_CALLBACK_HOTPLUG, nullptr,
             reinterpret_cast<hwc2_function_pointer_t>(HotplugCallback));
    printf("register: hotplug -> %d\n", rc);
  } else {
    rc = reg(device, HWC2_CALLBACK_HOTPLUG, nullptr,
             reinterpret_cast<hwc2_function_pointer_t>(HotplugCallback));
    printf("register: hotplug -> %d\n", rc);
  }
  printf("register: connected_right_after=%d callbacks=%d\n",
         __atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_hotplug_count, __ATOMIC_SEQ_CST));

  // DisplayManager::resentCallback() forwards a synthetic "display 0 connected"
  // to the client callback when the hotplug callback is registered a second
  // time, now that DisplayManager::init() has installed the listener. Cheap to
  // try, and it is exactly what a client that re-registers would see.
  if (all_three) {
    const int again = reg(device, HWC2_CALLBACK_HOTPLUG, nullptr,
                          reinterpret_cast<hwc2_function_pointer_t>(HotplugCallback));
    printf("register: hotplug again -> %d connected=%d callbacks=%d\n", again,
           __atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST),
           __atomic_load_n(&g_hotplug_count, __ATOMIC_SEQ_CST));
  }

  // Poll for up to five seconds, printing the first time the state changes.
  int waited_ms = 0;
  while (waited_ms < 5000 && !__atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST)) {
    usleep(100000);
    waited_ms += 100;
  }
  printf("waited  : %d ms connected=%d callbacks=%d display=%llu vsync=%d refresh=%d\n",
         waited_ms, __atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_hotplug_count, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_hotplug_display, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_vsync_count, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_refresh_count, __ATOMIC_SEQ_CST));

  // Whether the display is reachable without hotplug at all.
  if (configs != nullptr) {
    for (uint64_t id = 0; id < 4; ++id) {
      uint32_t count = 0;
      const int config_rc = configs(device, id, &count, nullptr);
      printf("configs : display=%llu -> %d, %u config(s)\n",
             static_cast<unsigned long long>(id), config_rc, count);
    }
  }

  // ~Impl(): unregister, close, dlclose. This is where recovery dies:
  //
  //   I hwcomposer: [ASYNCBLTDEV] ~AsyncBlitDevice
  //   I scudo     : Scudo ERROR: misaligned pointer when deallocating address 0x...
  //
  // i.e. MTK's Android 12 destructor frees an interior/tagged pointer that
  // Android 16's allocator refuses. --no-teardown stops before it, so a run can
  // end cleanly when the question is only about hotplug.
  if (!teardown) {
    printf("teardown: skipped (--no-teardown); leaving the module loaded\n");
    return 0;
  }
  if (all_three) {
    reg(device, HWC2_CALLBACK_VSYNC, nullptr, nullptr);
    reg(device, HWC2_CALLBACK_REFRESH, nullptr, nullptr);
  }
  printf("unreg   : %d\n", reg(device, HWC2_CALLBACK_HOTPLUG, nullptr, nullptr));
  printf("close   : %d\n", device->common.close(&device->common));
  printf("dlclose : %d\n", dlclose(library));
  printf("survived the teardown\n");
  return 0;
}

// ---------------------------------------------------------------------------
// --lifecycle: which call makes MTK's composer admit display 0 exists
// ---------------------------------------------------------------------------
// --hotplug showed that this composer never delivers a hotplug callback for the
// primary display, so mali_display.cpp's "connected right after registration"
// check can never pass. But the display object *does* exist -- the composer
// answers with
//
//   E hwcomposer: [HWC] displayGetConfigs: the display(0) is not connected
//
// i.e. HWC2_ERROR_BAD_DISPLAY for id 0 and "this display(3) is invalid" for ids
// past the end. So something has to flip that per-display "connected" flag, and
// SurfaceFlinger does call setPowerMode()/getActiveConfig()/getDisplayType()
// around the same point in its own init. This mode calls them one at a time and
// re-asks getDisplayConfigs() after each, so the answer is which call connects
// the display -- if any of them does.
int RunLifecycleProbe(const char* path, bool teardown) {
  printf("hwc     : %s\n", path);
  void* library = dlopen(path, RTLD_NOW);
  if (library == nullptr) {
    printf("dlopen  : %s\n", dlerror());
    return 1;
  }
  auto* module = static_cast<hw_module_t*>(dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  if (module == nullptr || module->methods == nullptr || module->methods->open == nullptr) {
    printf("dlsym/open: %s\n", module == nullptr ? dlerror() : "no methods->open");
    return 1;
  }
  hw_device_t* base = nullptr;
  if (module->methods->open(module, "composer", &base) != 0 || base == nullptr) {
    printf("hwcomposer open failed\n");
    return 1;
  }
  auto* device = reinterpret_cast<hwc2_device_t*>(base);

  auto configs = reinterpret_cast<HWC2_PFN_GET_DISPLAY_CONFIGS>(
      device->getFunction(device, HWC2_FUNCTION_GET_DISPLAY_CONFIGS));
  auto type = reinterpret_cast<HWC2_PFN_GET_DISPLAY_TYPE>(
      device->getFunction(device, HWC2_FUNCTION_GET_DISPLAY_TYPE));
  auto power = reinterpret_cast<HWC2_PFN_SET_POWER_MODE>(
      device->getFunction(device, HWC2_FUNCTION_SET_POWER_MODE));
  auto active = reinterpret_cast<HWC2_PFN_GET_ACTIVE_CONFIG>(
      device->getFunction(device, HWC2_FUNCTION_GET_ACTIVE_CONFIG));
  auto attribute = reinterpret_cast<HWC2_PFN_GET_DISPLAY_ATTRIBUTE>(
      device->getFunction(device, HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE));
  auto name = reinterpret_cast<HWC2_PFN_GET_DISPLAY_NAME>(
      device->getFunction(device, HWC2_FUNCTION_GET_DISPLAY_NAME));
  printf("lookup  : configs=%p type=%p power=%p active=%p attribute=%p name=%p\n",
         reinterpret_cast<void*>(configs), reinterpret_cast<void*>(type),
         reinterpret_cast<void*>(power), reinterpret_cast<void*>(active),
         reinterpret_cast<void*>(attribute), reinterpret_cast<void*>(name));
  if (configs == nullptr) {
    printf("no getDisplayConfigs\n");
    return 1;
  }

  auto report = [&](const char* step) {
    uint32_t count = 0;
    const int rc = configs(device, 0, &count, nullptr);
    printf("  %-26s getDisplayConfigs(0) -> %d (%s), %u config(s)\n", step, rc,
           rc == 0 ? "OK" : rc == 2 ? "BAD_DISPLAY" : "other", count);
  };

  printf("display 0:\n");
  report("as opened");
  if (type) {
    int32_t value = -1;
    const int rc = type(device, 0, &value);
    printf("  %-26s -> %d, type=%d (0=physical)\n", "getDisplayType(0)", rc, value);
  }
  if (name) {
    uint32_t size = 0;
    const int rc = name(device, 0, &size, nullptr);
    printf("  %-26s -> %d, name size=%u\n", "getDisplayName(0)", rc, size);
  }
  if (power) {
    const int rc = power(device, 0, HWC2_POWER_MODE_ON);
    printf("  %-26s -> %d\n", "setPowerMode(0, ON)", rc);
  }
  report("after power on");
  if (active) {
    uint32_t config = 0;
    const int rc = active(device, 0, &config);
    printf("  %-26s -> %d, config=%u\n", "getActiveConfig(0)", rc, config);
  }
  report("after getActiveConfig");
  if (attribute) {
    // 0 = WIDTH, 1 = HEIGHT, 2 = VSYNC_PERIOD, 3 = DPI_X, 4 = DPI_Y
    for (int32_t which = 0; which <= 4; ++which) {
      int32_t value = -1;
      const int rc = attribute(device, 0, 0, which, &value);
      printf("  getDisplayAttribute(0,0,%d)    -> %d, value=%d\n", which, rc, value);
    }
  }
  report("after attributes");
  if (power) {
    const int rc = power(device, 0, HWC2_POWER_MODE_OFF);
    printf("  %-26s -> %d\n", "setPowerMode(0, OFF)", rc);
  }
  report("after power off");

  if (!teardown) {
    printf("teardown: skipped (--no-teardown)\n");
    return 0;
  }
  printf("close   : %d\n", device->common.close(&device->common));
  printf("dlclose : %d\n", dlclose(library));
  printf("survived the teardown\n");
  return 0;
}

// ---------------------------------------------------------------------------
// --present: the rest of Initialize() and one Present()
// ---------------------------------------------------------------------------
// --hotplug settled display discovery. This walks what mali_display.cpp does
// next, in the same order: setPowerMode(ON), createLayer, the layer properties,
// setClientTarget with a real shim-allocated buffer, validateDisplay,
// acceptDisplayChanges, presentDisplay, getReleaseFences. Every one of those can
// fail on its own, and each failure otherwise costs a two-and-a-half-hour image
// build to discover.
bool AllocateScanout(const char* shim_path, uint32_t width, uint32_t height,
                     buffer_handle_t* out, int count, gralloc1_device_t** out_device,
                     uint32_t* out_strides = nullptr) {
  void* library = dlopen(shim_path, RTLD_NOW);
  if (library == nullptr) {
    printf("shim    : dlopen failed: %s\n", dlerror());
    return false;
  }
  auto* module = static_cast<hw_module_t*>(dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  if (module == nullptr) {
    printf("shim    : %s not found: %s\n", HAL_MODULE_INFO_SYM_AS_STR, dlerror());
    return false;
  }
  gralloc1_device_t* device = nullptr;
  if (gralloc1_open(module, &device) != 0 || device == nullptr || !device->getFunction) {
    printf("shim    : gralloc1_open failed\n");
    return false;
  }
  // The file-scope LOOKUP returns 1 on failure, which is the opposite of this
  // function's convention, so it gets its own.
#define SHIM(type, name)                                                  \
  auto name = reinterpret_cast<type>(                                     \
      device->getFunction(device, GRALLOC1_FUNCTION_##name));              \
  if (name == nullptr) {                                                  \
    printf("shim    : getFunction(%s) returned null\n", #name);            \
    return false;                                                         \
  }
  SHIM(GRALLOC1_PFN_CREATE_DESCRIPTOR, CREATE_DESCRIPTOR);
  SHIM(GRALLOC1_PFN_DESTROY_DESCRIPTOR, DESTROY_DESCRIPTOR);
  SHIM(GRALLOC1_PFN_SET_DIMENSIONS, SET_DIMENSIONS);
  SHIM(GRALLOC1_PFN_SET_FORMAT, SET_FORMAT);
  SHIM(GRALLOC1_PFN_SET_PRODUCER_USAGE, SET_PRODUCER_USAGE);
  SHIM(GRALLOC1_PFN_SET_CONSUMER_USAGE, SET_CONSUMER_USAGE);
  SHIM(GRALLOC1_PFN_ALLOCATE, ALLOCATE);
  SHIM(GRALLOC1_PFN_GET_STRIDE, GET_STRIDE);
#undef SHIM

  gralloc1_buffer_descriptor_t descriptor = 0;
  if (CREATE_DESCRIPTOR(device, &descriptor) != GRALLOC1_ERROR_NONE) {
    printf("shim    : create descriptor failed\n");
    return false;
  }
  const bool configured =
      SET_DIMENSIONS(device, descriptor, width, height) == 0 &&
      SET_FORMAT(device, descriptor, HAL_PIXEL_FORMAT_RGBA_8888) == 0 &&
      SET_PRODUCER_USAGE(device, descriptor, kProducerUsage) == 0 &&
      SET_CONSUMER_USAGE(device, descriptor, kConsumerUsage) == 0;
  if (!configured) {
    printf("shim    : descriptor configuration failed\n");
    DESTROY_DESCRIPTOR(device, descriptor);
    return false;
  }
  bool ok = true;
  for (int i = 0; i < count; ++i) {
    out[i] = nullptr;
    const int rc = ALLOCATE(device, 1, &descriptor, &out[i]);
    uint32_t stride = 0;
    if (rc == GRALLOC1_ERROR_NONE && out[i] != nullptr) GET_STRIDE(device, out[i], &stride);
    if (out_strides != nullptr) out_strides[i] = stride;
    printf("shim    : allocate %d -> %d handle=%p stride=%u\n", i, rc,
           static_cast<const void*>(out[i]), stride);
    ok = ok && rc == GRALLOC1_ERROR_NONE && out[i] != nullptr;
  }
  DESTROY_DESCRIPTOR(device, descriptor);
  *out_device = device;
  return ok;
}

int RunPresentProbe(const char* hwc_path, const char* shim_path, uint32_t width,
                    uint32_t height) {
  printf("hwc     : %s\n", hwc_path);
  void* library = dlopen(hwc_path, RTLD_NOW);
  if (library == nullptr) {
    printf("dlopen  : %s\n", dlerror());
    return 1;
  }
  auto* module = static_cast<hw_module_t*>(dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  if (module == nullptr || module->methods == nullptr || module->methods->open == nullptr) {
    printf("dlsym/open: %s\n", module == nullptr ? dlerror() : "no methods->open");
    return 1;
  }
  hw_device_t* base = nullptr;
  if (module->methods->open(module, "composer", &base) != 0 || base == nullptr) {
    printf("hwcomposer open failed\n");
    return 1;
  }
  auto* device = reinterpret_cast<hwc2_device_t*>(base);

#define COMPOSER(type, name)                                                   \
  auto name = reinterpret_cast<type>(                                          \
      device->getFunction(device, HWC2_FUNCTION_##name));                       \
  if (name == nullptr) {                                                       \
    printf("no HWC2 function %s\n", #name);                                     \
    return 1;                                                                  \
  }
  COMPOSER(HWC2_PFN_REGISTER_CALLBACK, REGISTER_CALLBACK);
  COMPOSER(HWC2_PFN_SET_POWER_MODE, SET_POWER_MODE);
  COMPOSER(HWC2_PFN_CREATE_LAYER, CREATE_LAYER);
  COMPOSER(HWC2_PFN_SET_LAYER_COMPOSITION_TYPE, SET_LAYER_COMPOSITION_TYPE);
  COMPOSER(HWC2_PFN_SET_LAYER_DISPLAY_FRAME, SET_LAYER_DISPLAY_FRAME);
  COMPOSER(HWC2_PFN_SET_LAYER_SOURCE_CROP, SET_LAYER_SOURCE_CROP);
  COMPOSER(HWC2_PFN_SET_LAYER_BLEND_MODE, SET_LAYER_BLEND_MODE);
  COMPOSER(HWC2_PFN_SET_LAYER_PLANE_ALPHA, SET_LAYER_PLANE_ALPHA);
  COMPOSER(HWC2_PFN_SET_CLIENT_TARGET, SET_CLIENT_TARGET);
  COMPOSER(HWC2_PFN_VALIDATE_DISPLAY, VALIDATE_DISPLAY);
  COMPOSER(HWC2_PFN_ACCEPT_DISPLAY_CHANGES, ACCEPT_DISPLAY_CHANGES);
  COMPOSER(HWC2_PFN_PRESENT_DISPLAY, PRESENT_DISPLAY);
  COMPOSER(HWC2_PFN_GET_RELEASE_FENCES, GET_RELEASE_FENCES);
  COMPOSER(HWC2_PFN_GET_DISPLAY_CONFIGS, GET_DISPLAY_CONFIGS);
  COMPOSER(HWC2_PFN_GET_ACTIVE_CONFIG, GET_ACTIVE_CONFIG);
  COMPOSER(HWC2_PFN_GET_DISPLAY_ATTRIBUTE, GET_DISPLAY_ATTRIBUTE);
#undef COMPOSER

  if (REGISTER_CALLBACK(device, HWC2_CALLBACK_VSYNC, nullptr,
                        reinterpret_cast<hwc2_function_pointer_t>(VsyncCallback)) ||
      REGISTER_CALLBACK(device, HWC2_CALLBACK_REFRESH, nullptr,
                        reinterpret_cast<hwc2_function_pointer_t>(RefreshCallback)) ||
      REGISTER_CALLBACK(device, HWC2_CALLBACK_HOTPLUG, nullptr,
                        reinterpret_cast<hwc2_function_pointer_t>(HotplugCallback))) {
    printf("callback registration failed\n");
    return 1;
  }
  if (!__atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST)) {
    printf("display : not connected after registration\n");
    return 1;
  }
  const hwc2_display_t display =
      __atomic_load_n(&g_hotplug_display, __ATOMIC_SEQ_CST);

  uint32_t config_count = 0;
  const int config_rc = GET_DISPLAY_CONFIGS(device, display, &config_count, nullptr);
  hwc2_config_t active = 0;
  const int active_rc = GET_ACTIVE_CONFIG(device, display, &active);
  printf("config  : getDisplayConfigs -> %d (%u), getActiveConfig -> %d (%u)\n",
         config_rc, config_count, active_rc, active);
  for (int32_t which = 0; which <= 4; ++which) {
    int32_t value = -1;
    const int rc = GET_DISPLAY_ATTRIBUTE(device, display, active, which, &value);
    printf("attr    : %d -> %d value=%d\n", which, rc, value);
  }

  printf("power   : %d\n", SET_POWER_MODE(device, display, HWC2_POWER_MODE_ON));
  hwc2_layer_t layer = 0;
  const int layer_rc = CREATE_LAYER(device, display, &layer);
  printf("layer   : create -> %d id=%llu\n", layer_rc,
         static_cast<unsigned long long>(layer));
  if (layer_rc) return 1;

  const hwc_rect_t frame{0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  const hwc_frect_t crop{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
  printf("layer   : composition -> %d\n",
         SET_LAYER_COMPOSITION_TYPE(device, display, layer, HWC2_COMPOSITION_CLIENT));
  printf("layer   : frame -> %d\n", SET_LAYER_DISPLAY_FRAME(device, display, layer, frame));
  printf("layer   : crop -> %d\n", SET_LAYER_SOURCE_CROP(device, display, layer, crop));
  printf("layer   : blend -> %d\n",
         SET_LAYER_BLEND_MODE(device, display, layer, HWC2_BLEND_MODE_NONE));
  printf("layer   : alpha -> %d\n",
         SET_LAYER_PLANE_ALPHA(device, display, layer, 1.0f));

  // Two buffers and three frames, which is the shape the renderer actually
  // uses: present, wait for the release fence, present the other buffer, ...
  // A composer that only survives the first frame would still look fine in a
  // one-shot test.
  buffer_handle_t buffers[2] = {nullptr, nullptr};
  gralloc1_device_t* allocator = nullptr;
  if (!AllocateScanout(shim_path, width, height, buffers, 2, &allocator)) return 1;

  const hwc_region_t damage{1, &frame};
  int previous_fence = -1;
  int failures = 0;
  for (int i = 0; i < 3; ++i) {
    if (previous_fence >= 0) {
      pollfd pending{previous_fence, POLLIN, 0};
      int polled;
      do { polled = poll(&pending, 1, 1000); } while (polled < 0 && errno == EINTR);
      printf("frame %d : waited for the previous release fence -> %d revents=%#x\n",
             i, polled, pending.revents);
      close(previous_fence);
      previous_fence = -1;
    }
    const int target_rc = SET_CLIENT_TARGET(device, display, buffers[i % 2], -1, 0, damage);
    uint32_t types = 0, requests = 0;
    int result = VALIDATE_DISPLAY(device, display, &types, &requests);
    printf("frame %d : target=%d validate=%d types=%u requests=%u\n", i, target_rc, result,
           types, requests);
    if (result == HWC2_ERROR_HAS_CHANGES) {
      result = ACCEPT_DISPLAY_CHANGES(device, display);
      printf("frame %d : accept=%d\n", i, result);
    }
    int fence = -1;
    const int present_rc = PRESENT_DISPLAY(device, display, &fence);
    printf("frame %d : present=%d fence=%d\n", i, present_rc, fence);
    if (target_rc || result || present_rc) ++failures;
    previous_fence = fence;
  }
  if (previous_fence >= 0) close(previous_fence);

  uint32_t fence_count = 0;
  const int fences_rc = GET_RELEASE_FENCES(device, display, &fence_count, nullptr, nullptr);
  printf("fences  : %d count=%u\n", fences_rc, fence_count);
  if (fences_rc == 0 && fence_count > 0 && fence_count <= 16) {
    hwc2_layer_t layers[16] = {};
    int32_t fds[16] = {};
    const int rc = GET_RELEASE_FENCES(device, display, &fence_count, layers, fds);
    printf("fences  : second call -> %d\n", rc);
    for (uint32_t i = 0; i < fence_count; ++i) {
      printf("          layer=%llu fd=%d\n", static_cast<unsigned long long>(layers[i]),
             fds[i]);
      if (fds[i] >= 0) close(fds[i]);
    }
  }

  printf("teardown: skipped (deliberately; the composer is never unloaded)\n");
  return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --egl: the EGL half of GpuRenderer::Initialize()
// ---------------------------------------------------------------------------
// Everything between "Mali: primary display discovery failed" and
// "Mali HWC2 GPU ready" is EGL work, and GpuRenderer::Initialize() logs nothing
// when any of it fails: it just returns false and AERA silently falls back to the
// software renderer. Reproducing it here makes those failures visible in 90
// seconds instead of one image build each.
//
// The sequence is the tree's, step for step: libdl_android namespace, driver
// dlopen inside it, pbuffer context, then the shim's scanout buffer imported as
// an EGLImage and attached to a framebuffer. The final glReadPixels is the part
// that proves Mali really rendered into the shim allocation rather than
// somewhere else.
struct ProbeNativeBase {
  int magic;
  int version;
  void* reserved[4];
  void (*incRef)(ProbeNativeBase*);
  void (*decRef)(ProbeNativeBase*);
};

// AOSP's ANativeWindowBuffer (system/window.h). mali_display.cpp fills in the
// same struct from nativebase/nativebase.h, and mali_gralloc reads width, height,
// stride, format and handle out of it, so the layout has to match exactly. The
// size is printed at startup: 168 on arm64.
struct ProbeWindowBuffer {
  ProbeNativeBase common;
  int width;
  int height;
  int stride;
  int format;
  int usage_deprecated;
  uintptr_t layerCount;
  void* reserved[1];
  const native_handle_t* handle;
  uint64_t usage;
  void* reserved2[8];
};

void ProbeReference(ProbeNativeBase*) {}

// Same bits as kNativeUsage in mali_display.cpp.
constexpr uint64_t kProbeNativeUsage = GRALLOC_USAGE_HW_RENDER |
                                       GRALLOC_USAGE_HW_COMPOSER |
                                       GRALLOC_USAGE_HW_FB |
                                       GRALLOC_USAGE_SW_READ_OFTEN |
                                       GRALLOC_USAGE_SW_WRITE_OFTEN;

bool HasExtension(const char* list, const char* name) {
  if (list == nullptr || name == nullptr) return false;
  const size_t length = strlen(name);
  for (const char* at = list; (at = strstr(at, name)) != nullptr; at += length) {
    const bool starts = at == list || at[-1] == ' ';
    const bool ends = at[length] == ' ' || at[length] == '\0';
    if (starts && ends) return true;
  }
  return false;
}

template <typename T>
bool LoadEglSymbol(void* library, const char* name, T* out) {
  *out = reinterpret_cast<T>(dlsym(library, name));
  if (*out == nullptr) printf("egl     : no %s\n", name);
  return *out != nullptr;
}

template <typename T>
bool LoadGlSymbol(void* library, PFNEGLGETPROCADDRESSPROC get_proc_address,
                  const char* name, T* out) {
  *out = reinterpret_cast<T>(get_proc_address(name));
  if (*out == nullptr) *out = reinterpret_cast<T>(dlsym(library, name));
  if (*out == nullptr) printf("gles    : no %s\n", name);
  return *out != nullptr;
}

int RunEglProbe(const char* driver_path, const char* shim_path, uint32_t width,
                uint32_t height) {
  printf("egl     : sizeof(ProbeWindowBuffer)=%zu (arm64 expects 176)\n",
         sizeof(ProbeWindowBuffer));
  printf("egl     : driver %s\n", driver_path);

  // 1. The driver lives outside the default namespace, exactly as in
  //    MaliDisplay::OpenDriver(): libdl_android creates a namespace whose search
  //    path is the staged /gpu/lib64 and which may reach /gpu and /vendor.
  //
  //    The path is absolute on purpose. This probe is normally started with
  //    LD_LIBRARY_PATH=/gpu/lib64 (the shim's vendor allocator needs it), and
  //    /gpu/lib64/libdl_android.so is the staged Android 12 copy: loading that
  //    into an Android 16 process segfaults, which is exactly what the first
  //    --egl run did. Recovery does not set LD_LIBRARY_PATH, so it gets the
  //    process copy from /system/lib64 -- and so must the probe.
  void* linker = dlopen("/system/lib64/libdl_android.so", RTLD_NOW);
  if (linker == nullptr) {
    printf("linker  : dlopen failed: %s\n", dlerror());
    return 1;
  }
  printf("linker  : /system/lib64/libdl_android.so -> %p\n", linker);
  using CreateNamespace = android_namespace_t* (*)(const char*, const char*,
      const char*, uint64_t, const char*, android_namespace_t*);
  using LinkNamespace = bool (*)(android_namespace_t*, android_namespace_t*,
                                 const char*);
  auto create_namespace =
      reinterpret_cast<CreateNamespace>(dlsym(linker, "android_create_namespace"));
  auto link_namespaces =
      reinterpret_cast<LinkNamespace>(dlsym(linker, "android_link_namespaces"));
  printf("linker  : create_namespace=%p link_namespaces=%p\n",
         reinterpret_cast<void*>(create_namespace),
         reinterpret_cast<void*>(link_namespaces));
  if (create_namespace == nullptr || link_namespaces == nullptr) {
    printf("linker  : namespace API missing: %s\n", dlerror());
    return 1;
  }
  android_namespace_t* driver_namespace = create_namespace(
      "aera_mali", "/gpu/lib64", "/gpu/lib64", 1, "/gpu:/vendor", nullptr);
  printf("linker  : namespace -> %p\n", static_cast<void*>(driver_namespace));
  if (driver_namespace == nullptr) {
    printf("linker  : android_create_namespace failed\n");
    return 1;
  }
  const bool linked =
      link_namespaces(driver_namespace, nullptr, "libc.so:libm.so:libdl.so");
  printf("linker  : link_namespaces -> %s\n", linked ? "true" : "false");
  if (!linked) {
    printf("linker  : android_link_namespaces failed\n");
    return 1;
  }
  android_dlextinfo info{};
  info.flags = ANDROID_DLEXT_USE_NAMESPACE;
  info.library_namespace = driver_namespace;
  printf("driver  : dlopen in the namespace...\n");
  void* driver = android_dlopen_ext(driver_path, RTLD_NOW, &info);
  printf("driver  : dlopen -> %p%s%s\n", driver, driver ? "" : " ",
         driver ? "" : dlerror());
  if (driver == nullptr) return 1;

  // 2. EGL bootstrap.
  PFNEGLGETDISPLAYPROC get_display = nullptr;
  PFNEGLINITIALIZEPROC initialize = nullptr;
  PFNEGLGETPROCADDRESSPROC get_proc_address = nullptr;
  PFNEGLTERMINATEPROC terminate = nullptr;
  PFNEGLGETERRORPROC get_error = nullptr;
  if (!LoadEglSymbol(driver, "eglGetDisplay", &get_display) ||
      !LoadEglSymbol(driver, "eglInitialize", &initialize) ||
      !LoadEglSymbol(driver, "eglGetProcAddress", &get_proc_address) ||
      !LoadEglSymbol(driver, "eglTerminate", &terminate) ||
      !LoadEglSymbol(driver, "eglGetError", &get_error)) {
    printf("egl     : bootstrap incomplete\n");
    return 1;
  }
  EGLDisplay display = get_display(EGL_DEFAULT_DISPLAY);
  EGLint major = 0;
  EGLint minor = 0;
  if (display == EGL_NO_DISPLAY || !initialize(display, &major, &minor)) {
    printf("egl     : eglInitialize failed: %#x\n", get_error());
    return 1;
  }
  printf("egl     : initialized %d.%d\n", major, minor);

  PFNEGLQUERYSTRINGPROC query_string = nullptr;
  PFNEGLCHOOSECONFIGPROC choose_config = nullptr;
  PFNEGLCREATEPBUFFERSURFACEPROC create_pbuffer = nullptr;
  PFNEGLCREATECONTEXTPROC create_context = nullptr;
  PFNEGLMAKECURRENTPROC make_current = nullptr;
  PFNEGLBINDAPIPROC bind_api = nullptr;
  PFNEGLCREATEIMAGEKHRPROC create_image = nullptr;
  PFNEGLDESTROYIMAGEKHRPROC destroy_image = nullptr;
  if (!LoadEglSymbol(driver, "eglQueryString", &query_string) ||
      !LoadEglSymbol(driver, "eglChooseConfig", &choose_config) ||
      !LoadEglSymbol(driver, "eglCreatePbufferSurface", &create_pbuffer) ||
      !LoadEglSymbol(driver, "eglCreateContext", &create_context) ||
      !LoadEglSymbol(driver, "eglMakeCurrent", &make_current) ||
      !LoadEglSymbol(driver, "eglBindAPI", &bind_api) ||
      !LoadEglSymbol(driver, "eglCreateImageKHR", &create_image) ||
      !LoadEglSymbol(driver, "eglDestroyImageKHR", &destroy_image)) {
    printf("egl     : entry points incomplete\n");
    return 1;
  }

  const char* egl_extensions = query_string(display, EGL_EXTENSIONS);
  printf("egl     : extensions %s\n",
         egl_extensions == nullptr ? "(null)" : egl_extensions);
  printf("egl     : EGL_ANDROID_image_native_buffer -> %s\n",
         HasExtension(egl_extensions, "EGL_ANDROID_image_native_buffer") ? "yes"
                                                                        : "NO");
  if (!bind_api(EGL_OPENGL_ES_API)) {
    printf("egl     : eglBindAPI failed: %#x\n", get_error());
    return 1;
  }

  const EGLint config_attributes[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8,
      EGL_GREEN_SIZE, 8,
      EGL_BLUE_SIZE, 8,
      EGL_ALPHA_SIZE, 8,
      EGL_NONE,
  };
  EGLConfig config = nullptr;
  EGLint config_count = 0;
  if (!choose_config(display, config_attributes, &config, 1, &config_count) ||
      config_count != 1) {
    printf("egl     : no pbuffer config: %#x\n", get_error());
    return 1;
  }
  const EGLint surface_attributes[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
  const EGLint context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  EGLSurface surface = create_pbuffer(display, config, surface_attributes);
  EGLContext context =
      create_context(display, config, EGL_NO_CONTEXT, context_attributes);
  if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT ||
      !make_current(display, surface, surface, context)) {
    printf("egl     : context creation failed: %#x\n", get_error());
    return 1;
  }

  // 3. GLES entry points, through eglGetProcAddress like glad does.
  PFNGLGETSTRINGPROC gl_get_string = nullptr;
  PFNGLGETERRORPROC gl_get_error = nullptr;
  PFNGLGENTEXTURESPROC gl_gen_textures = nullptr;
  PFNGLBINDTEXTUREPROC gl_bind_texture = nullptr;
  PFNGLTEXPARAMETERIPROC gl_tex_parameteri = nullptr;
  PFNGLGENFRAMEBUFFERSPROC gl_gen_framebuffers = nullptr;
  PFNGLBINDFRAMEBUFFERPROC gl_bind_framebuffer = nullptr;
  PFNGLFRAMEBUFFERTEXTURE2DPROC gl_framebuffer_texture2d = nullptr;
  PFNGLCHECKFRAMEBUFFERSTATUSPROC gl_check_framebuffer_status = nullptr;
  PFNGLVIEWPORTPROC gl_viewport = nullptr;
  PFNGLCLEARCOLORPROC gl_clear_color = nullptr;
  PFNGLCLEARPROC gl_clear = nullptr;
  PFNGLFINISHPROC gl_finish = nullptr;
  PFNGLREADPIXELSPROC gl_read_pixels = nullptr;
  auto image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      get_proc_address("glEGLImageTargetTexture2DOES"));
  if (image_target == nullptr)
    image_target = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        dlsym(driver, "glEGLImageTargetTexture2DOES"));
  if (!LoadGlSymbol(driver, get_proc_address, "glGetString", &gl_get_string) ||
      !LoadGlSymbol(driver, get_proc_address, "glGetError", &gl_get_error) ||
      !LoadGlSymbol(driver, get_proc_address, "glGenTextures", &gl_gen_textures) ||
      !LoadGlSymbol(driver, get_proc_address, "glBindTexture", &gl_bind_texture) ||
      !LoadGlSymbol(driver, get_proc_address, "glTexParameteri", &gl_tex_parameteri) ||
      !LoadGlSymbol(driver, get_proc_address, "glGenFramebuffers", &gl_gen_framebuffers) ||
      !LoadGlSymbol(driver, get_proc_address, "glBindFramebuffer", &gl_bind_framebuffer) ||
      !LoadGlSymbol(driver, get_proc_address, "glFramebufferTexture2D",
                    &gl_framebuffer_texture2d) ||
      !LoadGlSymbol(driver, get_proc_address, "glCheckFramebufferStatus",
                    &gl_check_framebuffer_status) ||
      !LoadGlSymbol(driver, get_proc_address, "glViewport", &gl_viewport) ||
      !LoadGlSymbol(driver, get_proc_address, "glClearColor", &gl_clear_color) ||
      !LoadGlSymbol(driver, get_proc_address, "glClear", &gl_clear) ||
      !LoadGlSymbol(driver, get_proc_address, "glFinish", &gl_finish) ||
      !LoadGlSymbol(driver, get_proc_address, "glReadPixels", &gl_read_pixels)) {
    printf("gles    : entry points incomplete\n");
    return 1;
  }

  const char* gl_extensions =
      reinterpret_cast<const char*>(gl_get_string(GL_EXTENSIONS));
  printf("gles    : renderer %s\n",
         reinterpret_cast<const char*>(gl_get_string(GL_RENDERER)));
  printf("gles    : version %s\n",
         reinterpret_cast<const char*>(gl_get_string(GL_VERSION)));
  printf("gles    : GL_OES_EGL_image -> %s\n",
         HasExtension(gl_extensions, "GL_OES_EGL_image") ? "yes" : "NO");
  if (image_target == nullptr) {
    printf("gles    : no glEGLImageTargetTexture2DOES\n");
    return 1;
  }

  // 4. Import the shim's scanout buffer and render into it.
  buffer_handle_t buffers[2] = {nullptr, nullptr};
  uint32_t strides[2] = {0, 0};
  gralloc1_device_t* allocator = nullptr;
  if (!AllocateScanout(shim_path, width, height, buffers, 2, &allocator, strides)) return 1;

  GLuint textures[2] = {};
  GLuint framebuffers[2] = {};
  EGLImageKHR images[2] = {EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
  bool complete[2] = {false, false};
  gl_gen_textures(2, textures);
  gl_gen_framebuffers(2, framebuffers);
  int failures = 0;
  for (int i = 0; i < 2; ++i) {
    ProbeWindowBuffer window{};
    // AOSP's GraphicBuffer sets these two; mali_display.cpp does not. Buffer 0
    // gets them and buffer 1 does not, so one run says whether Mali validates
    // them (EGL_BAD_PARAMETER if so).
    if (i == 0) {
      window.common.magic = 0x5f626672;  // ANDROID_NATIVE_BUFFER_MAGIC ('_bfr')
      window.common.version = sizeof(ProbeWindowBuffer);
    }
    window.common.incRef = ProbeReference;
    window.common.decRef = ProbeReference;
    window.width = static_cast<int>(width);
    window.height = static_cast<int>(height);
    window.stride = static_cast<int>(strides[i]);
    window.format = HAL_PIXEL_FORMAT_RGBA_8888;
    window.layerCount = 1;
    window.handle = buffers[i];
    window.usage = kProbeNativeUsage;
    window.usage_deprecated = static_cast<int>(window.usage);
    printf("image %d : magic %#x version %zu handle fds=%d ints=%d fd0=%d\n", i,
           window.common.magic, static_cast<size_t>(window.common.version),
           buffers[i]->numFds, buffers[i]->numInts, buffers[i]->data[0]);

    const EGLint image_attributes[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    images[i] = create_image(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID,
                             &window, image_attributes);
    printf("image %d : eglCreateImageKHR -> %p%s\n", i, images[i],
           images[i] == EGL_NO_IMAGE_KHR ? " (failed)" : "");
    if (images[i] == EGL_NO_IMAGE_KHR) {
      printf("image %d : eglGetError %#x\n", i, get_error());
      ++failures;
      continue;
    }
    gl_bind_texture(GL_TEXTURE_2D, textures[i]);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    gl_tex_parameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    image_target(GL_TEXTURE_2D, images[i]);
    printf("image %d : target texture error %#x\n", i, gl_get_error());
    gl_bind_framebuffer(GL_FRAMEBUFFER, framebuffers[i]);
    gl_framebuffer_texture2d(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                             textures[i], 0);
    const GLenum status = gl_check_framebuffer_status(GL_FRAMEBUFFER);
    printf("image %d : framebuffer %#x%s\n", i, status,
           status == GL_FRAMEBUFFER_COMPLETE ? " (complete)" : " (INCOMPLETE)");
    complete[i] = status == GL_FRAMEBUFFER_COMPLETE;
    if (!complete[i]) ++failures;
  }

  // The render check only needs one working image; the A/B above is allowed to
  // fail one of them on purpose.
  if (complete[0]) {
    gl_bind_framebuffer(GL_FRAMEBUFFER, framebuffers[0]);
    gl_viewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    gl_clear_color(0.0f, 1.0f, 0.0f, 1.0f);
    gl_clear(GL_COLOR_BUFFER_BIT);
    gl_finish();
    uint8_t pixel[4] = {0, 0, 0, 0};
    gl_read_pixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    printf("render  : cleared green, glReadPixels -> %u %u %u %u (want 0 255 0 255)\n",
           pixel[0], pixel[1], pixel[2], pixel[3]);
    if (pixel[1] != 255) ++failures;
  }

  printf("teardown: %d failure(s); the driver is not unloaded\n", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // Unbuffered, because the interesting failures abort the process: a buffered
  // printf loses the last few lines and with them the answer to "how far did it
  // get".
  setvbuf(stdout, nullptr, _IONBF, 0);

  // The vendor's Android 12 destructor frees a pointer whose top-byte heap tag
  // Android 16's bionic refuses to accept ("Pointer tag for ... was truncated"),
  // which is a SIGABRT in a recovery process. Disabling heap tagging for this
  // process is the documented escape hatch and the only lever available without
  // patching the vendor binary. --keep-tags runs without it, to compare.
  bool keep_tags = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--keep-tags") == 0) keep_tags = true;
  }
#if defined(__BIONIC__) && defined(M_BIONIC_SET_HEAP_TAGGING_LEVEL) && \
    defined(M_HEAP_TAGGING_LEVEL_NONE)
  if (!keep_tags) {
    const int rc = mallopt(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
    printf("tagging : heap tagging off (mallopt -> %d)\n", rc);
  } else {
    printf("tagging : heap tagging left on (--keep-tags)\n");
  }
#else
  (void)keep_tags;
  printf("tagging : no mallopt heap-tagging constants at build time\n");
#endif

  if (argc > 1 && strcmp(argv[1], "--hwc") == 0) {
    const char* hwc_path = "/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so";
    for (int i = 2; i < argc; ++i) {
      if (strncmp(argv[i], "--", 2) != 0) hwc_path = argv[i];
    }
    return RunHwcProbe(hwc_path);
  }

  if (argc > 1 && strcmp(argv[1], "--present") == 0) {
    const char* hwc_path = "/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so";
    const char* shim_path = "/gpu/stock-vendor/lib64/hw/gralloc.mt6768.so";
    uint32_t width = 1080;
    uint32_t height = 2400;
    int positional = 0;
    for (int i = 2; i < argc; ++i) {
      if (strncmp(argv[i], "--", 2) == 0) continue;
      if (positional == 0) hwc_path = argv[i];
      else if (positional == 1) shim_path = argv[i];
      else if (positional == 2) width = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
      else if (positional == 3) height = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
      ++positional;
    }
    return RunPresentProbe(hwc_path, shim_path, width, height);
  }

  if (argc > 1 && strcmp(argv[1], "--egl") == 0) {
    const char* driver_path = "/gpu/stock-vendor/lib64/egl/libGLES_mali.so";
    const char* shim_path = "/gpu/stock-vendor/lib64/hw/gralloc.mt6768.so";
    uint32_t width = 1080;
    uint32_t height = 2400;
    int positional = 0;
    for (int i = 2; i < argc; ++i) {
      if (strncmp(argv[i], "--", 2) == 0) continue;
      if (positional == 0) driver_path = argv[i];
      else if (positional == 1) shim_path = argv[i];
      else if (positional == 2) width = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
      else if (positional == 3) height = static_cast<uint32_t>(strtoul(argv[i], nullptr, 0));
      ++positional;
    }
    return RunEglProbe(driver_path, shim_path, width, height);
  }

  if (argc > 1 && strcmp(argv[1], "--hotplug") == 0) {
    const char* hwc_path = "/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so";
    bool teardown = true;
    bool all_three = true;
    for (int i = 2; i < argc; ++i) {
      if (strcmp(argv[i], "--no-teardown") == 0) teardown = false;
      else if (strcmp(argv[i], "--only-hotplug") == 0) all_three = false;
      else if (strncmp(argv[i], "--", 2) != 0) hwc_path = argv[i];
    }
    return RunHotplugProbe(hwc_path, teardown, all_three);
  }

  if (argc > 1 && strcmp(argv[1], "--lifecycle") == 0) {
    const char* hwc_path = "/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so";
    bool teardown = true;
    for (int i = 2; i < argc; ++i) {
      if (strcmp(argv[i], "--no-teardown") == 0) teardown = false;
      else if (strncmp(argv[i], "--", 2) != 0) hwc_path = argv[i];
    }
    return RunLifecycleProbe(hwc_path, teardown);
  }

  const char* path = argc > 1 ? argv[1]
                              : "/gpu-rescue/aera-gpu/vendor/lib64/hw/gralloc.mt6768.so";
  const uint32_t width = argc > 2 ? static_cast<uint32_t>(strtoul(argv[2], nullptr, 0)) : 1080;
  const uint32_t height = argc > 3 ? static_cast<uint32_t>(strtoul(argv[3], nullptr, 0)) : 2400;

  printf("shim    : %s\n", path);
  printf("request : %ux%u format=RGBA_8888 producer=%#llx consumer=%#llx\n",
         width, height,
         static_cast<unsigned long long>(kProducerUsage),
         static_cast<unsigned long long>(kConsumerUsage));

  void* library = dlopen(path, RTLD_NOW);
  if (library == nullptr) {
    printf("dlopen  : %s\n", dlerror());
    PrintShimLog();
    return 1;
  }
  auto* module = static_cast<hw_module_t*>(dlsym(library, HAL_MODULE_INFO_SYM_AS_STR));
  if (module == nullptr) {
    printf("dlsym   : %s not found: %s\n", HAL_MODULE_INFO_SYM_AS_STR, dlerror());
    PrintShimLog();
    return 1;
  }
  printf("module  : %s (by %s)\n", module->name, module->author);

  gralloc1_device_t* device = nullptr;
  if (gralloc1_open(module, &device) != 0 || device == nullptr || !device->getFunction) {
    printf("gralloc1_open failed\n");
    PrintShimLog();
    return 1;
  }
  printf("device  : version %#x\n", device->common.version);

  LOOKUP(GRALLOC1_PFN_CREATE_DESCRIPTOR, CREATE_DESCRIPTOR);
  LOOKUP(GRALLOC1_PFN_DESTROY_DESCRIPTOR, DESTROY_DESCRIPTOR);
  LOOKUP(GRALLOC1_PFN_SET_DIMENSIONS, SET_DIMENSIONS);
  LOOKUP(GRALLOC1_PFN_SET_FORMAT, SET_FORMAT);
  LOOKUP(GRALLOC1_PFN_SET_PRODUCER_USAGE, SET_PRODUCER_USAGE);
  LOOKUP(GRALLOC1_PFN_SET_CONSUMER_USAGE, SET_CONSUMER_USAGE);
  LOOKUP(GRALLOC1_PFN_ALLOCATE, ALLOCATE);
  LOOKUP(GRALLOC1_PFN_GET_STRIDE, GET_STRIDE);
  LOOKUP(GRALLOC1_PFN_RELEASE, RELEASE);

  gralloc1_buffer_descriptor_t descriptor = 0;
  int rc = CREATE_DESCRIPTOR(device, &descriptor);
  printf("create  : %d (descriptor=%llu)\n", rc,
         static_cast<unsigned long long>(descriptor));
  if (rc != GRALLOC1_ERROR_NONE) {
    PrintShimLog();
    return 1;
  }
  printf("dims    : %d\n", SET_DIMENSIONS(device, descriptor, width, height));
  printf("format  : %d\n", SET_FORMAT(device, descriptor, HAL_PIXEL_FORMAT_RGBA_8888));
  printf("prod    : %d\n", SET_PRODUCER_USAGE(device, descriptor, kProducerUsage));
  printf("cons    : %d\n", SET_CONSUMER_USAGE(device, descriptor, kConsumerUsage));

  buffer_handle_t buffer = nullptr;
  rc = ALLOCATE(device, 1, &descriptor, &buffer);
  printf("allocate: %d handle=%p\n", rc, static_cast<const void*>(buffer));
  if (buffer != nullptr) {
    uint32_t stride = 0;
    const int stride_rc = GET_STRIDE(device, buffer, &stride);
    printf("stride  : %d value=%u (>= width: %s)\n", stride_rc, stride,
           stride >= width ? "yes" : "NO");
    RELEASE(device, buffer);
  }
  DESTROY_DESCRIPTOR(device, descriptor);
  gralloc1_close(device);

  PrintShimLog();
  return rc == GRALLOC1_ERROR_NONE && buffer != nullptr ? 0 : 1;
}
