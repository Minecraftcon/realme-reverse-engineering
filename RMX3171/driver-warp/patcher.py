#!/usr/bin/env python3
"""
MediaTek Wi-Fi Driver Warp & Code Cave Trampoline Patcher
Target: wlan_drv_gen4m.ko (Realme Narzo 30A / MT6768 Helio G85)
"""

import os
import sys
import struct
import shutil
import capstone

STOCK_KO = "/home/shado/Documents/realme-reverse-engineering/RMX3171/drivers-backup-stock/wlan_drv_gen4m.ko"
OUTPUT_KO = "/home/shado/Documents/realme-reverse-engineering/RMX3171/driver-warp/wlan_drv_gen4m_warped.ko"

TEXT_OFFSET = 0x44
RODATA_OFFSET = 0x1cbff8
RELA_RODATA_OFFSET = 0x484a20
RELA_RODATA_SIZE = 0x43c8

# Code Caves identified
CAVE_DUMPMEM8_OFFSET = 0x16798  # Size: 1252 bytes
CAVE_DUMPMEM32_OFFSET = 0x16c7c # Size: 864 bytes

def verify_stock_driver(data):
    # Verify ELF magic
    if data[:4] != b"\x7fELF":
        raise ValueError("Invalid ELF magic!")
    
    # Verify target string at 0x17106f
    str_target = data[0x17106f:0x17106f+10]
    if str_target != b"radiotap%d":
        raise ValueError(f"Expected b'radiotap%d' at 0x17106f, got {str_target}")
    
    print("[+] Stock driver validated successfully.")

def apply_step1_rename(data, new_name=b"mon%d\x00\x00\x00\x00\x00\x00"):
    """
    Step 1: Rename 'radiotap%d' -> 'mon%d' (padded with nulls to 11 bytes)
    """
    offset = 0x17106f
    old_bytes = data[offset:offset+11]
    
    # Ensure padded to 11 bytes with nulls
    padded = new_name.ljust(11, b"\x00")[:11]
    data[offset:offset+11] = padded
    print(f"[+] Step 1: Renamed interface from {old_bytes} -> {padded}")
    return data

def apply_step2_tx_wiring(data):
    """
    Step 2: Wire mon0's net_device_ops to wlan_netdev_ops (addend 0x1780).
    wlan_netdev_ops has .ndo_start_xmit pointing directly to wlanHardStartXmit (0x65840),
    enabling raw packet transmission on mon0!
    
    Also unblocks TX queues and carrier:
    1. Replaces netif_carrier_off (sym 1866) with netif_carrier_on (sym 2127) at 0x65a18.
    2. NOPs out netif_tx_stop_all_queues at 0x65a20 so transmit queues stay running.
    """
    rela_text_off = 0x1fe5e0
    rela_text_sz = 0x27f0d8
    count = 0
    for i in range(0, rela_text_sz, 24):
        entry_off = rela_text_off + i
        r_offset, r_info, r_addend = struct.unpack("<QQq", data[entry_off : entry_off + 24])
        
        # 1. Wire net_device_ops to wlan_netdev_ops (0x1780)
        if 0x65a00 <= r_offset <= 0x65a0c and r_addend == 0x1580:
            struct.pack_into("<q", data, entry_off + 16, 0x1780)
            count += 1
            
        # 2. Patch netif_carrier_off -> netif_carrier_on (sym 2127, type 283)
        elif r_offset == 0x65a18 and (r_info >> 32) == 1866:
            new_r_info = (2127 << 32) | 283
            struct.pack_into("<Q", data, entry_off + 8, new_r_info)
            print("[+] Step 2 (Carrier): Patched 0x65a18 from netif_carrier_off to netif_carrier_on.")
            
        # 3. NOP out netif_tx_stop_all_queues (clear relocation and NOP instruction)
        elif r_offset == 0x65a20 and (r_info >> 32) == 1867:
            struct.pack_into("<Qq", data, entry_off + 8, 0, 0)
            # Patch instruction at 0x44 + 0x65a20 to NOP (0xd503201f)
            data[0x44 + 0x65a20 : 0x44 + 0x65a20 + 4] = b"\x1f\x20\x03\xd5"
            print("[+] Step 2 (TX Queues): NOPed netif_tx_stop_all_queues at 0x65a20.")
            
    print(f"[+] Step 2 (TX Wiring): Wired mon0 net_device_ops to wlanHardStartXmit (updated {count} relocations).")
    return data

def apply_step3_code_cave_autospawn(data):
    """
    Step 3: Code Cave Trampoline (Auto-Spawn mon0 on Boot).
    Hooks wlanNetCreate (0x670a0) to jump to Code Cave A (dumpMemory8 at 0x16798).
    Code Cave A executes:
      1. Loads prGlueInfo and wlan0 net_device.
      2. Invokes priv_driver_set_monitor(wlan0, "MONITOR 1 1 20 0", 16).
         This automatically initializes RF monitor mode and schedules mon0 creation.
      3. Safely returns to wlanNetCreate's normal exit path (0x670f4).
    """
    # 1. Clear dead relocations in the code cave range [0x16798, 0x167e8]
    rela_text_off = 0x1fe5e0
    rela_text_sz = 0x27f0d8
    cleared = 0
    for i in range(0, rela_text_sz, 24):
        entry_off = rela_text_off + i
        r_off, r_info, r_addend = struct.unpack("<QQq", data[entry_off : entry_off + 24])
        if 0x16798 <= r_off < 0x167e8:
            struct.pack_into("<Qq", data, entry_off + 8, 0, 0)
            cleared += 1
    print(f"[+] Step 3 (Code Cave): Cleared {cleared} legacy relocations in dumpMemory8 cave.")

    # 2. Helper functions for AArch64 opcode encoding
    def encode_cbz_x(src, dst, reg=0):
        diff = (dst - src) // 4
        val = 0xb4000000 | ((diff & 0x7ffff) << 5) | (reg & 0x1f)
        return val.to_bytes(4, "little")

    def encode_b(src, dst):
        diff = (dst - src) // 4
        return (0x14000000 | (diff & 0x03ffffff)).to_bytes(4, "little")

    def encode_bl(src, dst):
        diff = (dst - src) // 4
        return (0x94000000 | (diff & 0x03ffffff)).to_bytes(4, "little")

    def encode_adr(src, dst, reg=1):
        diff = dst - src
        immlo = (diff & 3) << 29
        immhi = ((diff >> 2) & 0x7ffff) << 5
        val = 0x10000000 | immlo | immhi | (reg & 0x1f)
        return val.to_bytes(4, "little")

    # 3. Assemble Code Cave Routine at 0x16798
    cave = bytearray()
    cave += bytes.fromhex("c0035fd6")               # 0x16798: ret (dumpMemory8 entry for other callers)
    
    # 0x1679c: Code Cave Entry (called ONLY from wlanNetCreate at 0x670a0)
    cave += bytes.fromhex("fd7bbea9")               # 0x1679c: stp x29, x30, [sp, #-32]!
    cave += bytes.fromhex("f35701a9")               # 0x167a0: stp x19, x21, [sp, #16]
    cave += bytes.fromhex("b3835ef8")               # 0x167a4: ldur x19, [x29, #-0x18] (prGlueInfo)
    cave += encode_cbz_x(0x167a8, 0x167c0, 19)      # 0x167a8: cbz x19, 0x167c0 (skip if null)
    cave += bytes.fromhex("601240f9")               # 0x167ac: ldr x0, [x19, #0x20] (wlan0)
    cave += encode_cbz_x(0x167b0, 0x167c0, 0)       # 0x167b0: cbz x0, 0x167c0 (skip if null)
    cave += encode_adr(0x167b4, 0x167d8, 1)         # 0x167b4: adr x1, 0x167d8 ("MONITOR 1 1 20 0")
    cave += bytes.fromhex("02028052")               # 0x167b8: mov w2, #16
    cave += encode_bl(0x167bc, 0x8ad24)             # 0x167bc: bl priv_driver_set_monitor (0x8ad24)
    cave += bytes.fromhex("f35741a9")               # 0x167c0: ldp x19, x21, [sp, #16]
    cave += bytes.fromhex("fd7bc2a8")               # 0x167c4: ldp x29, x30, [sp], #32
    cave += encode_b(0x167c8, 0x670f4)              # 0x167c8: b 0x670f4 (return to wlanNetCreate)
    cave += bytes.fromhex("1f2003d5")               # 0x167cc: nop
    cave += bytes.fromhex("1f2003d5")               # 0x167d0: nop
    cave += bytes.fromhex("1f2003d5")               # 0x167d4: nop
    cave += b"MONITOR 1 1 20 0\x00\x00\x00\x00"     # 0x167d8: command string (20 bytes)

    # Write cave bytes into .text at 0x44 + 0x16798
    cave_file_offset = 0x44 + 0x16798
    data[cave_file_offset : cave_file_offset + len(cave)] = cave
    print(f"[+] Step 3 (Code Cave): Injected {len(cave)}-byte trampoline at .text + 0x16798.")

    # 4. Hook wlanNetCreate at 0x670a0 to branch to Code Cave Entry (0x1679c)
    hook_file_offset = 0x44 + 0x670a0
    data[hook_file_offset : hook_file_offset + 4] = encode_b(0x670a0, 0x1679c)
    print("[+] Step 3 (Hook): Hooked wlanNetCreate (0x670a0 -> 0x1679c).")
    return data

def apply_step4_rx_promisc_unlock(data):
    """
    Step 4: Hardware Promiscuous RX & Packet Filter Unlock
    1. In wlanoidSetCurrentPacketFilter (0x32bf0):
       Bypass 'cmp w22, #0x10' promiscuous mode rejection.
       Replace 0x32bf0 with 'b 0x32c38' (12 00 00 14) and clear 9 dead relocations.
    2. In nicRxProcessRFBs (0x4fca8):
       Bypass packet type whitelist check ('cmp w8, #2' / 'b.ne 0x4fcfc').
       Replace 'b.ne 0x4fcfc' at 0x4fca8 with NOP (1f 20 03 d5) so all frames
       flow into nicRxProcessMonitorPacket when monitor mode is enabled.
    """
    # 1. Bypass packet filter check
    filter_bypass_offset = 0x44 + 0x32bf0
    data[filter_bypass_offset : filter_bypass_offset + 4] = bytes.fromhex("12000014") # b 0x32c38
    print("[+] Step 4: Patched wlanoidSetCurrentPacketFilter (0x32bf0 -> b 0x32c38).")

    # Clear 9 dead relocations in [0x32bf0, 0x32c38)
    rela_offset = 0x1fe5e0
    rela_size = 0x27f0d8
    num_entries = rela_size // 24
    cleared = 0
    for i in range(num_entries):
        entry = data[rela_offset + i*24 : rela_offset + (i+1)*24]
        r_offset, r_info, r_addend = struct.unpack("<QQq", entry)
        if 0x32bf0 <= r_offset < 0x32c38:
            data[rela_offset + i*24 : rela_offset + (i+1)*24] = b"\x00" * 24
            cleared += 1
    print(f"[+] Step 4: Cleared {cleared} dead relocations in packet filter check.")

    # 2. Allow all packet types into monitor handler when monitor mode is active
    rfb_check_offset = 0x44 + 0x4fca8
    data[rfb_check_offset : rfb_check_offset + 4] = bytes.fromhex("1f2003d5") # nop
    print("[+] Step 4: Patched nicRxProcessRFBs (0x4fca8 -> NOP) to forward all packet types to mon0.")

    return data

def main():
    print("=" * 60)
    print(" MediaTek wlan_drv_gen4m Warp Patcher")
    print("=" * 60)
    
    if not os.path.exists(STOCK_KO):
        print(f"[-] Stock driver not found: {STOCK_KO}")
        sys.exit(1)
        
    with open(STOCK_KO, "rb") as f:
        data = bytearray(f.read())
        
    verify_stock_driver(data)
    
    # Apply Step 1: Interface name -> mon%d
    data = apply_step1_rename(data, b"mon%d\x00\x00\x00\x00\x00\x00")
    
    # Apply Step 2: TX Packet Injection Wiring (.ndo_start_xmit = wlanHardStartXmit + carrier + TX queues)
    data = apply_step2_tx_wiring(data)
    
    # Apply Step 3: Code Cave Trampoline Auto-Spawn (wlanNetCreate -> dumpMemory8 -> priv_driver_set_monitor)
    data = apply_step3_code_cave_autospawn(data)
    
    # Apply Step 4: Hardware Promiscuous RX & Packet Filter Unlock
    data = apply_step4_rx_promisc_unlock(data)
    
    with open(OUTPUT_KO, "wb") as f:
        f.write(data)
        
    print(f"[+] Warped driver saved to: {OUTPUT_KO} ({len(data)} bytes)")

if __name__ == "__main__":
    main()


