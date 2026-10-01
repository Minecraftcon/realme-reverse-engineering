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
- [x] **9. Raw Packet Injection Analysis (Monitor Mode vs. `nl80211 mgmt_tx`)**
- [x] **10. Airgeddon Port & Desktop-Style Interface Integration (`wlan0mon`, smart `iw`, `tmux`)**

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

---

### 9. Raw Packet Injection Analysis (Monitor Mode vs. `nl80211 mgmt_tx`)
- **Status:** `[x] COMPLETED & REVERSED`
- **Findings:**
  1. **`radiotap0` Interface is RX-Only (Sniffer Tap):**
     - Tracked down `wlanMonWorkHandler` (`0x65984`) where `radiotap0` is allocated and its `net_device_ops` is assigned at offset `0x1f0`.
     - Parsed the ELF relocation table (`.rela.rodata`):
       ```text
       0x1590 (.rodata + 0x10): ndo_open  -> 0x697a8 (wlanMonNetOpen)
       0x1598 (.rodata + 0x18): ndo_stop  -> 0x69804 (wlanMonNetStop)
       0x15a0 (.rodata + 0x20): ndo_start_xmit -> NULL (0x0)
       ```
     - MediaTek intentionally set `ndo_start_xmit = NULL` for `radiotap0`.
     - Furthermore, `radiotap0` operates with `carrier = 0` (`NO-CARRIER`). Raw frames sent to `radiotap0` via `AF_PACKET` socket are dropped by the kernel `qdisc` layer before transmission (`TX: 0 packets`).
  2. **Supported Injection Paths in Hardware/Driver:**
     - **Management Frame Injection via `nl80211` (`NL80211_CMD_FRAME`):**
       MediaTek implements `_mtk_cfg80211_mgmt_tx` (`0xbbdb8`), which connects to the kernel `cfg80211_ops->mgmt_tx` callback. User-space daemons can inject raw 802.11 Action, Probe, Auth, and Deauth frames through `nl80211`.
     - **Driver Disassociation Backdoor:**
       `priv_driver_set_ap_sta_disassoc` allows direct client deauthentication/kick from userspace via `wpa_cli driver "DISASSOC <mac>"`.
  3. **Path to Enable Raw Injection on `radiotap0` (Binary Patch):**
     - Patch `wlan_mon_netdev_ops` at `.rodata + 0x15a0` to point to `wlanHardStartXmit` (or a custom trampoline to `nicTxDirectStartXmit`).
     - In `wlanMonWorkHandler`, call `netif_carrier_on(prMonDevHandler)` to bring the carrier state to `1` (UP).


---

### 10. Airgeddon Port & Desktop-Style Interface Integration
- **Status:** `[x] COMPLETED & VERIFIED ON HARDWARE`
- **Mechanism:**
  - **Toolchain & Binaries Deployed:** Installed `aircrack-ng (3:1.7)`, `libpcap (1.11.0)`, `libnl (3.12.0)`, `pcre (8.45-2)`, `tmux (3.7c-1)`, and `ncurses-utils (6.6)` directly into the device Termux environment.
  - **Smart `iw` Wrapper (`/data/adb/modules/rmx3171-wifi-enhancer/system/bin/iw`):**
    - Intercepts nl80211 commands targeting virtual monitor devices (`wlan0mon`, `radiotap0`, `mon0`).
    - Emulates `iw dev <iface> info` returning `type monitor` and `wiphy 0`.
    - Synthesizes `Interface wlan0mon` into `iw dev` listings when active.
    - Translates standard `iw dev <iface> set channel <ch>` into MediaTek driver OIDs (`MONITOR 1 <ch> 20 0`).
    - Maps `iw <iface> set monitor control` / `iw <iface> set type managed` into `airmon-ng start` / `stop`.
  - **Driver-Aware `airmon-ng` (`system/bin/airmon-ng`):**
    - Uses driver OID dispatch while preserving `wpa_supplicant` for command control.
    - Renames `radiotap0` to standard desktop `wlan0mon`.
    - Outputs standard aircrack-ng notification strings (`(mac80211 monitor mode vif enabled...)` and `(mac80211 station mode vif enabled on [phy0]wlan0)`) so desktop scripts recognize the interface transition.
  - **Airgeddon Patches (`airgeddon.sh`):**
    - `known_arm_compatible_distros`: Added `"Android"` and `"Termux"`.
    - `detect_distro_phase2`: Automatically identifies Android environment via `/system/build.prop` / `/data/data/com.termux`.
    - `check_inside_tmux`: Recognizes existing `$TMUX` environment variable to prevent nested session attachment failures.
    - `check_interface_coherence`: Directly handles transitions between `wlan0` and `wlan0mon` where hardware MAC is `00:00:00:00:00:00`.
    - `select_interface`: Filters out 30+ internal cellular modem (`ccmni*`), IFB, IMQ, and tunnel interfaces, presenting only genuine wireless interfaces (`wlan0`, `wlan1`, `p2p0`, `ap0`, `wlan0mon`).
    - `set_chipset`: Automatically detects MediaTek SoC interfaces as `MediaTek MT6768 (Helio G85 gen4m)`.
    - `.airgeddonrc`: Configured `AIRGEDDON_WINDOWS_HANDLING=tmux`, `AIRGEDDON_AUTO_UPDATE=false`, `AIRGEDDON_FORCE_NETWORK_MANAGER_KILLING=false`.
- **Observed Behavior:**
  - Launching `airgeddon-launcher.sh` passes all 9 essential dependency checks (`iw`, `awk`, `airmon-ng`, `airodump-ng`, `aircrack-ng`, `tmux`, `ip`, `lspci`, `ps`).
  - Displays clean interface selection menu with chipset identification and Wi-Fi 5 band capabilities.
  - Selecting `wlan0` and choosing Option 2 ("Put interface in monitor mode") successfully initializes `wlan0mon`, autoselects it, and updates Airgeddon's main menu state to `Mode: Monitor`.
  - Choosing Option 3 ("Put interface in managed mode") successfully restores `wlan0` and updates Airgeddon's main menu state to `Mode: Managed`.
