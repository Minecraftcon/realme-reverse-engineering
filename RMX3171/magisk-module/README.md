# MTK Aircrack Core

**Deployable Magisk Module for MediaTek Helio G85 (MT6768 / Linux 4.14.186 AArch64)**  
*Reverse-engineered & developed by Shado & Antigravity (2026)*

---

## 1. Overview

**MTK Aircrack Core** converts closed-source Android MediaTek Wi-Fi drivers (`wlan_drv_gen4m.ko`) into a desktop-grade penetration testing engine supporting:
- Native Monitor Mode (`mon0`, link-type `IEEE802_11_RADIO / radiotap`)
- Full Promiscuous Sniffing (Beacons, Probes, Data, Control, Management frames)
- Hardware DMA Raw 802.11 Packet Injection Engine (zero dropped frames, direct HIF ISR dispatch)
- On-the-fly Channel Switching & Background Channel Hopping (`1..13`)
- Drop-in Desktop CLI Compatibility (`airmon-ng`, `airmon-mtk`, `iw`)

---

## 2. Module Structure

```
mtk-aircrack-core-v1.1.0.zip
├── META-INF/
│   └── com/google/android/
│       ├── update-binary         # Universal Magisk / Recovery installer stub
│       └── updater-script        # #MAGISK tag
├── customize.sh                  # Architecture check & permission setup
├── module.prop                   # Module metadata (v1.1.0 / id: mtk-aircrack-core)
├── service.sh                    # Late_start service & lock cleanup
└── system/
    ├── bin/
    │   ├── airmon-mtk            # Multi-channel monitor mode & hopper daemon
    │   ├── airmon-ng             # Aircrack-ng desktop shim
    │   ├── iw                    # Netlink/OID translation wrapper
    │   ├── iw.real               # Genuine AArch64 iw binary
    │   └── bash -> /system_ext/bin/bash
    └── vendor/
        └── lib/modules/
            └── wlan_drv_gen4m.ko # Warped MediaTek driver payload
```

---

## 3. Building the Module

To rebuild or repackage the zip with SHA256 checksums:

```bash
python3 /home/shado/Documents/realme-reverse-engineering/RMX3171/magisk-module/build_module.py
```

Output:
- `mtk-aircrack-core-v1.1.0.zip`
- `mtk-aircrack-core-v1.1.0.zip.sha256`

---

## 4. Installation

### Option A: From Magisk App (GUI)
1. Copy `mtk-aircrack-core-v1.1.0.zip` to phone storage (`/sdcard/Download/`).
2. Open the **Magisk App** -> Modules -> **Install from storage**.
3. Select `mtk-aircrack-core-v1.1.0.zip` and tap Reboot.

### Option B: From ADB / Terminal (CLI)
```bash
adb push mtk-aircrack-core-v1.1.0.zip /data/local/tmp/
adb shell "su -c 'magisk --install-module /data/local/tmp/mtk-aircrack-core-v1.1.0.zip'"
adb reboot
```

---

## 5. Usage in Termux or Root Shell

### Start Monitor Mode
```bash
# Bring up mon0 on Channel 1
su -c 'airmon-mtk start 1'

# Or on Channel 6
su -c 'airmon-mtk start 6'
```

### Channel Hopping & Sniffing
```bash
# Start background channel hopper (0.3s delay)
su -c 'airmon-mtk hop 0.3'

# Sniff live packets in real-time
su -c 'tcpdump -i mon0 -n -e -vv'

# Capture to PCAP
su -c 'tcpdump -i mon0 -s 0 -w /sdcard/capture.pcap'
```

### Raw Packet Injection Test
```bash
# Test frame injection via aireplay-ng
su -c 'aireplay-ng --test mon0'
```

### Stop Monitor Mode
```bash
su -c 'airmon-mtk stop'
```
