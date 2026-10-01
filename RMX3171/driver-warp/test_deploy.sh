#!/bin/bash
set -e

WARPED_KO="/home/shado/Documents/realme-reverse-engineering/RMX3171/driver-warp/wlan_drv_gen4m_warped.ko"
TARGET_PATH="/data/local/tmp/wlan_drv_gen4m_warped.ko"

echo "[*] Pushing warped driver to device..."
adb push "$WARPED_KO" "$TARGET_PATH"

echo "[*] Unloading current Wi-Fi module..."
adb shell su <<'EOF'
rmmod wlan_drv_gen4m 2>/dev/null || true
sleep 1
echo "[*] Loading warped Wi-Fi module..."
insmod /data/local/tmp/wlan_drv_gen4m_warped.ko
sleep 2

echo "[*] Triggering sniffer interface..."
iwpriv wlan0 set_fw_test 1 2 1 1 0 0 1 2412 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0
sleep 1

echo "[*] Checking network interfaces:"
ip link show | grep -E "wlan|mon|radiotap"
EOF

echo "[+] Deployment test complete."
