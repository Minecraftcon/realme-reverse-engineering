# Realme Narzo 30A (RMX3171) Wi-Fi Capabilities & Driver Reimplementation Checklist

**Target Hardware:** MediaTek MT6768 (Helio G85)  
**Kernel Module:** `wlan_drv_gen4m.ko`  
**Host Architecture:** `aarch64` (Android 11 / Realme UI 2.0)  
**Objective:** Reverse engineer, test, and document all hardware, driver, and interface capabilities to build a fully standalone Wi-Fi penetration & management framework on Android.

---

## Progress Overview

- [x] **1. Monitor Mode on 2.4 GHz Band**
- [x] **2. Monitor Mode on 5 GHz Band (Channels 36–165, 20/40/80 MHz)**
- [x] **3. Automated Channel Hopping Engine**
- [x] **4. Multiple AP Interface Creation (`ap0`, `ap1`, `ap2`)**
- [x] **5. Dual-Band Concurrent SoftAP (DBDC: 2.4GHz + 5GHz simultaneous)**
- [x] **6. Wi-Fi Repeater (STA + AP Concurrency)**
- [x] **7. MediaTek Diagnostic Backdoors & Hardware Commands (`priv_driver_cmds`)**
- [x] **8. Native Network & Routing Daemons (`dnsmasq`, `iptables`, `tcpdump`)**

---

## Detailed Test Logs & Execution Notes

### 1. Monitor Mode on 2.4 GHz Band
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  Standard `nl80211` monitor mode (`iw dev wlan0 interface add mon0 type monitor`) fails with `-EOPNOTSUPP` (-95) because MediaTek removed `BIT(NL80211_IFTYPE_MONITOR)` from `wiphy->interface_modes`.  
  Triggered via proprietary driver command:
  ```bash
  wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 1 <channel> 20 0"
  ip link set radiotap0 up
  ```
- **Observed Behavior:**  
  Driver registers an `ARPHRD_IEEE80211_RADIOTAP` interface named `radiotap0`. Captured raw 802.11 frames with signal strength (-dBm), channel frequency, and antenna data via `tcpdump -i radiotap0 -n -e -vv`. Verified by capturing live beacons from Redmi 14C 5G (Channel 5) and JioPrivateNet (Channel 6).

---

### 2. Monitor Mode on 5 GHz Band (VHT80 / Channels 36–165)
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  The RF transceiver supports Band A (5GHz) with channel bandwidths of 20MHz, 40MHz, and 80MHz (VHT80):
  ```bash
  # 5GHz UNII-1 Channel 36, 80MHz width:
  wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 1 36 80 0"
  
  # 5GHz UNII-3 Channel 149, 80MHz width:
  wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 1 149 80 0"
  ```
- **Observed Behavior:**  
  `wlanMonWorkHandler: Registered prMonDevHandler context DONE.`  
  `radiotap0` successfully tuned to 5GHz frequencies. Sniffed incoming 802.11 QoS Data frames, Request-To-Send (RTS) control frames, and Probe Responses on 5GHz without kernel panics or desyncs.

---

### 3. Automated Channel Hopping Engine
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  Because `radiotap0` is an unmanaged netdevice, tools like `airodump-ng` cannot change channels via `iw`. Built a background daemon into `airmon-mtk` using `nohup` that cycles channels through a low-latency loop:
  ```bash
  airmon-mtk hop 0.2
  ```
- **Observed Behavior:**  
  Hopper runs in the background (`PID` tracked in `/data/local/tmp/airmon_mtk_hop.pid`) cycling `1 6 11 2 7 12 3 8 13 4 9 5 10`. Hardware PLL locks within milliseconds. Automatically killed when calling `airmon-mtk stop`.

---

### 4. Multiple AP Interface Creation (`ap0`, `ap1`, `ap2`)
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  Probed Linux `cfg80211` and driver limits using `iw phy phy0 interface add`:
  ```bash
  iw phy phy0 interface add ap1 type __ap
  iw phy phy0 interface add ap2 type __ap
  ```
- **Observed Behavior:**  
  The kernel module successfully allocated `ap1` (ifindex 43) and `ap2` (ifindex 44) alongside native `ap0` (ifindex 40), each receiving a distinct BSSID MAC address. Concurrency is governed by the chip's `#channels <= 2` limit.

---

### 5. Dual-Band Concurrent SoftAP (DBDC: 2.4GHz + 5GHz)
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  The MediaTek MT6768 chip integrates dual baseband radios capable of operating on 2 channels simultaneously (`#channels <= 2`). Reverse-engineered `cnmSapIsConcurrent` and `cnmDbdcMode == 3` in the driver.  
  Triggered via Android's native Wi-Fi framework:
  ```bash
  cmd wifi start-softap <SSID> open "" -b bridged
  ```
- **Observed Behavior:**  
  Hotspot started successfully with `MaximumSupportedClientNumber=16`. The hardware radio enables simultaneous broadcast across 2.4GHz (Channels 1–11) and 5GHz (Channels 36–165).

---

### 6. Wi-Fi Repeater (STA + AP Concurrency)
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  Verified from Android dumpsys: `STA + AP Concurrency Supported: true`.  
  The driver maintains client association on `wlan0` while simultaneously hosting an active SoftAP on `ap0`:
  ```bash
  # Check wlan0 and ap0 both active:
  ip link show wlan0
  ip link show ap0
  ```
- **Observed Behavior:**  
  Allows the phone to connect to an existing Wi-Fi network for WAN backhaul while repeating traffic over its own hotspot without requiring cellular data.

---

### 7. MediaTek Diagnostic Backdoors & Hardware Commands (`priv_driver_cmds`)
- **Status:** `[x] COMPLETED & MAPPED`
- **Mechanism:**  
  Disassembled `priv_support_driver_cmd` and extracted the complete function dispatch table from `/proc/kallsyms`:
  - `priv_driver_set_monitor`: Triggers raw 802.11 monitor tap.
  - `priv_driver_set_dbdc`: Configures Dual-Band Dual-Concurrent mode.
  - `priv_driver_set_csa`: Injects Channel Switch Announcement to force connected clients to hop channels.
  - `priv_driver_set_ap_sta_disassoc`: Direct driver-level client disassociation command.
  - `priv_driver_set_ap_set_mac_acl`: Hardware-level MAC address filtering/blacklisting.
  - `priv_driver_set_ap_get_sta_list`: Returns connected station list directly from hardware baseband tables (`WTBL`).
  - `priv_driver_set_mcr` / `priv_driver_set_drv_mcr`: Direct baseband/MAC register read/write (`MCR`).
  - `priv_driver_set_fixed_rate`: Locks TX bitrates (legacy, HT, VHT MCS rates).
- **Observed Behavior:**  
  All commands are dispatched via `wpa_cli driver "<CMD>"`, providing an interface for custom scripting without modifying the binary module.

---

### 8. Native Network & Routing Daemons
- **Status:** `[x] COMPLETED & VERIFIED`
- **Mechanism:**  
  Verified all essential networking, capture, and routing daemons are pre-compiled and native in Android (`/system/bin`):
  - **Packet Sniffer:** `/system/bin/tcpdump` (Full libpcap 802.11 radio support).
  - **DHCP / DNS Server:** `/system/bin/dnsmasq` (v2.51 with IPv6, DHCP, and no-scripts options pre-compiled).
  - **Firewall & NAT:** `/system/bin/iptables` & `/system/bin/ip6tables` (Linux netfilter NAT, forwarding, and redirection).
  - **Access Point Daemon:** `/vendor/bin/hw/hostapd` (v2.10-devel).
- **Observed Behavior:**  
  A standalone Python/Shell framework can run 100% self-contained on this device with zero external package dependencies.

