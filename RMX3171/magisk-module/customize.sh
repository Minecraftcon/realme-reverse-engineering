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
set_perm_recursive "$MODPATH/system/xbin" 0 0 0755 0755
set_perm_recursive "$MODPATH/system/vendor/lib/modules" 0 0 0755 0644

ui_print "- Verifying patched driver payload..."
if [ -f "$MODPATH/system/vendor/lib/modules/wlan_drv_gen4m.ko" ]; then
    ui_print "[+] Warped MediaTek driver installed successfully."
else
    abort "[-] Error: Driver module payload missing from zip!"
fi

# Check for competing / non-Magisk root binaries (Toybox, legacy SuperSU, etc.)
ui_print "- Scanning for competing root binaries across ROM partitions..."
SU_CONFLICTS="/system/xbin/su /vendor/bin/su /vendor/xbin/su /product/bin/su /system_ext/bin/su /system/bin/failsafe/su /data/local/xbin/su /data/local/bin/su"
found_conflict=0
for bad_su in $SU_CONFLICTS; do
    if [ -e "$bad_su" ]; then
        ver=$("$bad_su" -v 2>&1)
        case "$ver" in
            *MAGISK*|*magisk*)
                ;;
            *)
                ui_print "[!] Detected competing su binary: $bad_su"
                ui_print "    Reported version: $ver"
                found_conflict=1
                ;;
        esac
    fi
done

if [ "$found_conflict" -eq 1 ]; then
    ui_print "[*] Auto-neutralization configured: service.sh will suppress conflicting su at boot."
else
    ui_print "[+] Clean root environment: zero competing su binaries detected."
fi

ui_print ""
ui_print "Commands available after boot / in Termux:"
ui_print "  - airgeddon-mobile   (Interactive 802.11 TUI audit suite)"
ui_print "  - airmon-ap          (Pure-Linux SoftAP & Evil Twin engine)"
ui_print "  - airmon-inject      (Dual-vector packet injection engine)"
ui_print "  - airmon-mtk         (Direct RF Synth & monitor mode control)"
ui_print "  - airmon-sniff       (Channel-locked PCAP sniffer)"
ui_print "  - airmon-ng          (Standard Aircrack-ng shim)"
ui_print "  - btmon-mtk          (Pure-Linux Bluetooth packet sniffer / PCAP)"
ui_print "  - bt-inject          (Hardware BLE beacon & packet injector)"
ui_print ""
ui_print "[*] Installation complete! Reboot to load warped driver."
