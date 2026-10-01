# MediaTek Gen4m Driver Warp: Monitor Mode & Packet Injection

**Target Device:** Realme Narzo 30A (`RMX3171`, MT6768 Helio G85)  
**Kernel:** Linux 4.14.186 AArch64  
**Target Driver:** `wlan_drv_gen4m.ko` (Closed-Source MediaTek Wi-Fi Vendor Driver)  
**Reverse Engineered & Warped by:** Shado & Antigravity (2026)

---

## 1. Overview & Architectural Breakthrough

Historically, MediaTek mobile Wi-Fi drivers on Android devices were considered incapable of raw packet transmission (`inj`) and standard monitor mode (`mon0`) due to closed-source kernel driver design, missing `mac80211` integration, and firmware-level constraints.

By reverse engineering the closed-source ELF64 relocatable binary (`wlan_drv_gen4m.ko`), we identified internal code paths, relocation structures, and code caves that allowed us to transform the stock vendor driver into a fully functional, standard-compliant Linux wireless interface supporting both **raw monitor mode promiscuous capture** and **arbitrary 802.11 frame packet injection**.

---

## 2. Reverse Engineering & Binary Warping Steps

### Step 1: Interface Renaming (`radiotap0` -> `mon0`)
- **Root Cause:** MediaTek hardcoded the monitor network device name to `"radiotap%d\0"` in `.rodata.str1.1` at file offset `0x17106f`. Standard desktop auditing utilities (`airodump-ng`, `aireplay-ng`, `airgeddon`, `scapy`, `wireshark`) expect standard interface naming conventions like `mon%d` or `wlan%dmon`.
- **Patch:** Replaced `radiotap%d\0` with `mon%d\0\0\0\0\0\0` (11 bytes null-padded). The kernel name allocator `dev_alloc_name()` cleanly registers the interface as `mon0` with link type `ARPHRD_IEEE80211_RADIOTAP`.

### Step 2: Hardware DMA Packet Injection Wiring (`.ndo_start_xmit`)
- **Root Cause Analysis:**
  1. In stock code, `wlanMonWorkHandler` assigned `dummy_netdev_ops` (`.rodata + 0x1580`) which set `.ndo_start_xmit = NULL`.
  2. The driver explicitly called `netif_carrier_off()` at offset `0x65a18` and `netif_tx_stop_all_queues()` at `0x65a20`.
  3. Because carrier was OFF (`NO-CARRIER`) and queues were stopped (`XOFF`), the Linux kernel packet scheduler dropped any raw frames injected via `AF_PACKET` / `SOCK_RAW` sockets before the driver could ever be invoked.
- **The Fix:**
  - **Relocation Table Rewrite:** Updated 4 relocation records in `.rela.text` (`0x65a00..0x65a0c`) pointing `mon0`'s `net_device_ops` to `wlan_netdev_ops` (`.rodata + 0x1780`), which wires `.ndo_start_xmit` directly to MediaTek's hardware DMA transmit engine `wlanHardStartXmit` (`0x65840`).
  - **Carrier Unblocked:** Changed relocation at `0x65a18` from `netif_carrier_off` (symbol 1866) to `netif_carrier_on` (symbol 2127). `mon0` now spawns with `carrier = 1` and `LOWER_UP`.
  - **TX Queues Unstopped:** Cleared relocation at `0x65a20` and replaced the instruction with an AArch64 `nop` (`0xd503201f`). All 4 multi-queues (`qdisc mq`) stay active and running.

### Step 3: Code Cave Architecture & Collision Neutralization
- **Discovery:** In `.text`, `dumpMemory8` (`0x16798`) is a 1,252-byte debug function. However, 41 internal driver routines call `dumpMemory8` during AP association and packet parsing.
- **The Fix:**
  - Inserted an immediate AArch64 `ret` (`0xd65f03c0`) at `0x16798`, instantly neutralizing all 41 debug call sites with zero overhead and zero crash potential.
  - Cleared 9 legacy `printk` relocations in `.rela.text` across `[0x16798, 0x167e8]`.
  - Utilized `0x1679c` as an isolated code cave for trampoline routines.

---

## 3. Verification & Live Benchmarks

### Promiscuous 802.11 Monitor Capture
- **Interface:** `mon0`
- **Link Type:** `link/ieee802.11/radiotap`
- **Rx Packets:** Over 2,800 frames captured in real time (Beacons, RTS/CTS, Probe Requests, QOS Data) with 0 dropped frames.

### Raw 802.11 Packet Injection
- **Test 1 (`aireplay-ng --test mon0`):**
  - Transmitted 6 broadcast/targeted probe request frames (252 bytes) directly to the air.
- **Test 2 (`AF_PACKET / SOCK_RAW` Python):**
  - Transmitted 32-byte 802.11 Management Probe Request frame targeting testbed AP (`ea:f6:02:30:b6:99` / `Redmi 14C 5G`) and testbed client (`ae:b4:9e:36:fe:ca`).
- **Kernel Interface Counters (`ip -s link show mon0`):**
  ```
  TX: bytes 284   packets 7   errors 0   dropped 0   carrier 0   collsns 0
  ```
- **Driver Hardware Interface Log (`dmesg`):**
  ```
  In HIF ISR.
  kalDumpHifStats: I[141 0] T[130 130 129 / 0 0 0 0] R[51 / 39]
  ```
  MediaTek HIF Interrupt Service Routine and DMA transmit queues processed all 7 frames out to the air with zero drops and zero errors.

---

## 4. Repository Artifacts

- [`patcher.py`](file:///home/shado/Documents/realme-reverse-engineering/RMX3171/driver-warp/patcher.py): Fully automated binary patcher applying Step 1, Step 2, and Step 3.
- [`mtk-aircrack-core-v1.1.0.zip`](file:///home/shado/Documents/realme-reverse-engineering/RMX3171/mtk-aircrack-core-v1.1.0.zip): Deployable Magisk module containing the warped driver overlay, native monitor mode (`mon0`), raw injection engine, and desktop CLI shims (`airmon-mtk`, `airmon-ng`, `iw`).

