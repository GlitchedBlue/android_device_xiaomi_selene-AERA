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
// The second form is the crash-loop test: it opens and closes the vendor HWC2
// module without recovery being involved.
//
// LD_LIBRARY_PATH must contain the staged Android 12 libraries, because the
// shim dlopens the vendor allocator impl, whose own DT_NEEDED (libhidlbase,
// libion, libdrm, ...) have to resolve to the vendor copies.

#include <dlfcn.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/gralloc1.h>
#include <hardware/hardware.h>

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
