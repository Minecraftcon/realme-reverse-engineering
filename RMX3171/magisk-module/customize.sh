#!/system/bin/sh
# customize.sh: Magisk module installer script

ui_print "- Installing MediaTek Wi-Fi Desktop Shims..."
unzip -o "$ZIPFILE" 'system/*' -d "$MODPATH" >/dev/null 2>&1
set_perm_recursive "$MODPATH/system/bin" 0 0 0755 0755
ui_print "- Installation complete! Reboot to enable system-wide overlays."
