#!/system_ext/bin/bash
# airgeddon-launcher: Optimized launcher for Realme Narzo 30A (MediaTek MT6768)
# Automatically sets PATH, library preloading, tmux multiplexing, and virtual interface shims.

export PATH="/data/adb/modules/rmx3171-wifi-enhancer/system/bin:/data/data/com.termux/files/usr/bin:/system_ext/bin:/system/bin:/vendor/bin:$PATH"
export LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib:$LD_LIBRARY_PATH"
export TERM="xterm-256color"
export AIRGEDDON_WINDOWS_HANDLING="tmux"
export AIRGEDDON_AUTO_UPDATE="false"
export AIRGEDDON_FORCE_NETWORK_MANAGER_KILLING="false"
export AIRGEDDON_EVIL_TWIN_SOUNDS="false"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

exec bash "$SCRIPT_DIR/airgeddon.sh" "$@"
