#!/bin/bash
# SuperStation One / Retro Remake -- idle screensaver installer
#
# Interactive settings menu, similar in spirit to update_all's settings
# UI: pick your options, then Install applies them. Safe to re-run any
# time to change settings -- always cleanly stops whatever's currently
# running first.
#
# Run this from the MiSTer Scripts menu, or over SSH with:
#   bash install_ssone_screensaver.sh

set -u

INSTALL_DIR="/media/fat/linux/sso"
STARTUP_FILE="/media/fat/linux/user-startup.sh"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF_FILE="$INSTALL_DIR/ssone_screensaver.conf"

# ---- pick a menu backend: dialog, whiptail, or plain text fallback ----
MENU_TOOL=""
if command -v dialog >/dev/null 2>&1; then
    MENU_TOOL="dialog"
elif command -v whiptail >/dev/null 2>&1; then
    MENU_TOOL="whiptail"
fi

# ---- defaults (overridden below if a config already exists) ----
ASPECT="4x3"
TIMEOUT_SECONDS="180"
TIMEOUT_LABEL="3 minutes"

if [ -f "$CONF_FILE" ]; then
    . "$CONF_FILE"
fi

# Timeout presets: label + seconds ("-1" means disabled/"Never")
TIMEOUT_LABELS=("1 minute" "2 minutes" "5 minutes" "10 minutes" "15 minutes" "30 minutes" "Never (disabled)")
TIMEOUT_VALUES=(60 120 300 600 900 1800 -1)

menu_choice() {
    # menu_choice "Title" "current_label" label1 label2 ...
    local title="$1"; shift
    local current="$1"; shift
    local options=("$@")
    local i=1
    local choice

    if [ "$MENU_TOOL" = "dialog" ] || [ "$MENU_TOOL" = "whiptail" ]; then
        local args=()
        for opt in "${options[@]}"; do
            args+=("$i" "$opt")
            i=$((i + 1))
        done
        choice=$("$MENU_TOOL" --title "$title" --menu "Current: $current" 20 60 "${#options[@]}" "${args[@]}" 3>&1 1>&2 2>&3)
        echo "$choice"
        return
    fi

    # plain text fallback
    echo ""
    echo "=== $title (current: $current) ==="
    for opt in "${options[@]}"; do
        echo "  $i) $opt"
        i=$((i + 1))
    done
    read -p "Choice [1-${#options[@]}]: " choice
    echo "$choice"
}

pick_aspect() {
    local current_label="4:3 (original)"
    [ "$ASPECT" = "16x9" ] && current_label="16:9 (widescreen)"
    local choice
    choice=$(menu_choice "Screen Aspect Ratio" "$current_label" "4:3 (original)" "16:9 (widescreen)")
    case "$choice" in
        1) ASPECT="4x3" ;;
        2) ASPECT="16x9" ;;
    esac
}

pick_timeout() {
    local choice
    choice=$(menu_choice "Idle Timeout" "$TIMEOUT_LABEL" "${TIMEOUT_LABELS[@]}")
    if [ -n "$choice" ] && [ "$choice" -ge 1 ] && [ "$choice" -le "${#TIMEOUT_LABELS[@]}" ] 2>/dev/null; then
        local idx=$((choice - 1))
        TIMEOUT_LABEL="${TIMEOUT_LABELS[$idx]}"
        TIMEOUT_SECONDS="${TIMEOUT_VALUES[$idx]}"
    fi
}

do_install() {
    echo ""
    echo "Installing to $INSTALL_DIR ..."

    # Stop anything currently running -- ps-based, not "pkill -f"
    # (BusyBox's pkill often doesn't support -f and can silently fail).
    OLD_PIDS=$(ps | grep '[i]dle_watcher' | awk '{print $1}')
    if [ -n "$OLD_PIDS" ]; then
        echo "Stopping existing watcher process(es): $OLD_PIDS"
        for pid in $OLD_PIDS; do
            kill "$pid" 2>/dev/null || true
        done
        sleep 1
        STILL_UP=$(ps | grep '[i]dle_watcher' | awk '{print $1}')
        for pid in $STILL_UP; do
            kill -9 "$pid" 2>/dev/null || true
        done
    fi
    rm -f /tmp/ssone_idle_watcher.pid

    mkdir -p "$INSTALL_DIR"
    cp "$SRC_DIR/idle_watcher" "$INSTALL_DIR/"
    cp "$SRC_DIR/intro_screensaver" "$INSTALL_DIR/"
    cp "$SRC_DIR/intro_screensaver.sh" "$INSTALL_DIR/"
    [ -f "$SRC_DIR/intro_frames_4x3.bin" ] && cp "$SRC_DIR/intro_frames_4x3.bin" "$INSTALL_DIR/"
    [ -f "$SRC_DIR/intro_frames_169.bin" ] && cp "$SRC_DIR/intro_frames_169.bin" "$INSTALL_DIR/"
    chmod +x "$INSTALL_DIR/idle_watcher" "$INSTALL_DIR/intro_screensaver" "$INSTALL_DIR/intro_screensaver.sh"

    cat > "$CONF_FILE" << EOF
ASPECT="$ASPECT"
TIMEOUT_SECONDS="$TIMEOUT_SECONDS"
TIMEOUT_LABEL="$TIMEOUT_LABEL"
EOF

    STARTUP_LINE="[[ -e $INSTALL_DIR/idle_watcher ]] && $INSTALL_DIR/idle_watcher $INSTALL_DIR/intro_screensaver.sh $TIMEOUT_SECONDS &"

    mkdir -p "$(dirname "$STARTUP_FILE")"
    if [ ! -f "$STARTUP_FILE" ]; then
        echo "#!/bin/sh" > "$STARTUP_FILE"
        chmod +x "$STARTUP_FILE"
    fi

    # remove any previous SuperStation One screensaver line(s) before
    # adding the current one, so repeated installs don't stack up
    grep -v "SuperStation One / Retro Remake -- idle screensaver\|/idle_watcher .*intro_screensaver" "$STARTUP_FILE" > "$STARTUP_FILE.tmp" 2>/dev/null || cp "$STARTUP_FILE" "$STARTUP_FILE.tmp"
    mv "$STARTUP_FILE.tmp" "$STARTUP_FILE"
    chmod +x "$STARTUP_FILE"

    echo "" >> "$STARTUP_FILE"
    echo "# SuperStation One / Retro Remake -- idle screensaver" >> "$STARTUP_FILE"
    echo "$STARTUP_LINE" >> "$STARTUP_FILE"

    echo ""
    echo "Installed with:"
    echo "  Aspect ratio: $([ "$ASPECT" = "16x9" ] && echo "16:9 (widescreen)" || echo "4:3 (original)")"
    echo "  Idle timeout: $TIMEOUT_LABEL"
    echo ""
    echo "The watcher will start on next boot (60s startup delay, then the"
    echo "configured idle timeout). To start it right now without rebooting:"
    echo "  $INSTALL_DIR/idle_watcher $INSTALL_DIR/intro_screensaver.sh $TIMEOUT_SECONDS &"
    echo ""
    echo "Sanity check -- watcher processes currently running:"
    ps | grep '[i]dle_watcher' || echo "  (none)"
}

do_uninstall() {
    echo ""
    OLD_PIDS=$(ps | grep '[i]dle_watcher' | awk '{print $1}')
    for pid in $OLD_PIDS; do
        kill -9 "$pid" 2>/dev/null || true
    done
    rm -f /tmp/ssone_idle_watcher.pid
    if [ -f "$STARTUP_FILE" ]; then
        grep -v "SuperStation One / Retro Remake -- idle screensaver\|/idle_watcher .*intro_screensaver" "$STARTUP_FILE" > "$STARTUP_FILE.tmp" 2>/dev/null || true
        mv "$STARTUP_FILE.tmp" "$STARTUP_FILE"
        chmod +x "$STARTUP_FILE"
    fi
    rm -rf "$INSTALL_DIR"
    echo "Uninstalled. Removed $INSTALL_DIR and the startup hook."
}

# ---- main menu loop ----
while true; do
    ASPECT_LABEL="4:3 (original)"
    [ "$ASPECT" = "16x9" ] && ASPECT_LABEL="16:9 (widescreen)"

    choice=$(menu_choice "SuperStation One / Retro Remake Screensaver" \
        "Aspect: $ASPECT_LABEL | Timeout: $TIMEOUT_LABEL" \
        "Set Aspect Ratio" \
        "Set Idle Timeout" \
        "Install / Apply Settings" \
        "Uninstall" \
        "Exit")

    case "$choice" in
        1) pick_aspect ;;
        2) pick_timeout ;;
        3) do_install ;;
        4) do_uninstall ;;
        5|"") break ;;
        *) break ;;
    esac

    # plain-text fallback mode: run once through and exit rather than
    # looping forever without a real menu UI
    if [ -z "$MENU_TOOL" ]; then
        if [ "$choice" = "3" ] || [ "$choice" = "4" ]; then
            break
        fi
    fi
done

clear 2>/dev/null || true
