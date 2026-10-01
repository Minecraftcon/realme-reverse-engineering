#!/system/bin/sh
# uninstall.sh: Magisk module uninstall hook for MTK Aircrack Core
# Called by Magisk when user clicks "Remove" in Magisk App or removes module directory

# Stop any running channel hopper background daemon
if [ -f /data/local/tmp/airmon_mtk_hop.pid ]; then
    HOP_PID=$(cat /data/local/tmp/airmon_mtk_hop.pid 2>/dev/null)
    if [ -n "$HOP_PID" ]; then
        kill -9 "$HOP_PID" 2>/dev/null
    fi
    rm -f /data/local/tmp/airmon_mtk_hop.pid
fi

# Disable monitor mode if interface is up
if ip link show mon0 >/dev/null 2>&1; then
    /vendor/bin/wpa_cli -i wlan0 -p /data/vendor/wifi/wpa/sockets driver "MONITOR 0" >/dev/null 2>&1
    ip link set mon0 down 2>/dev/null
fi

# Clean up any leftover temporary socket locks
rm -f /data/vendor/wifi/wpa/sockets/wpa_ctrl_* 2>/dev/null

# Log completion
log -p i -t MTK-Aircrack-Core "Module uninstalled. Magisk overlays will unbind on next reboot, restoring factory stock driver automatically."
