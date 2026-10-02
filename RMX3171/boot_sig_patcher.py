#!/usr/bin/env python3
"""
boot_sig_patcher.py
MediaTek MT6768 (Helio G85 / Linux 4.14.186 AArch64)
Kernel Module Signature Check Bypass Patcher for Android boot.img

Function targeted: mod_verify_sig() (kernel 0xfde38)
Stock instructions:
    0xfde38: fd 7b bf a9    stp     x29, x30, [sp, #-16]!
    0xfde3c: fd 03 00 91    mov     x29, sp
Patched instructions:
    0xfde38: e0 03 1f 2a    mov     w0, wzr    ; return 0 (STATUS_SUCCESS)
    0xfde3c: c0 03 5f d6    ret
"""

import sys
import os
import struct
import gzip

STOCK_PATTERN = bytes.fromhex("fd7bbfa9fd030091290040f93f3500f1c3030054")
PATCHED_PREFIX = bytes.fromhex("e0031f2ac0035fd6")

def patch_boot_img(input_path, output_path):
    if not os.path.exists(input_path):
        print(f"[-] Error: Input file not found: {input_path}")
        return False

    with open(input_path, "rb") as f:
        data = bytearray(f.read())

    magic = data[:8]
    if magic != b"ANDROID!":
        print(f"[-] Error: Not a valid Android boot.img header (magic: {magic})")
        return False

    kernel_size = struct.unpack("<I", data[8:12])[0]
    page_size = struct.unpack("<I", data[36:40])[0]

    print(f"[+] Android boot.img identified:")
    print(f"    Page Size:   {page_size}")
    print(f"    Kernel Size: {kernel_size} (0x{kernel_size:x}) bytes")

    kernel_offset = page_size
    raw_kernel = data[kernel_offset : kernel_offset + kernel_size]

    # Detect compression (gzip: 1f 8b 08)
    is_gzip = raw_kernel[:3] == b"\x1f\x8b\x08"
    if is_gzip:
        print("[+] Compressed kernel payload (gzip) detected. Decompressing...")
        try:
            uncompressed_kernel = bytearray(gzip.decompress(raw_kernel))
        except Exception as e:
            print(f"[-] Error decompressing kernel: {e}")
            return False
    else:
        print("[+] Uncompressed kernel payload detected.")
        uncompressed_kernel = bytearray(raw_kernel)

    print(f"[+] Uncompressed Kernel Size: {len(uncompressed_kernel)} bytes")

    # Locate mod_verify_sig signature pattern
    pos = uncompressed_kernel.find(STOCK_PATTERN)
    if pos == -1:
        # Check if already patched
        already_patched = uncompressed_kernel.find(PATCHED_PREFIX + STOCK_PATTERN[8:])
        if already_patched != -1:
            print(f"[!] Kernel already contains mod_verify_sig bypass at 0x{already_patched:x}.")
            return True
        print("[-] Error: Target mod_verify_sig() signature pattern not found in kernel!")
        return False

    print(f"[+] Found mod_verify_sig() at kernel offset: 0x{pos:x}")
    uncompressed_kernel[pos : pos + 8] = PATCHED_PREFIX
    print(f"[+] Applied patch: 'mov w0, wzr; ret' (e0031f2ac0035fd6)")

    # Re-compress if originally gzip
    if is_gzip:
        print("[*] Re-compressing kernel payload with gzip...")
        recompressed_kernel = gzip.compress(uncompressed_kernel, compresslevel=9)
        new_k_size = len(recompressed_kernel)
        print(f"[+] Recompressed Kernel Size: {new_k_size} bytes (delta: {new_k_size - kernel_size:+d} bytes)")

        # Re-pack boot.img
        # Calculate padding to next page boundary
        def pad(size, page):
            return (page - (size % page)) % page

        ramdisk_size = struct.unpack("<I", data[16:20])[0]
        second_size = struct.unpack("<I", data[24:28])[0]

        # Calculate offsets
        ramdisk_page_offset = ((kernel_offset + kernel_size + page_size - 1) // page_size) * page_size
        ramdisk_data = data[ramdisk_page_offset : ramdisk_page_offset + ramdisk_size]

        rest_offset = ((ramdisk_page_offset + ramdisk_size + page_size - 1) // page_size) * page_size
        rest_data = data[rest_offset:]

        # Update kernel size in header
        struct.pack_into("<I", data, 8, new_k_size)
        header_page = data[:page_size]

        new_boot = bytearray()
        new_boot += header_page
        new_boot += recompressed_kernel
        new_boot += b"\x00" * pad(len(recompressed_kernel), page_size)
        new_boot += ramdisk_data
        new_boot += b"\x00" * pad(len(ramdisk_data), page_size)
        new_boot += rest_data

        out_data = new_boot
    else:
        data[kernel_offset : kernel_offset + len(uncompressed_kernel)] = uncompressed_kernel
        out_data = data

    with open(output_path, "wb") as f:
        f.write(out_data)

    print(f"[+] Patched boot image successfully saved to: {output_path} ({len(out_data)} bytes)")
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python3 boot_sig_patcher.py <boot_stock.img> [boot_patched.img]")
        sys.exit(1)

    in_img = sys.argv[1]
    out_img = sys.argv[2] if len(sys.argv) > 2 else "boot_patched_modsig.img"
    patch_boot_img(in_img, out_img)
