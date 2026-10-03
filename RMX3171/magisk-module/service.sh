#!/system/bin/sh
# service.sh: Magisk late_start service for MTK Aircrack Core
MODDIR=${0%/*}

# Set proper executable permissions on overlay binaries
chmod 755 "$MODDIR"/system/bin/* 2>/dev/null
chmod 755 "$MODDIR"/system/xbin/* 2>/dev/null

# Clean up stale locks and hopper PID
rm -f /data/local/tmp/airmon_mtk_hop.pid

# Suppress competing ROM/Toybox su binaries across all potential paths
SU_TARGETS="/system/xbin/su /vendor/bin/su /vendor/xbin/su /product/bin/su /system_ext/bin/su /system/bin/failsafe/su /data/local/xbin/su /data/local/bin/su"
for target in $SU_TARGETS; do
    if [ -e "$target" ]; then
        ver=$("$target" -v 2>&1)
        case "$ver" in
            *MAGISK*|*magisk*)
                ;;
            *)
                # 1. Try unmounting and removing
                umount -l "$target" 2>/dev/null
                rm -f "$target" 2>/dev/null
                # 2. If file still exists (read-only filesystem), mask it with official Magisk su
                if [ -e "$target" ]; then
                    mount --bind /system/bin/su "$target" 2>/dev/null
                fi
                ;;
        esac
    fi
done
