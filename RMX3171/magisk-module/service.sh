#!/system/bin/sh
# service.sh: Magisk late_start service for MTK Aircrack Core
MODDIR=${0%/*}

# Set proper executable permissions on overlay binaries
chmod 755 "$MODDIR"/system/bin/* 2>/dev/null

# Clean up stale locks and hopper PID
rm -f /data/local/tmp/airmon_mtk_hop.pid

