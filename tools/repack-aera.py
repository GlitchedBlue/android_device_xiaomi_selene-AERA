#!/usr/bin/env python3
"""Pair AERA's recovery ramdisk with any 4.14/4.19 boot image's kernel.

AERA's userspace is kernel-agnostic: everything it needs from the kernel is a
Mali DDK at least as new as the vendor userspace's, a WLAN driver that
registers wlan0, and the normal block/FUSE/FunctionFS plumbing. So the way to
run AERA on a kernel other than the one this repository ships is to keep that
kernel and replace the ramdisk.

This tool takes a donor boot image (any kernel that boots selene), keeps its
kernel, dtb, dtbo, load addresses, page size, os_version and cmdline, and
writes AERA's ramdisk into it:

    python tools/repack-aera.py \
        --donor stock-boot.img \
        --aera  AERA-selene-dualboot-dieD419.img \
        --out   AERA-selene-my-kernel.img \
        --pad   67108864

To use a kernel that is not already inside a boot image (an AnyKernel3 zip
ships a raw `Image`, a CI artifact ships `Image.gz`), pass it with --kernel;
a raw Image is gzipped for you:

    python tools/repack-aera.py --donor stock-boot.img --kernel Image \
        --aera AERA-selene-dualboot-dieD419.img --out out.img --pad 67108864

Both inputs must be boot header v2 images -- selene has no vendor_boot, and the
device tree lives in the boot image (dtb field), not in a separate partition.
--aera is read only for its ramdisk; every other field comes from --donor.

selene cannot `fastboot boot`, so the result has to be flashed to boot_a (or
boot_b) with a rollback image kept on hand.
"""

import argparse
import hashlib
import struct
import sys

PAGE_DEFAULT = 2048
BOOT_MAGIC = b"ANDROID!"
HDR_V2_SIZE = 1660
SELENE_BOOT_PARTITION = 64 * 1024 * 1024


def align(n, page):
    return (n + page - 1) // page * page


def read_header(data):
    if data[:8] != BOOT_MAGIC:
        raise SystemExit("not an Android boot image (bad magic)")
    h = {}
    (h["kernel_size"], h["kernel_addr"], h["ramdisk_size"], h["ramdisk_addr"],
     h["second_size"], h["second_addr"], h["tags_addr"], h["page_size"],
     h["header_version"], h["os_version"]) = struct.unpack_from("<10I", data, 8)
    h["name"] = data[48:64]
    h["cmdline"] = data[64:576]
    h["id"] = data[576:608]
    h["extra_cmdline"] = data[608:1632]
    if h["header_version"] < 1:
        raise SystemExit("boot header v%d has no dtb support" % h["header_version"])
    h["recovery_dtbo_size"] = struct.unpack_from("<I", data, 1632)[0]
    h["recovery_dtbo_offset"] = struct.unpack_from("<Q", data, 1636)[0]
    h["header_size"] = struct.unpack_from("<I", data, 1644)[0]
    if h["header_version"] >= 2:
        h["dtb_size"] = struct.unpack_from("<I", data, 1648)[0]
        h["dtb_addr"] = struct.unpack_from("<Q", data, 1652)[0]
    else:
        h["dtb_size"] = 0
        h["dtb_addr"] = 0
    return h


def extract(data, h):
    """Return (kernel, ramdisk, dtbo, dtb), located by file order.

    Boot header v2 puts the sections in this order, each page aligned:
    header, kernel, ramdisk, second, recovery_dtbo, dtb. The offsets are not in
    the header (only the dtbo offset is), so file order is the only way to find
    them.
    """
    page = h["page_size"]
    off = page
    kernel = data[off:off + h["kernel_size"]]
    off += align(h["kernel_size"], page)
    ramdisk = data[off:off + h["ramdisk_size"]]
    off += align(h["ramdisk_size"], page)
    off += align(h["second_size"], page)
    dtbo = data[off:off + h["recovery_dtbo_size"]]
    off += align(h["recovery_dtbo_size"], page)
    dtb = data[off:off + h["dtb_size"]]
    return kernel, ramdisk, dtbo, dtb


def build(h, kernel, ramdisk, dtbo, dtb, cmdline=None):
    page = h["page_size"]
    cmd = cmdline if cmdline is not None else h["cmdline"]
    cmd = cmd.split(b"\x00")[0][:511].ljust(512, b"\x00")

    dtbo_offset = page + align(len(kernel), page) + align(len(ramdisk), page)
    header = bytearray(HDR_V2_SIZE)
    header[0:8] = BOOT_MAGIC
    struct.pack_into("<10I", header, 8,
                     len(kernel), h["kernel_addr"],
                     len(ramdisk), h["ramdisk_addr"],
                     0, 0,
                     h["tags_addr"], page, 2, h["os_version"])
    header[48:64] = b"aera".ljust(16, b"\x00")
    header[64:576] = cmd
    # id: sha1 over the payloads, the way mkbootimg fills it. It must be the
    # 20-byte digest inside a 32-byte field -- writing 32 bytes here shifts the
    # rest of the header and truncates it.
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, dtbo, dtb):
        digest.update(blob)
    header[576:608] = digest.digest().ljust(32, b"\x00")
    struct.pack_into("<I", header, 1632, len(dtbo))
    struct.pack_into("<Q", header, 1636, dtbo_offset)
    struct.pack_into("<I", header, 1644, HDR_V2_SIZE)
    struct.pack_into("<I", header, 1648, len(dtb))
    struct.pack_into("<Q", header, 1652, h["dtb_addr"])

    out = bytearray()
    out += bytes(header).ljust(page, b"\x00")
    out += kernel.ljust(align(len(kernel), page), b"\x00")
    out += ramdisk.ljust(align(len(ramdisk), page), b"\x00")
    out += dtbo.ljust(align(len(dtbo), page), b"\x00")
    out += dtb.ljust(align(len(dtb), page), b"\x00")
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(
        description="Put AERA's ramdisk into another kernel's boot image.")
    ap.add_argument("--donor", required=True,
                    help="boot.img that supplies kernel/dtb/dtbo/addresses")
    ap.add_argument("--kernel", default=None,
                    help="use this kernel instead of the donor's (raw Image or Image.gz)")
    ap.add_argument("--aera", required=True, help="AERA boot.img (ramdisk source)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--cmdline", default=None,
                    help="override the donor's cmdline (default: keep it)")
    ap.add_argument("--pad", type=int, default=0,
                    help="pad the output to this many bytes (selene: 67108864)")
    args = ap.parse_args()

    donor = open(args.donor, "rb").read()
    dh = read_header(donor)
    kernel, _, dtbo, dtb = extract(donor, dh)

    if args.kernel:
        kernel = open(args.kernel, "rb").read()
        if kernel[:2] != b"\x1f\x8b":
            import zlib
            co = zlib.compressobj(9, zlib.DEFLATED, 31)
            kernel = co.compress(kernel) + co.flush()
            print("kernel gzipped to %d bytes" % len(kernel))

    aera = open(args.aera, "rb").read()
    ah = read_header(aera)
    ramdisk = extract(aera, ah)[1]

    if not ramdisk:
        raise SystemExit("no ramdisk in %s" % args.aera)
    if not kernel:
        raise SystemExit("no kernel in %s" % args.donor)

    cmd = args.cmdline.encode() if args.cmdline else None
    out = build(dh, kernel, ramdisk, dtbo, dtb, cmd)
    if args.pad:
        if len(out) > args.pad:
            raise SystemExit("image is %d bytes, larger than --pad %d" % (len(out), args.pad))
        out += b"\x00" * (args.pad - len(out))
    open(args.out, "wb").write(out)

    print("donor kernel   %d bytes @%#x" % (len(kernel), dh["kernel_addr"]))
    print("aera ramdisk   %d bytes @%#x" % (len(ramdisk), dh["ramdisk_addr"]))
    print("dtbo           %d bytes, dtb %d bytes @%#x"
          % (len(dtbo), len(dtb), dh["dtb_addr"]))
    print("out            %s, %d bytes" % (args.out, len(out)))


if __name__ == "__main__":
    main()
