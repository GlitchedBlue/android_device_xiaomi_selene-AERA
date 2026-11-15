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

#include <dlfcn.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
volatile int g_hotplug_count = 0;
volatile int g_hotplug_connected = 0;
volatile unsigned long long g_hotplug_display = ~0ull;

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

int RunHotplugProbe(const char* path, bool teardown) {
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
  const int rc = reg(device, HWC2_CALLBACK_HOTPLUG, nullptr,
                     reinterpret_cast<hwc2_function_pointer_t>(HotplugCallback));
  printf("register: %d connected_right_after=%d callbacks=%d\n", rc,
         __atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_hotplug_count, __ATOMIC_SEQ_CST));

  // Poll for up to five seconds, printing the first time the state changes.
  int waited_ms = 0;
  while (waited_ms < 5000 && !__atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST)) {
    usleep(100000);
    waited_ms += 100;
  }
  printf("waited  : %d ms connected=%d callbacks=%d display=%llu\n", waited_ms,
         __atomic_load_n(&g_hotplug_connected, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_hotplug_count, __ATOMIC_SEQ_CST),
         __atomic_load_n(&g_hotplug_display, __ATOMIC_SEQ_CST));

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

  if (argc > 1 && strcmp(argv[1], "--hotplug") == 0) {
    const char* hwc_path = "/gpu/stock-vendor/lib64/hw/hwcomposer.mt6768.so";
    bool teardown = true;
    for (int i = 2; i < argc; ++i) {
      if (strcmp(argv[i], "--no-teardown") == 0) teardown = false;
      else if (strncmp(argv[i], "--", 2) != 0) hwc_path = argv[i];
    }
    return RunHotplugProbe(hwc_path, teardown);
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
