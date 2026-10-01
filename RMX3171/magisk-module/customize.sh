#!/system/bin/sh
# customize.sh: Magisk module installer script for MTK Aircrack Core
# Target: MediaTek Helio G85 (MT6768 / Linux 4.14.186 AArch64)

ui_print "**********************************************"
ui_print "            MTK AIRCRACK CORE                 "
ui_print "   Native Monitor Mode & Packet Injection     "
ui_print "      MediaTek Gen4m / Helio G85 MT6768       "
ui_print "**********************************************"
ui_print ""

# Check architecture
ARCH=$(getprop ro.product.cpu.abi)
if [ "$ARCH" != "arm64-v8a" ]; then
    abort "[-] Error: Unsupported architecture: $ARCH (requires arm64-v8a)."
fi

ui_print "- Extracting warped wlan_drv_gen4m.ko & system shims..."
unzip -o "$ZIPFILE" 'system/*' -d "$MODPATH" >/dev/null 2>&1

# Permissions
ui_print "- Setting binary execution permissions..."
set_perm_recursive "$MODPATH/system/bin" 0 0 0755 0755
set_perm_recursive "$MODPATH/system/vendor/lib/modules" 0 0 0755 0644

ui_print "- Verifying patched driver payload..."
if [ -f "$MODPATH/system/vendor/lib/modules/wlan_drv_gen4m.ko" ]; then
    ui_print "[+] Warped MediaTek driver installed successfully."
else
    abort "[-] Error: Driver module payload missing from zip!"
fi

ui_print ""
ui_print "Commands available after boot/in Termux:"
ui_print "  - airmon-mtk start [channel]"
ui_print "  - airmon-ng start wlan0"
ui_print "  - iw dev / iw info"
ui_print ""
ui_print "[*] Installation complete! Reboot to load warped driver."
