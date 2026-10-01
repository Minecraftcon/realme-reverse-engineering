#!/system/bin/sh
# airgeddon-launcher: Optimized launcher for Realme Narzo 30A (MediaTek MT6768)
# Automatically sets PATH, library preloading, tmux multiplexing, and virtual interface shims.

export PATH="/data/adb/modules/rmx3171-wifi-enhancer/system/bin:/data/data/com.termux/files/usr/bin:/system_ext/bin:/system/bin:/vendor/bin:$PATH"
export LD_LIBRARY_PATH="/data/data/com.termux/files/usr/lib:$LD_LIBRARY_PATH"
export TERM="xterm-256color"
export TMPDIR="/data/local/tmp"
export AIRGEDDON_WINDOWS_HANDLING="tmux"
export AIRGEDDON_AUTO_UPDATE="false"
export AIRGEDDON_FORCE_NETWORK_MANAGER_KILLING="false"
export AIRGEDDON_EVIL_TWIN_SOUNDS="false"

# Locate bash dynamically
BASH_BIN=""
if [ -x "/system_ext/bin/bash" ]; then
    BASH_BIN="/system_ext/bin/bash"
elif [ -x "/data/adb/modules/rmx3171-wifi-enhancer/system/bin/bash" ]; then
    BASH_BIN="/data/adb/modules/rmx3171-wifi-enhancer/system/bin/bash"
elif [ -x "/data/data/com.termux/files/usr/bin/bash" ]; then
    BASH_BIN="/data/data/com.termux/files/usr/bin/bash"
elif command -v bash >/dev/null 2>&1; then
    BASH_BIN="$(command -v bash)"
else
    echo "[-] Error: bash not found on device!" >&2
    exit 127
fi

# Set global tmux defaults so child windows inherit the full environment
if command -v tmux >/dev/null 2>&1; then
    tmux set-environment -g PATH "$PATH" 2>/dev/null || true
    tmux set-environment -g LD_LIBRARY_PATH "$LD_LIBRARY_PATH" 2>/dev/null || true
    tmux set-environment -g TMPDIR "$TMPDIR" 2>/dev/null || true
    tmux set-option -g default-shell "$BASH_BIN" 2>/dev/null || true
    tmux set-option -g default-command "$BASH_BIN" 2>/dev/null || true
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR" || exit 1

exec "$BASH_BIN" "$SCRIPT_DIR/airgeddon.sh" "$@"
