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

def apply_step5_raw_tx_injection(data):
    """
    Step 5: Raw 802.11 Packet Injection Over The Air
    1. In wlanProcessTxFrame (0x1e424):
       Replace 'b.eq 0x1e44c' (drop on non-Ethernet frames) with NOP (1f 20 03 d5).
       Allows raw 802.11 management/control/data frames to pass classification.
    2. In kalHardStartXmit (0x6f190):
       Replace 'b.eq 0x6f1ec' (drop on classification error) with NOP (1f 20 03 d5).
       Ensures raw frames proceed straight to the HIF TX queue.
    3. Clear relocations in range [0x16800, 0x16900] for Code Caves 1 & 2.
    4. Code Cave 1 at 0x16800 (kalHardStartXmit Trampoline):
       - Keeps ucBssIndex = 0 in [x20, #0x38] (CRITICAL: prevents kernel data abort in kalSendCompleteAndAwakeQueue).
       - Identifies mon0 via prDev->type == 0x323 (ARPHRD_IEEE80211_RADIOTAP at [x22, #0x234]).
       - Automatically strips userland Radiotap header if present.
       - Marks skb->cb[1] ([x20, #0x39]) = 1 as mon0 injection flag.
       - Hooked at 0x6f178 (b 0x16800).
    5. Code Cave 2 at 0x16860 (nicTxFillMsduInfo Direct Hardware TX Routing):
       - Checks skb->cb[1] ([x20, #0x39]) == 1.
       - Sets ucPacketType = 3 (TX_PACKET_TYPE_MGMT), bypassing Queue Manager drop.
       - Sets fgIs802_11 = 1, ucStaRecIndex = 0xff, ucBssIndex = 0.
       - Sets ucMacHeaderLength = 24, u2PayloadLength = skb->len.
       - Sets ucFormat = FORMAT_802_11_NORMAL (2).
       - Hooked at 0x47680 (b 0x16860).
    """
    # 1. Bypass kalQoSFrameClassifier drop in wlanProcessTxFrame (0x1e424 -> NOP)
    tx_classifier_offset = 0x44 + 0x1e424
    data[tx_classifier_offset : tx_classifier_offset + 4] = bytes.fromhex("1f2003d5")
    print("[+] Step 5: Patched wlanProcessTxFrame (0x1e424 -> NOP) to allow raw 802.11 frames.")

    # 2. Bypass drop branch in kalHardStartXmit (0x6f190 -> NOP)
    tx_drop_offset = 0x44 + 0x6f190
    data[tx_drop_offset : tx_drop_offset + 4] = bytes.fromhex("1f2003d5")
    print("[+] Step 5: Patched kalHardStartXmit (0x6f190 -> NOP) to forward raw frames to HIF TX queue.")

    # 3. Clear dead relocations in [0x16800, 0x16900]
    rela_text_off = 0x1fe5e0
    rela_text_sz = 0x27f0d8
    cleared = 0
    for i in range(0, rela_text_sz, 24):
        entry_off = rela_text_off + i
        r_off, r_info, r_addend = struct.unpack("<QQq", data[entry_off : entry_off + 24])
        if 0x16800 <= r_off < 0x16900:
            struct.pack_into("<Qq", data, entry_off + 8, 0, 0)
            cleared += 1
    print(f"[+] Step 5 (Code Caves): Cleared {cleared} legacy relocations in dumpMemory8 cave range [0x16800-0x16900].")

    # AArch64 opcode encoders
    def ldrh_imm(rt, rn, imm):
        return (0x79400000 | ((imm >> 1) << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def strh_imm(rt, rn, imm):
        return (0x79000000 | ((imm >> 1) << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def ldrb_imm(rt, rn, imm):
        return (0x39400000 | (imm << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def strb_imm(rt, rn, imm):
        return (0x39000000 | (imm << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def ldr_x_imm(rt, rn, imm):
        return (0xf9400000 | ((imm >> 3) << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def str_x_imm(rt, rn, imm):
        return (0xf9000000 | ((imm >> 3) << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def ldr_w_imm(rt, rn, imm):
        return (0xb9400000 | ((imm >> 2) << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def str_w_imm(rt, rn, imm):
        return (0xb9000000 | ((imm >> 2) << 10) | (rn << 5) | rt).to_bytes(4, 'little')
    def movz_w(rt, imm):
        return (0x52800000 | (imm << 5) | rt).to_bytes(4, 'little')
    def cmp_w_imm(rn, imm):
        return (0x71000000 | (imm << 10) | (rn << 5) | 0x1f).to_bytes(4, 'little')
    def b_cond(src, dst, cond):
        diff = (dst - src) >> 2
        return (0x54000000 | ((diff & 0x7ffff) << 5) | cond).to_bytes(4, 'little')
    def b_imm(src, dst):
        diff = (dst - src) >> 2
        return (0x14000000 | (diff & 0x03ffffff)).to_bytes(4, 'little')
    def add_x_reg(rd, rn, rm):
        return (0x8b000000 | (rm << 16) | (rn << 5) | rd).to_bytes(4, 'little')
    def sub_w_reg(rd, rn, rm):
        return (0x4b000000 | (rm << 16) | (rn << 5) | rd).to_bytes(4, 'little')
    def cbnz_w(rt, src, dst):
        diff = (dst - src) >> 2
        return (0x35000000 | ((diff & 0x7ffff) << 5) | rt).to_bytes(4, 'little')

    # 4. Assemble Cave 1 (kalHardStartXmit at 0x16800)
    cave1_insns = [
        ('entry', strb_imm(21, 20, 0x38)),           # strb w21, [x20, #0x38] (stock ucBssIndex = 0)
        (None, ldrh_imm(8, 22, 0x234)),              # ldrh w8, [x22, #0x234] (prDev->type)
        (None, cmp_w_imm(8, 0x323)),                 # cmp w8, #0x323 (ARPHRD_IEEE80211_RADIOTAP)
        (None, lambda pc: b_cond(pc, labels1['normal_hif'], 1)), # b.ne normal_hif
        (None, ldr_x_imm(9, 20, 0xf8)),              # ldr x9, [x20, #0xf8] (skb->data)
        (None, ldrh_imm(10, 9, 0)),                  # ldrh w10, [x9] (it_version, it_pad)
        (None, lambda pc: cbnz_w(10, pc, labels1['is_raw'])),    # cbnz w10, is_raw
        (None, ldrh_imm(11, 9, 2)),                  # ldrh w11, [x9, #2] (it_len)
        (None, cmp_w_imm(11, 4)),                    # cmp w11, #4
        (None, lambda pc: b_cond(pc, labels1['is_raw'], 3)),     # b.lo is_raw
        (None, add_x_reg(9, 9, 11)),                 # add x9, x9, x11 (skb->data += it_len)
        (None, str_x_imm(9, 20, 0xf8)),              # str x9, [x20, #0xf8]
        (None, ldr_w_imm(12, 20, 0xa0)),             # ldr w12, [x20, #0xa0] (skb->len)
        (None, sub_w_reg(12, 12, 11)),               # sub w12, w12, w11 (skb->len -= it_len)
        (None, str_w_imm(12, 20, 0xa0)),             # str w12, [x20, #0xa0]
        ('is_raw', movz_w(8, 1)),                    # mov w8, #1 (mon0 marker flag)
        (None, strb_imm(8, 20, 0x39)),               # strb w8, [x20, #0x39] (skb->cb[1] = 1)
        (None, lambda pc: b_imm(pc, labels1['hif_done'])),       # b hif_done
        ('normal_hif', strb_imm(31, 20, 0x39)),      # strb wzr, [x20, #0x39] (skb->cb[1] = 0 for wlan0)
        ('hif_done', bytes.fromhex('fa031baa')),     # mov x26, x27 (stock instruction from 0x6f17c)
        (None, lambda pc: b_imm(pc, 0x6f180))        # b 0x6f180 (return to kalHardStartXmit)
    ]
    labels1 = {}
    pc = 0x16800
    for label, item in cave1_insns:
        if label:
            labels1[label] = pc
        pc += 4
    cave1_bytes = bytearray()
    pc = 0x16800
    for label, item in cave1_insns:
        if callable(item):
            cave1_bytes += item(pc)
        else:
            cave1_bytes += item
        pc += 4

    data[0x44 + 0x16800 : 0x44 + 0x16800 + len(cave1_bytes)] = cave1_bytes
    print(f"[+] Step 5: Injected Cave 1 ({len(cave1_bytes)} bytes) at 0x16800.")

    # Hook kalHardStartXmit at 0x6f178: b 0x16800
    hook1 = b_imm(0x6f178, 0x16800)
    data[0x44 + 0x6f178 : 0x44 + 0x6f178 + len(hook1)] = hook1
    print("[+] Step 5: Hooked kalHardStartXmit (0x6f178 -> 0x16800).")

    # 5. Assemble Cave 2 (nicTxFillMsduInfo at 0x16860)
    cave2_insns = [
        ('entry', ldrb_imm(8, 20, 0x39)),            # ldrb w8, [x20, #0x39] (check mon0 marker)
        (None, cmp_w_imm(8, 1)),                     # cmp w8, #1
        (None, lambda pc: b_cond(pc, labels2['normal_msdu'], 1)), # b.ne normal_msdu
        (None, movz_w(8, 1)),                        # mov w8, #1
        (None, strb_imm(8, 19, 0x1e)),               # strb w8, [x19, #0x1e] (fgIs802_11 = 1)
        (None, strb_imm(8, 19, 0x25)),               # strb w8, [x19, #0x25] (fgIs802_11 = 1)
        (None, strb_imm(8, 19, 0x38)),               # strb w8, [x19, #0x38] (ucControlFlag = MSDU_CONTROL_FLAG_FORCE_TX = 1)
        (None, movz_w(8, 0xff)),                     # mov w8, #0xff
        (None, strb_imm(8, 19, 0x1f)),               # strb w8, [x19, #0x1f] (ucStaRecIndex = 0xff)
        (None, movz_w(8, 0)),                        # mov w8, #0
        (None, strb_imm(8, 19, 0x20)),               # strb w8, [x19, #0x20] (ucBssIndex = 0)
        (None, movz_w(8, 0x18)),                     # mov w8, #0x18 (24 bytes)
        (None, strb_imm(8, 19, 0x41)),               # strb w8, [x19, #0x41] (ucMacHeaderLength = 24)
        (None, ldr_w_imm(8, 20, 0xa0)),              # ldr w8, [x20, #0xa0] (skb->len)
        (None, strh_imm(8, 19, 0x44)),               # strh w8, [x19, #0x44] (u2PayloadLength = skb->len)
        (None, movz_w(8, 2)),                        # mov w8, #2
        (None, strb_imm(8, 19, 0x6c)),               # strb w8, [x19, #0x6c] (ucFormat = FORMAT_802_11_NORMAL)
        (None, lambda pc: b_imm(pc, 0x476f4)),       # b 0x476f4 (skip wlanPktTxDone, use nicTxDummyTxDone!)
        ('normal_msdu', ldrb_imm(8, 19, 0x6c)),      # ldrb w8, [x19, #0x6c]
        (None, lambda pc: b_imm(pc, 0x47684))        # b 0x47684
    ]
    labels2 = {}
    pc = 0x16860
    for label, item in cave2_insns:
        if label:
            labels2[label] = pc
        pc += 4
    cave2_bytes = bytearray()
    pc = 0x16860
    for label, item in cave2_insns:
        if callable(item):
            cave2_bytes += item(pc)
        else:
            cave2_bytes += item
        pc += 4

    data[0x44 + 0x16860 : 0x44 + 0x16860 + len(cave2_bytes)] = cave2_bytes
    print(f"[+] Step 5: Injected Cave 2 ({len(cave2_bytes)} bytes) at 0x16860.")

    # Hook nicTxFillMsduInfo at 0x47680: b 0x16860
    hook2 = b_imm(0x47680, 0x16860)
    data[0x44 + 0x47680 : 0x44 + 0x47680 + len(hook2)] = hook2
    print("[+] Step 5: Hooked nicTxFillMsduInfo (0x47680 -> 0x16860).")

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
    
    # Apply Step 5: Raw 802.11 Packet Injection Over The Air
    data = apply_step5_raw_tx_injection(data)
    
    with open(OUTPUT_KO, "wb") as f:
        f.write(data)
        
    print(f"[+] Warped driver saved to: {OUTPUT_KO} ({len(data)} bytes)")

if __name__ == "__main__":
    main()


