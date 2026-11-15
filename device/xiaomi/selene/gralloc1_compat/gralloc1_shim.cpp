// Copyright (C) 2026 AERA Recovery Project contributors
// SPDX-License-Identifier: Apache-2.0
//
// gralloc1 shim for selene (Redmi 10 / MT6768).
//
// AERA's Mali backend (aeraui/core/mali_display.cpp) allocates its scanout
// buffers through gralloc1: gralloc1_open(), then GRALLOC1_FUNCTION_{CREATE_
// DESCRIPTOR, DESTROY_DESCRIPTOR, SET_DIMENSIONS, SET_FORMAT, SET_PRODUCER_
// USAGE, SET_CONSUMER_USAGE, ALLOCATE, GET_STRIDE, RELEASE}.
//
// selene has no gralloc1 module. Its vendor stack is gralloc4 (HIDL
// allocator@4.0 + mapper@4.0). Opening the AOSP fbdev gralloc0 module in its
// place hands the backend a mismatched function table, so that is not an
// option either.
//
// What makes a shim possible here is that MediaTek's allocator implementation
// exports the plain ARM mali_gralloc entry points alongside the HIDL fetcher:
//
//   mali_gralloc_buffer_allocate(buffer_descriptor_t*, private_handle_t**)
//   mali_gralloc_buffer_free(private_handle_t*)
//
// (both visible in `nm -D` on
//  android.hardware.graphics.allocator@4.0-impl-mediatek.so). Calling those
// keeps the HIDL runtime completely out of the picture: no sp<IAllocator>, no
// second libhidlbase, no ABI crossing between an Android 16 shim and an
// Android 12 vendor impl. This shim is loaded *inside* AERA's private linker
// namespace, so even the libc it uses is the process copy the namespace is
// linked to.
//
// Allocation therefore produces exactly the handles MediaTek's HWC2 expects
// (its own gralloc metadata included), which is the whole point: HWC2
// composites these as the client target.
//
// ---------------------------------------------------------------------------
// buffer_descriptor_t layout (recovered from the binary, not from a header)
// ---------------------------------------------------------------------------
// ARM's public descriptor does NOT match this build. Disassembling
// mali_gralloc_buffer_allocate (0xb648) and mali_gralloc_derive_format_and_size
// (0x9fc0) in the vendor impl gives the offsets this shim uses:
//
//   +0x04  width            (ldp w10, w11, [x0, #4] in derive;
//                            str w8, [handle, #0x1c] in allocate)
//   +0x08  height           (same pair; str w8, [handle, #0x20])
//   +0x10  producer_usage   (ldp x8, x9, [x0, #0x10])
//   +0x18  consumer_usage
//   +0x20  hal_format       (u64; ldr w0, [x0, #0x20])
//   +0x28  layer_count      (read-before-write; cmp w9, #2 in derive)
//   +0x34  format_type      (ldr w1, [x19, #0x34] passed as select_format's
//                            mali_gralloc_format_type argument)
//   +0x58  size             (u64; derive writes it, allocator_allocate reads it
//                            as the ION allocation length)
//   +0x68  internal_format  (u64; select_format's return value)
//   +0x74  byte_stride
//   +0x78  pixel_stride     (gralloc1 GET_STRIDE wants this one)
//
// The descriptor is therefore handed over as an opaque zeroed byte buffer with
// these fields poked in, which avoids re-declaring a struct whose full layout
// (it is larger than ARM's) is not known.
//
// Two values cannot be read off the binary with certainty:
//
//   * format_type: select_format compares it against 1 and, when it is 1,
//     skips the usage-bit validation. That reads like 1 == "hal_format is
//     already an internal format", i.e. 0 == usage-based, but the vendor's own
//     caller is not visible in this .so.
//   * layer_count: a fresh gralloc4 allocation sets it to 1, but 0 may work.
//
// Both are therefore probed at first use (and can be forced from
// /gpu/gralloc1.conf, see ReadConfig below), so one build covers every
// combination and the winner is recorded in /tmp/gralloc1.log.
//
// No STL and no liblog on purpose: the private namespace shares exactly
// libc/libm/libdl with the recovery process, so anything else would have to be
// staged in the blob store as well.

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <hardware/gralloc.h>
#include <hardware/gralloc1.h>
#include <hardware/hardware.h>

namespace {

// ---------------------------------------------------------------------------
// Descriptor offsets, see the header comment.
// ---------------------------------------------------------------------------
constexpr size_t kWidthOff = 0x04;
constexpr size_t kHeightOff = 0x08;
constexpr size_t kProducerUsageOff = 0x10;
constexpr size_t kConsumerUsageOff = 0x18;
constexpr size_t kHalFormatOff = 0x20;
constexpr size_t kLayerCountOff = 0x28;
constexpr size_t kFormatTypeOff = 0x34;
constexpr size_t kSizeOff = 0x58;
constexpr size_t kInternalFormatOff = 0x68;
constexpr size_t kByteStrideOff = 0x74;
constexpr size_t kPixelStrideOff = 0x78;

// The highest offset the vendor code touches is 0xac; leave plenty of room so
// a slightly larger struct in a future build cannot scribble past our buffer.
constexpr size_t kDescriptorSize = 0x200;

struct Descriptor {
  uint8_t bytes[kDescriptorSize];

  void Clear() { memset(bytes, 0, sizeof(bytes)); }
  void Set32(size_t off, uint32_t value) { memcpy(bytes + off, &value, sizeof(value)); }
  void Set64(size_t off, uint64_t value) { memcpy(bytes + off, &value, sizeof(value)); }
  uint32_t Get32(size_t off) const {
    uint32_t value = 0;
    memcpy(&value, bytes + off, sizeof(value));
    return value;
  }
  uint64_t Get64(size_t off) const {
    uint64_t value = 0;
    memcpy(&value, bytes + off, sizeof(value));
    return value;
  }
};

// ---------------------------------------------------------------------------
// Logging: /tmp is a writable tmpfs in recovery, and this keeps the shim free
// of a liblog dependency. Read it with `adb shell cat /tmp/gralloc1.log`.
// ---------------------------------------------------------------------------
void Log(const char* format, ...) {
  char message[512];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (length <= 0) return;
  if (length > static_cast<int>(sizeof(message) - 2)) length = sizeof(message) - 2;
  message[length++] = '\n';
  int fd = open("/tmp/gralloc1.log", O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd < 0) return;
  ssize_t ignored = write(fd, message, length);
  (void)ignored;
  close(fd);
}

// ---------------------------------------------------------------------------
// Optional on-device overrides, so a single build can be tuned without a CI
// cycle. Format is one key=value per line:
//
//   impl=/gpu/stock-vendor/lib64/hw/android.hardware.graphics.allocator@4.0-impl-mediatek.so
//   format_type=0
//   layer_count=1
//   probe=1
//
// Missing file, missing key, or an unparsable value all fall back to the
// defaults below.
// ---------------------------------------------------------------------------
constexpr const char* kConfigPath = "/gpu/gralloc1.conf";

struct Config {
  char impl[512];
  int format_type;
  int layer_count;
  int probe;
};

uint32_t ParseUint(const char* text, uint32_t fallback) {
  if (text == nullptr || *text == '\0') return fallback;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text += 2;
  uint32_t value = 0;
  bool any = false;
  for (const char* p = text; *p != '\0'; ++p) {
    int digit;
    if (*p >= '0' && *p <= '9') {
      digit = *p - '0';
    } else if (*p >= 'a' && *p <= 'f') {
      digit = *p - 'a' + 10;
    } else if (*p >= 'A' && *p <= 'F') {
      digit = *p - 'A' + 10;
    } else {
      return fallback;
    }
    value = value * 16 + digit;  // values are hex, with or without 0x
    any = true;
  }
  return any ? value : fallback;
}

void ReadConfig(Config* config) {
  config->format_type = 0;
  config->layer_count = 1;
  config->probe = 1;
  config->impl[0] = '\0';

  char buffer[1024];
  int fd = open(kConfigPath, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
  close(fd);
  if (length <= 0) return;
  buffer[length] = '\0';

  char* line = buffer;
  while (line != nullptr && *line != '\0') {
    char* next = strchr(line, '\n');
    if (next != nullptr) *next++ = '\0';
    char* equals = strchr(line, '=');
    if (equals != nullptr) {
      *equals = '\0';
      const char* key = line;
      const char* value = equals + 1;
      if (strcmp(key, "impl") == 0) {
        snprintf(config->impl, sizeof(config->impl), "%s", value);
      } else if (strcmp(key, "format_type") == 0) {
        config->format_type = static_cast<int>(ParseUint(value, 0));
      } else if (strcmp(key, "layer_count") == 0) {
        config->layer_count = static_cast<int>(ParseUint(value, 1));
      } else if (strcmp(key, "probe") == 0) {
        config->probe = static_cast<int>(ParseUint(value, 1));
      } else {
        Log("config: ignoring unknown key %s", key);
      }
    }
    line = next;
  }
}

// ---------------------------------------------------------------------------
// MediaTek / ARM mali_gralloc ABI
// ---------------------------------------------------------------------------
using AllocateFn = int (*)(Descriptor*, void**);
using FreeFn = void (*)(void*);

// The vendor impl sits next to this module in hw/, but the shim may be loaded
// from the blob store, from /gpu/lib64/hw, or by an explicit path in the config
// file. Try the plausible locations rather than relying on dladdr (whose
// Dl_info is behind _GNU_SOURCE in bionic).
constexpr const char* kImplName =
    "android.hardware.graphics.allocator@4.0-impl-mediatek.so";
constexpr const char* kImplDirs[] = {
    "/gpu/stock-vendor/lib64/hw",
    "/gpu/lib64/hw",
    "/gpu/stock-vendor/lib64",
    "/gpu/lib64",
};

struct Allocator {
  void* library;
  AllocateFn allocate;
  FreeFn free_buffer;
  int format_type;
  int layer_count;
};

Allocator g_allocator;
bool g_allocator_ready = false;

// Try one (format_type, layer_count) combination on a real allocation. Returns
// true on success and fills in the resulting stride.
bool TryAllocate(const Descriptor& source, void** handle, uint32_t* stride,
                 int format_type, int layer_count, const char* reason) {
  Descriptor descriptor;
  memcpy(descriptor.bytes, source.bytes, sizeof(descriptor.bytes));
  descriptor.Set32(kFormatTypeOff, static_cast<uint32_t>(format_type));
  descriptor.Set32(kLayerCountOff, static_cast<uint32_t>(layer_count));

  void* result = nullptr;
  int error = g_allocator.allocate(&descriptor, &result);
  Log("allocate %s: format_type=%d layer_count=%d -> error=%d handle=%p "
      "size=%llu internal_format=%#llx byte_stride=%d pixel_stride=%d",
      reason, format_type, layer_count, error, result,
      static_cast<unsigned long long>(descriptor.Get64(kSizeOff)),
      static_cast<unsigned long long>(descriptor.Get64(kInternalFormatOff)),
      static_cast<int>(descriptor.Get32(kByteStrideOff)),
      static_cast<int>(descriptor.Get32(kPixelStrideOff)));
  if (error != 0 || result == nullptr) return false;

  uint32_t pixel_stride = descriptor.Get32(kPixelStrideOff);
  uint32_t byte_stride = descriptor.Get32(kByteStrideOff);
  uint32_t width = source.Get32(kWidthOff);
  // GET_STRIDE is in pixels. If the vendor left pixel_stride empty, fall back
  // to the width rather than to byte_stride (which is 4x for RGBA_8888).
  *stride = pixel_stride >= width ? pixel_stride : width;
  if (byte_stride != 0 && byte_stride != *stride) {
    Log("note: byte_stride=%u pixel_stride=%u width=%u (using %u)", byte_stride,
        pixel_stride, width, *stride);
  }
  *handle = result;
  return true;
}

void* OpenImpl(const Config& config) {
  char path[512];
  if (config.impl[0] != '\0') {
    Log("trying %s", config.impl);
    void* library = dlopen(config.impl, RTLD_NOW | RTLD_LOCAL);
    if (library != nullptr) return library;
    const char* error = dlerror();
    Log("dlopen failed: %s", error != nullptr ? error : "(no error)");
  }
  for (const char* directory : kImplDirs) {
    snprintf(path, sizeof(path), "%s/%s", directory, kImplName);
    void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (library != nullptr) {
      Log("using %s", path);
      return library;
    }
  }
  // Last resort: let the namespace search path resolve the bare soname.
  void* library = dlopen(kImplName, RTLD_NOW | RTLD_LOCAL);
  if (library != nullptr) Log("using %s from the namespace search path", kImplName);
  return library;
}

bool EnsureAllocator() {
  if (g_allocator_ready) return g_allocator.allocate != nullptr;

  Config config;
  ReadConfig(&config);

  g_allocator.library = OpenImpl(config);
  if (g_allocator.library == nullptr) {
    Log("could not open %s anywhere", kImplName);
    g_allocator_ready = true;
    return false;
  }
  // Mangled names, so the lookup does not depend on demangling support.
  g_allocator.allocate = reinterpret_cast<AllocateFn>(dlsym(g_allocator.library,
      "_Z28mali_gralloc_buffer_allocateP19buffer_descriptor_tPP16private_handle_t"));
  g_allocator.free_buffer = reinterpret_cast<FreeFn>(dlsym(g_allocator.library,
      "_Z24mali_gralloc_buffer_freeP16private_handle_t"));
  if (g_allocator.allocate == nullptr || g_allocator.free_buffer == nullptr) {
    Log("mali_gralloc entry points missing from the allocator impl");
    g_allocator.allocate = nullptr;
    g_allocator.free_buffer = nullptr;
  }
  g_allocator.format_type = config.format_type;
  g_allocator.layer_count = config.layer_count;
  g_allocator_ready = true;
  return g_allocator.allocate != nullptr;
}

// ---------------------------------------------------------------------------
// Descriptor and buffer bookkeeping
// ---------------------------------------------------------------------------
constexpr int kMaxDescriptors = 32;
constexpr int kMaxBuffers = 16;

struct DescriptorSlot {
  bool used;
  uint32_t width;
  uint32_t height;
  int32_t format;
  uint64_t producer_usage;
  uint64_t consumer_usage;
};

struct BufferSlot {
  bool used;
  void* handle;
  uint32_t stride;
};

DescriptorSlot g_descriptors[kMaxDescriptors];
BufferSlot g_buffers[kMaxBuffers];

DescriptorSlot* FindDescriptor(gralloc1_buffer_descriptor_t id) {
  // Ids are slot index + 1, so a stale id can never alias a live slot.
  for (int i = 0; i < kMaxDescriptors; ++i) {
    if (g_descriptors[i].used &&
        static_cast<gralloc1_buffer_descriptor_t>(i + 1) == id) {
      return &g_descriptors[i];
    }
  }
  return nullptr;
}

BufferSlot* FindBuffer(buffer_handle_t handle) {
  for (auto& slot : g_buffers) {
    if (slot.used && slot.handle == const_cast<void*>(static_cast<const void*>(handle))) return &slot;
  }
  return nullptr;
}

BufferSlot* ReserveBuffer() {
  for (auto& slot : g_buffers) {
    if (!slot.used) return &slot;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// gralloc1 device
// ---------------------------------------------------------------------------
gralloc1_error_t CreateDescriptor(gralloc1_device_t*, gralloc1_buffer_descriptor_t* out) {
  if (out == nullptr) return GRALLOC1_ERROR_BAD_VALUE;
  for (int i = 0; i < kMaxDescriptors; ++i) {
    if (g_descriptors[i].used) continue;
    memset(&g_descriptors[i], 0, sizeof(g_descriptors[i]));
    g_descriptors[i].used = true;
    *out = static_cast<gralloc1_buffer_descriptor_t>(i + 1);
    return GRALLOC1_ERROR_NONE;
  }
  Log("out of descriptor slots");
  return GRALLOC1_ERROR_NO_RESOURCES;
}

gralloc1_error_t DestroyDescriptor(gralloc1_device_t*, gralloc1_buffer_descriptor_t id) {
  DescriptorSlot* slot = FindDescriptor(id);
  if (slot == nullptr) return GRALLOC1_ERROR_BAD_DESCRIPTOR;
  slot->used = false;
  return GRALLOC1_ERROR_NONE;
}

gralloc1_error_t SetDimensions(gralloc1_device_t*, gralloc1_buffer_descriptor_t id,
                              uint32_t width, uint32_t height) {
  DescriptorSlot* slot = FindDescriptor(id);
  if (slot == nullptr) return GRALLOC1_ERROR_BAD_DESCRIPTOR;
  slot->width = width;
  slot->height = height;
  return GRALLOC1_ERROR_NONE;
}

gralloc1_error_t SetFormat(gralloc1_device_t*, gralloc1_buffer_descriptor_t id,
                          int32_t format) {
  DescriptorSlot* slot = FindDescriptor(id);
  if (slot == nullptr) return GRALLOC1_ERROR_BAD_DESCRIPTOR;
  slot->format = format;
  return GRALLOC1_ERROR_NONE;
}

gralloc1_error_t SetProducerUsage(gralloc1_device_t*, gralloc1_buffer_descriptor_t id,
                                 uint64_t usage) {
  DescriptorSlot* slot = FindDescriptor(id);
  if (slot == nullptr) return GRALLOC1_ERROR_BAD_DESCRIPTOR;
  slot->producer_usage = usage;
  return GRALLOC1_ERROR_NONE;
}

gralloc1_error_t SetConsumerUsage(gralloc1_device_t*, gralloc1_buffer_descriptor_t id,
                                 uint64_t usage) {
  DescriptorSlot* slot = FindDescriptor(id);
  if (slot == nullptr) return GRALLOC1_ERROR_BAD_DESCRIPTOR;
  slot->consumer_usage = usage;
  return GRALLOC1_ERROR_NONE;
}

gralloc1_error_t Allocate(gralloc1_device_t*, uint32_t count,
                         const gralloc1_buffer_descriptor_t* ids,
                         buffer_handle_t* buffers) {
  if (count == 0 || ids == nullptr || buffers == nullptr) return GRALLOC1_ERROR_BAD_VALUE;
  if (!EnsureAllocator()) return GRALLOC1_ERROR_UNSUPPORTED;

  for (uint32_t i = 0; i < count; ++i) {
    DescriptorSlot* slot = FindDescriptor(ids[i]);
    if (slot == nullptr) return GRALLOC1_ERROR_BAD_DESCRIPTOR;

    Descriptor source;
    source.Clear();
    source.Set32(kWidthOff, slot->width);
    source.Set32(kHeightOff, slot->height);
    source.Set64(kProducerUsageOff, slot->producer_usage);
    source.Set64(kConsumerUsageOff, slot->consumer_usage);
    source.Set64(kHalFormatOff, static_cast<uint64_t>(slot->format));

    void* handle = nullptr;
    uint32_t stride = 0;
    bool allocated = TryAllocate(source, &handle, &stride, g_allocator.format_type,
                                 g_allocator.layer_count, "primary");
    if (!allocated) {
      Config config;
      ReadConfig(&config);
      if (!config.probe) {
        Log("allocation failed and probing is disabled");
        return GRALLOC1_ERROR_NO_RESOURCES;
      }
      // One build covers every combination the binary left ambiguous.
      const int format_types[2] = {g_allocator.format_type, 1 - g_allocator.format_type};
      const int layer_counts[2] = {g_allocator.layer_count, 1 - g_allocator.layer_count};
      for (int f = 0; f < 2 && !allocated; ++f) {
        for (int l = 0; l < 2 && !allocated; ++l) {
          if (format_types[f] == g_allocator.format_type &&
              layer_counts[l] == g_allocator.layer_count) {
            continue;
          }
          allocated = TryAllocate(source, &handle, &stride, format_types[f],
                                  layer_counts[l], "probe");
          if (allocated) {
            Log("probe won: format_type=%d layer_count=%d (add these to %s)",
                format_types[f], layer_counts[l], kConfigPath);
            g_allocator.format_type = format_types[f];
            g_allocator.layer_count = layer_counts[l];
          }
        }
      }
    }
    if (!allocated) {
      Log("allocation failed for %ux%u format=%#x producer=%#llx consumer=%#llx",
          slot->width, slot->height, slot->format,
          static_cast<unsigned long long>(slot->producer_usage),
          static_cast<unsigned long long>(slot->consumer_usage));
      return GRALLOC1_ERROR_NO_RESOURCES;
    }

    BufferSlot* buffer = ReserveBuffer();
    if (buffer == nullptr) {
      Log("out of buffer slots, freeing %p", handle);
      g_allocator.free_buffer(handle);
      return GRALLOC1_ERROR_NO_RESOURCES;
    }
    buffer->used = true;
    buffer->handle = handle;
    buffer->stride = stride;
    Log("allocated %p stride=%u", handle, stride);
    buffers[i] = static_cast<const native_handle_t*>(handle);
  }
  return GRALLOC1_ERROR_NONE;
}

gralloc1_error_t GetStride(gralloc1_device_t*, buffer_handle_t handle, uint32_t* stride) {
  if (handle == nullptr || stride == nullptr) return GRALLOC1_ERROR_BAD_HANDLE;
  BufferSlot* buffer = FindBuffer(handle);
  if (buffer == nullptr) return GRALLOC1_ERROR_BAD_HANDLE;
  *stride = buffer->stride;
  return GRALLOC1_ERROR_NONE;
}

void Release(gralloc1_device_t*, buffer_handle_t handle) {
  if (handle == nullptr) return;
  BufferSlot* buffer = FindBuffer(handle);
  if (buffer != nullptr) {
    buffer->used = false;
    buffer->handle = nullptr;
  }
  if (!EnsureAllocator()) return;
  Log("freeing %p", const_cast<void*>(static_cast<const void*>(handle)));
  g_allocator.free_buffer(const_cast<void*>(static_cast<const void*>(handle)));
}

gralloc1_function_pointer_t GetFunction(gralloc1_device_t*, int32_t descriptor) {
  switch (descriptor) {
    case GRALLOC1_FUNCTION_CREATE_DESCRIPTOR:
      return reinterpret_cast<gralloc1_function_pointer_t>(CreateDescriptor);
    case GRALLOC1_FUNCTION_DESTROY_DESCRIPTOR:
      return reinterpret_cast<gralloc1_function_pointer_t>(DestroyDescriptor);
    case GRALLOC1_FUNCTION_SET_DIMENSIONS:
      return reinterpret_cast<gralloc1_function_pointer_t>(SetDimensions);
    case GRALLOC1_FUNCTION_SET_FORMAT:
      return reinterpret_cast<gralloc1_function_pointer_t>(SetFormat);
    case GRALLOC1_FUNCTION_SET_PRODUCER_USAGE:
      return reinterpret_cast<gralloc1_function_pointer_t>(SetProducerUsage);
    case GRALLOC1_FUNCTION_SET_CONSUMER_USAGE:
      return reinterpret_cast<gralloc1_function_pointer_t>(SetConsumerUsage);
    case GRALLOC1_FUNCTION_ALLOCATE:
      return reinterpret_cast<gralloc1_function_pointer_t>(Allocate);
    case GRALLOC1_FUNCTION_GET_STRIDE:
      return reinterpret_cast<gralloc1_function_pointer_t>(GetStride);
    case GRALLOC1_FUNCTION_RELEASE:
      return reinterpret_cast<gralloc1_function_pointer_t>(Release);
    default:
      return nullptr;
  }
}

int Close(hw_device_t*) {
  return 0;
}

int Open(const hw_module_t*, const char* name, hw_device_t** out) {
  if (out == nullptr) return -EINVAL;
  // AOSP 16's gralloc1_open() passes GRALLOC_HARDWARE_MODULE_ID ("gralloc");
  // older callers pass GRALLOC_HARDWARE_GPU0 ("gpu0"). There is exactly one
  // device here, so either name (or none) means the same thing -- and getting
  // this wrong is invisible until a device tries to open the module, which is
  // why the first probe run reported "unsupported id gralloc".
  if (name != nullptr && strcmp(name, GRALLOC_HARDWARE_GPU0) != 0 &&
      strcmp(name, GRALLOC_HARDWARE_MODULE_ID) != 0) {
    Log("unsupported id %s", name);
    return -EINVAL;
  }
  // Static storage: no operator new, so the shim needs no C++ runtime.
  static gralloc1_device_t device;
  memset(&device, 0, sizeof(device));
  device.common.tag = HARDWARE_DEVICE_TAG;
  // gralloc1 device version, same encoding as GRALLOC1_MAKE_VERSION(1, 0).
  device.common.version = (1 << 24) | 0;
  device.common.close = Close;
  device.getFunction = GetFunction;
  *out = reinterpret_cast<hw_device_t*>(&device);
  return 0;
}

hw_module_methods_t g_methods = {
    .open = Open,
};

}  // namespace

// Exported for dlsym(HAL_MODULE_INFO_SYM_AS_STR) in mali_display.cpp.
hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = GRALLOC_MODULE_API_VERSION_0_1,
    .hal_api_version = HARDWARE_HAL_API_VERSION,
    .id = GRALLOC_HARDWARE_MODULE_ID,
    .name = "AERA gralloc1 shim over MediaTek allocator@4.0",
    .author = "AERA Recovery Project",
    .methods = &g_methods,
    .dso = nullptr,
    .reserved = {},
};
