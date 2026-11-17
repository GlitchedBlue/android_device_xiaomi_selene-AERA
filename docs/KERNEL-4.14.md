# Running AERA on selene's stock 4.14 kernel

AERA boots and works on both kernels selene has. Two kernel capabilities are
missing on 4.14, and both are kernel-side: WiFi and GPU acceleration. This note
is the exact evidence, so someone with the 4.14 tree can answer without
re-deriving it.

Kernel under discussion, extracted from the MIUI 14 stock boot image
(`V14.0.7.0.TKUMIXM`, Android 13):

```
Linux version 4.14.186-perf-gd9116d4b45d4
#1 SMP PREEMPT Mon Mar 20 17:07:34 CST 2023
Image.gz: 10658470 bytes, md5 89b6c5e0ac6110b032d679b68dc5ec2a
```

The working kernel, for comparison:

```
Linux version 4.19.325-cip136-st20-dieD
#2 SMP PREEMPT Fri Nov 13 15:12:12 MSK 2026
Image.gz: 12384187 bytes, md5 83b5653e0017301f84b441e600e8851d
```

Both configs are embedded (`IKCFG_ST`/`IKCFG_ED`, gzip) and were read from the
binaries, not from documentation.

## What already works on 4.14

| | |
| --- | --- |
| AERA boots, UI, touch | yes |
| MTP | yes — `CONFIG_USB_CONFIGFS_F_FS=y` |
| exFAT MicroSD | yes — `CONFIG_EXFAT_FS=y` (kernel driver; `exfat-fuse` is broken here) |
| Screenshots | yes (software canvas) |
| FBE decryption | yes (keymaster userspace, kernel-agnostic) |
| Dual boot with the ROM | yes (ramdisk construction, kernel-agnostic) |

Only the two items below are missing.

## 1. WiFi: the WLAN driver is not in the kernel image

The combo stack is otherwise configured:

```
CONFIG_MTK_COMBO=y
CONFIG_MTK_COMBO_CHIP_CONSYS_6768=y
CONFIG_MTK_COMBO_WIFI=y
CONFIG_CFG80211=y
# CONFIG_MTK_COMBO_COMM is not set
```

The 4.19 kernel differs in exactly one relevant line:

```
CONFIG_WLAN_DRV_BUILD_IN=y
```

That symbol does not appear in the 4.14 config **at all** (not even as
`# ... is not set`), so either it does not exist in that tree or it has no
prompt and the driver is forced to `obj-m`. Consequence: the 4.14 `vmlinux`
contains no WLAN driver and registers no `wlan0`. There is no `wmt_core` or
`stpwmt` string in it either; the only `mtk_wcn_*` strings are the
`mtk_wcn_cmb_stub_*` glue.

This is fine on MIUI, which loads the driver as a module from its own vendor
partition. It is not fine for us: the ROM we run ships no `.ko` built for
4.14, and a module built against a different tree fails `insmod` on vermagic.

Note `CONFIG_MTK_COMBO_COMM` is unset on **both** kernels, including the 4.19
one where WiFi works, so it is not the differentiator.

### What we need

Either:

- **A.** the WLAN driver linked in: build 4.14 with the driver as `obj-y`
  (`CONFIG_WLAN_DRV_BUILD_IN=y` if the symbol exists, otherwise the Makefile
  edit), or
- **B.** `wlan_drv_gen4m*.ko` built from the *same* tree and config as
  `4.14.186-perf-gd9116d4b45d4`, with its dependency list, so we can `insmod`
  it from the recovery ramdisk. This also covers MIUI users, whose
  `/vendor/lib/modules` already has the matching module.

Questions:

1. Does your 4.14 tree have `WLAN_DRV_BUILD_IN`? If yes, where, and does
   `=y` link the driver in cleanly, or does the Kconfig need a change?
2. If it has to stay a module: what is the module called on 6768, what does
   `modinfo -F vermagic` / `-F depends` say, and does it need anything else
   (`MTK_COMBO_COMM`, `MTK_WLAN_*`, a DTB node, a firmware path)?
3. Is there any reason a driver built as a module would not bind to
   `18000000.wifi` when insmod'd at recovery boot, before `/vendor` is
   mounted? (Firmware comes from `/vendor/firmware`, which we stage.)

## 2. GPU: kernel DDK is older than the userspace

The vendor's graphics userspace is Mali `r32p1` (UK version 11.31). It is the
stock vendor's, byte for byte, and cannot be swapped: it is the only Mali
userspace on the device, and the gralloc1 shim is written against MediaTek's
`r32p1` allocator.

The 4.14 kernel's kbase reports:

```
r26p0-01eac0 (UK version 11.27)
```

built from `mediatek/gpu/gpu_mali/mali_bifrost/mali-r25p0/drivers/gpu/arm/midgard/`
(the config string still says `CONFIG_MTK_GPU_VERSION="mali bifrost r25p0"`,
the binary is r26p0).

The 4.19 kernel that works reports:

```
r32p1-00bet5 (UK version 11.38)
```

built from `drivers/gpu/mediatek/gpu_mali/mali_valhall/mali-r32p1/...`, with
`CONFIG_MTK_GPU_VERSION="mali valhall r32p1"`.

So 11.38 works with the 11.31 userspace and 11.27 does not: the kernel DDK
must be at least the userspace's UK version. The failure is the kbase version
check — `Kernel DDK version %s` is printed once (that string is in the 4.14
`vmlinux`), the ioctl returns `EINVAL`, and `libGLES_mali.so` fails to
initialise, so AERA falls back to its software canvas. No crash, no dmesg
spam: it looks like "GPU just isn't used".

### What we need

A kbase on 4.14 whose UK version is >= 11.31, i.e. `r32p1` or newer, plus its
MTK platform glue (GED/DVFS, `kbase_platform_*`, `mtk_gpufreq`) building on
4.14.186.

Questions:

1. Which MTK 4.14 branch for mt6768 carries `mali_valhall/mali-r32p1` (or
   `mali_bifrost` r32p1)? Android 12/13 mt6768 trees should have it. A
   pointer to the branch is enough; we can do the merge.
2. If none exists: what is the smallest backport from r32p1 to 4.14.186 —
   which kbase files need 4.19-only APIs (`dma_fence`, `dma_buf`,
   `sync_file`, mm/ktime changes), and did anyone do it for another MTK
   4.14 device?
3. Is there a `r26p0` userspace (`libGLES_mali.so` + matching gralloc) for
   selene's vendor instead? We would rather not — the vendor is r32p1 and the
   gralloc1 shim targets it — but it is the other direction if r32p1 on 4.14
   is not realistic.

## Reproducing the measurements

Kernel and config, from any boot image:

```sh
python3 - <<'PY'
import struct, zlib, re
d = open("boot.img", "rb").read()
ks, ka, rs, ra, ss, sa, ta, pg = struct.unpack_from("<8I", d, 8)  # boot header v2
k = d[pg:pg + ks]                    # kernel is gzipped at page_size
open("Image.gz", "wb").write(k)
raw = zlib.decompressobj(31).decompress(k)
open("Image", "wb").write(raw)
print(re.search(rb"Linux version [^\x00\n ]+", raw).group(0).decode())
s, e = raw.find(b"IKCFG_ST"), raw.find(b"IKCFG_ED")
cfg = zlib.decompressobj(31).decompress(raw[s + 8:e])
open("config.txt", "wb").write(cfg)
PY
grep -E 'WLAN_DRV_BUILD_IN|MTK_COMBO_WIFI|MTK_GPU_VERSION' config.txt
```

Mali versions, from the kernel (the uncompressed `Image`):

```sh
grep -a -o 'r[0-9]*p[0-9]*-[0-9a-f]* (UK version [0-9.]*)' Image
```

and from the vendor userspace:

```sh
grep -a -o 'r[0-9]*p[0-9]*-[0-9a-f]* (UK version [0-9.]*)' \
  /vendor/lib64/egl/libGLES_mali.so
```
