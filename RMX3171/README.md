# MediaTek Helio G85 (MT6768) Native Monitor Mode & Raw Packet Injection

Reverse-engineered implementation of native 802.11 monitor mode (`mon0`) and Over-The-Air (OTA) raw packet injection for the MediaTek Helio G85 platform (`wlan_drv_gen4m.ko`, Linux 4.14.186 AArch64, Realme Narzo 30A / `RMX3171`).

Tested and verified with an external physical hardware sniffer (ESP32 listening on 2.4GHz RF).

---

## Repository Structure

```
.
├── boot_sig_patcher.py          # Android boot.img kernel module signature verification bypass
├── driver-warp/                 # Automated binary patcher and code cave injection engine
│   ├── patcher.py               # Complete AArch64 instruction patcher and assembler
│   └── test_deploy.sh           # Hot-reload validation script
├── esp32-sniffer/               # ESP32 hardware ground-truth RF sniffer (PlatformIO)
│   ├── platformio.ini           # PlatformIO configuration (115200 baud)
│   └── src/main.cpp             # Promiscuous 802.11 packet sniffer firmware
├── magisk-module/               # Flashable Magisk module tree
│   ├── module.prop              # Module metadata (v1.3.0-alpha)
│   ├── customize.sh             # Magisk installer script
│   ├── build_module.py          # Module packager & SHA256 generator
│   └── system/
│       ├── bin/airmon-mtk       # Management CLI tool for monitor mode & channels
│       └── vendor/lib/modules/  # Target location for warped wlan_drv_gen4m.ko
├── drivers-backup-stock/        # Unmodified factory OEM driver backup
├── wlan_drv_gen4m_warped.ko.enc # Encrypted production kernel driver (AES-256-CBC)
├── mtk-aircrack-core-v1.3.0-alpha.zip # Flashable Magisk release package
└── mtk-aircrack-core-v1.3.0-alpha.zip.sha256
```

---

## Encrypted Driver Access

The latest production driver with full raw injection functionality is stored in:
`wlan_drv_gen4m_warped.ko.enc`

### Decryption Key
```
RealmeNarzo30A_MT6768_WarpedKernel2026!
```

### Decryption Command
```bash
openssl enc -d -aes-256-cbc -pbkdf2 -iter 100000 \
  -in wlan_drv_gen4m_warped.ko.enc \
  -out wlan_drv_gen4m_warped.ko \
  -pass pass:RealmeNarzo30A_MT6768_WarpedKernel2026!
```

### Verification
Verify the SHA256 hash matches the validated build:
```bash
sha256sum wlan_drv_gen4m_warped.ko
# Expected: 4a25694d50f085b80852d91485059906ed39b221e92c14c3aa6e2b683265ac0d
```

---

## Technical Summary of Binary Patches

The patcher injects assembly trampolines and modifies MediaTek's driver logic in `wlan_drv_gen4m.ko`:

1. **Monitor Net Device Creation & Renaming:**
   - Patches `radiotap%d` string to `mon%d` (`0x17106f`).
   - Hooks `wlanNetCreate` (`0x670a0`) to automatically configure the monitor interface via code cave trampoline (`0x16798`).
2. **TX Transmission Wiring:**
   - Overwrites `netif_carrier_off` (`0x65a18`) with `netif_carrier_on`.
   - NOPs `netif_tx_stop_all_queues` (`0x65a20`).
   - Binds `mon0` ops directly to `wlanHardStartXmit` (`0x65840`).
3. **Hardware Promiscuous RX Unlock:**
   - Bypasses `wlanoidSetCurrentPacketFilter` (`0x32bf0 -> b 0x32c38`).
   - NOPs frame filter checks in `nicRxProcessRFBs` (`0x4fca8`) to forward all packet types to userland.
4. **Code Cave 1 (0x16800 - Radiotap Stripping & Classification):**
   - Intercepts incoming `ARPHRD_IEEE80211_RADIOTAP` skb frames in `kalHardStartXmit` (`0x6f178`).
   - Detects and strips userland radiotap headers.
   - Preserves `ucBssIndex = 0` in `skb->cb[0]` and sets `skb->cb[1] = 1` as the internal injection flag.
5. **Code Cave 2 (0x16860 - Descriptor Formulation & Safe TxDone):**
   - Intercepts MSDU allocation in `nicTxFillMsduInfo` (`0x475cc`).
   - Sets `ucPacketType = 3` (`TX_PACKET_TYPE_MGMT`).
   - Sets `fgIs802_11 = 1`, `ucStaRecIndex = 0xff` (unassociated), `u2PayloadLength`, and `ucFormat = FORMAT_802_11_NORMAL (2)`.
   - Attaches a custom `pfTxDoneHandler` (`my_txdone`) to safely release the SKB memory and prevent kernel panics.
6. **Queue Manager Unassociated Transmission Bypass:**
   - NOPs inactive BSS drop in `qmEnqueueTxPackets` (`0x5492c`, `0x54934`).
   - Redirects unassociated packets directly to `rBmTxQueue` (`0x54988 -> b.eq 0x54994`).
   - Forces `qmGetFrameAction` to authorize frame transmission (`0x5a8ec -> mov w24, #2; b 0x5ab60`).
   - NOPs inactive BSS discard in `qmDequeueTxPacketsFromGlobalQueue` (`0x55c98 -> NOP`), delivering packets to the physical DMA transmission queue.

---

## Usage Instructions

### Quick Start with Magisk
1. Flash `mtk-aircrack-core-v1.3.0-alpha.zip` in Magisk Manager.
2. Reboot the phone.
3. Open a root terminal (Termux / ADB) and run:
   ```bash
   # Enable monitor mode on Channel 11
   airmon-mtk start 11

   # View status
   airmon-mtk status

   # Switch channel dynamically
   airmon-mtk channel 6

   # Disable monitor mode
   airmon-mtk stop
   ```

### Packet Capture & Injection
With `mon0` active:
```bash
# Capture raw frames
tcpdump -i mon0 -n -e -vv

# Transmit raw frames via aireplay-ng
aireplay-ng -D --deauth 5 -c <TARGET_MAC> -a <AP_BSSID> mon0
```
