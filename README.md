# Realme Reverse Engineering Project

Reverse engineering documentation, driver hooks, bootloader analysis, and hardware discoveries for Realme & MediaTek devices.

## Devices Supported

* **Realme Narzo 30A (`RMX3171`)**
  - **SoC:** MediaTek Helio G85 (`MT6768`)
  - **GPU:** Mali-G52 MC2
  - **Wi-Fi Module:** MediaTek Gen4m (`wlan_drv_gen4m.ko`)

---

## Repository Structure

```text
.
└── RMX3171/
    └── wifi/
        └── MEDIATEK_WIFI_MONITOR_REVERSE_ENGINEERING.md
```

### Highlights:
* [MediaTek Gen4m Wi-Fi Monitor Mode & Radiotap Reversal Guide](RMX3171/wifi/MEDIATEK_WIFI_MONITOR_REVERSE_ENGINEERING.md): Detailed reverse-engineering walkthrough of the stock `wlan_drv_gen4m.ko` kernel module, uncovering the private `MONITOR` command, `ARPHRD_IEEE80211_RADIOTAP` interface registration, and raw 802.11 frame capturing without external dongles.
