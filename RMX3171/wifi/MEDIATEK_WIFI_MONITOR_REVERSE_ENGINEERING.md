# MediaTek Gen4m Wi-Fi Monitor Mode & Radiotap Reversal Guide
**Device:** Realme Narzo 30A (`RMX3171`) / Helio G85 (`MT6768`)  
**Kernel:** Linux 4.14.186+ / Android 12 GSI  
**Driver Module:** `/vendor/lib/modules/wlan_drv_gen4m.ko` (`wlan_drv_gen4m`)

---

## 1. Executive Summary & Discovery

Contrary to common assumptions that MediaTek mobile combo chips (WMT / Gen4m) are purely closed FullMAC architectures without monitor mode capabilities, the stock vendor driver `wlan_drv_gen4m.ko` contains a **fully implemented Radiotap sniffer engine**, complete with:
- Dedicated network interface allocation (`alloc_netdev_mqs`) with `ARPHRD_IEEE80211_RADIOTAP` (`0x323`).
- Raw 802.11 management/control/data frame reception and parsing (`nicRxProcessMonitorPacket`).
- Per-packet Radiotap header synthesis (`nicRxFillRadiotapMCS`, `nicRxFillRadiotapVHT`) including RSSI, channel frequency, timestamps, and MCS rate.
- Hardware promiscuous mode control (`nicRxEnablePromiscuousMode`).

This functionality is accessible via private driver commands passed through `wpa_supplicant`.

---

## 2. Binary Architecture & Exact Disassembly Locations

### A. The Private Command Dispatcher (`priv_cmd_handlers`)
- **Symbol:** `priv_cmd_handlers`
- **Location in `.data`:** Section 6, offset `0x4c00` (File offset `0x15b290`)
- **Table Size:** `0x600` bytes (array of command structs)
- **Relevant Entry:**
  - String Offset: `.data + 0x5040` $\rightarrow$ Points to `"MONITOR\0"` (must be **UPPERCASE**)
  - Function Pointer: `.data + 0x5048` $\rightarrow$ Points to `priv_driver_set_monitor`

### B. Command Parser & Parameter Validator
- **Symbol:** `priv_driver_set_monitor`
- **Location in `.text`:** `0x8ad24` - `0x8b25c` (Length: `0x538` bytes)
- **Calling Interface:**
  ```text
  MONITOR [Enable] [PriChannel] [ChannelWidth] [Sco]
  ```

#### Critical Decompiled Assembly Checks:
1. **Argument Count Check (`0x0808ae1c`):**
   ```armasm
   bl   wlanCfgParseArgument
   ldr  w8, [sp, 8]       ; Number of parsed tokens
   cmp  w8, 5             ; Checks if argc >= 5 (Command + 4 arguments)
   b.lt 0x808b000         ; Rejects if fewer than 4 arguments are supplied
   ```
2. **Channel Width / Bandwidth Check (`0x0808afcc` & `0x0808afd4`):**
   *Many online snippets use index 0/1/2 for bandwidth. In Gen4m, it expects exact MHz:*
   ```armasm
   cmp  w23, 0x14         ; 0x14 = 20 MHz
   b.eq 0x808b074
   cmp  w23, 0x28         ; 0x28 = 40 MHz
   b.ne 0x808b238         ; If not 20 or 40 (or 80), fails with error!
   ```
3. **State Change & Workqueue Trigger (`0x0808b164` - `0x0808b1a0`):**
   ```armasm
   ldrb w9, [x25, 0x3d4]  ; Current monitor state
   cmp  w26, 0            ; Requested state (1 = on, 0 = off)
   cset w8, ne
   cmp  w9, w8            ; Avoid redundant transition
   b.eq 0x808b1a4
   strb w8, [x25, 0x3d4]  ; Save new state
   ...
   bl   queue_work_on     ; Schedules wlanMonWorkHandler on system_wq
   ```

### C. The Radiotap Interface Creator (`wlanMonWorkHandler`)
- **Symbol:** `wlanMonWorkHandler`
- **Location in `.text`:** `0x659c8` - `0x65ba8` (Length: `0x1e0` bytes)
- **Mechanism:**
  1. Calls `alloc_netdev_mqs(sizeof(struct GLUE_INFO), "radiotap%d", ...)`
  2. Sets hardware type at `dev->type` (`offset 0x234` in `net_device`):
     ```armasm
     mov  w9, 0x323        ; 0x323 = 803 decimal = ARPHRD_IEEE80211_RADIOTAP
     strh w9, [x8, 0x234]  ; dev->type = ARPHRD_IEEE80211_RADIOTAP
     ```
  3. Registers device via `register_netdev()`.
  4. Device emerges in Linux sysfs as `radiotap0`.

### D. Packet Ingestion & Radiotap Header Construction (`nicRxProcessMonitorPacket`)
- **Symbol:** `nicRxProcessMonitorPacket`
- **Location in `.text`:** `0x4e998` - `0x4efec` (Length: `0x650` bytes)
- **Functionality:**
  - Extracts MAC descriptor flags and hardware channel details.
  - Allocates an `sk_buff` using `kalPacketAlloc`.
  - Injects standard Radiotap fields: TSFT, Flags, Rate, Channel Frequency, Channel Flags, and Antenna Signal (dBm).
  - Copies raw 802.11 MPDU payload into `skb->data` via `memcpy`.
  - Pushes frame up to networking subsystem (`kalSetTxEvent2Rx`).

---

## 3. How to Invoke from Userspace

The MediaTek driver is wired to the standard Android `wpa_supplicant` driver command interface. You do **not** need custom kernel re-compilation to enable reception.

### Quick Start (CLI / Termux / Root Shell)

#### Step 1: Initialize Monitor Mode on a Specific Channel
Target: **Channel 6**, **20 MHz** width:
```bash
su -c 'wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 1 6 20 0"'
```
*Parameters breakdown:*
- `1` : Enable (`1` = ON, `0` = OFF)
- `6` : Primary Channel (`1` - `14` for 2.4GHz, or valid 5GHz channels)
- `20`: Channel Bandwidth in MHz (`20` or `40`)
- `0` : Secondary Channel Offset (`0` = None, `1` = Above, `2` = Below)

#### Step 2: Bring the Radiotap Interface Up
Once the command executes, the driver instantiates `radiotap0`:
```bash
su -c 'ip link set radiotap0 up'
```

Verify interface status:
```bash
su -c 'ip link show radiotap0'
```
*Expected output:*
```text
42: radiotap0: <NO-CARRIER,BROADCAST,MULTICAST,UP> mtu 1500 qdisc mq state DOWN mode DEFAULT group default qlen 1000
    link/ieee802.11/radiotap 00:00:00:00:00:00 brd ff:ff:ff:ff:ff:ff
```

#### Step 3: Capture Raw 802.11 Air Traffic
Run packet capture using native `tcpdump`:
```bash
su -c 'tcpdump -i radiotap0 -n -e -vv'
```
*Output will stream raw 802.11 Beacon frames, Probe Requests/Responses, and Data frames with full signal levels (-dBm).*

#### Step 4: Channel Hopping
To switch channels without tearing down the interface, resend the command with the new channel:
```bash
# Switch to Channel 1:
su -c 'wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 1 1 20 0"'

# Switch to Channel 11:
su -c 'wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 1 11 20 0"'
```

#### Step 5: Deactivate Monitor Mode
```bash
su -c 'wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 0"'
su -c 'ip link set radiotap0 down'
```

---

## 4. `airmon-ng` Compatibility & The `airmon-mtk` Tool

### Why Standard `airmon-ng` Fails
Standard Linux penetration testing tools rely on the `nl80211` or legacy `WEXT` kernel subsystems:
```bash
iw dev wlan0 interface add mon0 type monitor
# or
iwconfig wlan0 mode monitor
```
On MediaTek `gen4m` drivers, this immediately fails:
```text
command failed: Operation not supported on transport endpoint (-95)
```
**Root Cause:**
MediaTek intentionally omits `BIT(NL80211_IFTYPE_MONITOR)` from `wiphy->interface_modes` in `gl_init.c`. Only `NL80211_IFTYPE_STATION`, `NL80211_IFTYPE_AP`, `NL80211_IFTYPE_P2P_CLIENT`, and `NL80211_IFTYPE_P2P_GO` are registered. Any attempt to create or change an interface to monitor type is intercepted and rejected with `-EOPNOTSUPP`.

MediaTek completely isolated monitor mode inside its proprietary `priv_driver_cmds` diagnostic handler, which instantiates an unmanaged `ARPHRD_IEEE80211_RADIOTAP` interface (`radiotap0`).

### The Solution: `airmon-mtk`
To provide a drop-in replacement that mimics `airmon-ng` and handles channel management and channel hopping, use [`airmon-mtk`](./airmon-mtk):

```bash
# Push to device:
adb push airmon-mtk /data/local/tmp/
adb shell "su -c 'chmod +x /data/local/tmp/airmon-mtk'"

# Start monitor mode on Channel 6 (20MHz):
su -c '/data/local/tmp/airmon-mtk start 6'

# Switch channel on the fly:
su -c '/data/local/tmp/airmon-mtk channel 11'

# Start background channel hopping (cycles 1-13 every 0.3s):
su -c '/data/local/tmp/airmon-mtk hop 0.3'

# Sniff live frames:
su -c '/data/local/tmp/airmon-mtk sniff'
# Or capture to PCAP:
su -c '/data/local/tmp/airmon-mtk sniff /sdcard/capture.pcap'

# Check interface and hopper status:
su -c '/data/local/tmp/airmon-mtk status'

# Stop monitor mode and kill background hopper:
su -c '/data/local/tmp/airmon-mtk stop'
```

---

## 5. Driver Modification & Kernel Patching (Exposing Native `nl80211`)

To make standard tools (`airmon-ng start wlan0`, `iw dev radiotap0 set channel X`) work natively without any helper script:

1. **Advertise Monitor Mode in `wiphy`:**
   - In `wlan_drv_gen4m.ko` (`gl_init.c` / `mtk_cfg80211_init`), add `BIT(NL80211_IFTYPE_MONITOR)` to `wiphy->interface_modes`.
2. **Hook `change_virtual_intf` / `add_virtual_intf`:**
   - In `mtk_cfg80211_change_virtual_intf`, intercept requests where `type == NL80211_IFTYPE_MONITOR` and call `wlanMonWorkHandler` internally instead of returning `-EOPNOTSUPP`.
3. **Hook Channel Tuning in Monitor State:**
   - In `mtk_cfg80211_set_channel`, route frequency changes to `priv_driver_set_monitor(1, channel, bw, 0)` when the interface is operating in monitor mode.
4. **Bypass Bandwidth Restriction:**
   - In `priv_driver_set_monitor` at `0x0808afcc`, NOP or invert the conditional jumps to allow arbitrary channel width configs without failure.
5. **Promiscuous Control:**
   - Call `wlanSetPromiscuousMode` directly within `wlanMonWorkHandler` to bypass standard BSSID hardware filtering if unicast frame delivery is restricted by firmware.
6. **Auto-creation on Module Load:**
   - Patch `wlanNetCreate` (`0x6696c`) to call `wlanMonWorkHandler` immediately during boot so `radiotap0` is always present by default without requiring `wpa_cli` invocation.
