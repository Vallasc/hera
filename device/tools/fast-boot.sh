#!/bin/bash
# ============================================================
# HERA device - shorter boot (Raspberry Pi OS Lite, dedicated device)
#
# Disables system services that a dedicated HERA device does not use and that
# delay boot, and sets a few firmware options. Run after setup.sh and after
# the first boot has completed (Raspberry Pi Imager customisation runs at
# first boot through cloud-init).
#
#   bash tools/fast-boot.sh             apply, then: sudo reboot
#   bash tools/fast-boot.sh --dry-run   show what would change
#   bash tools/fast-boot.sh --report    measure the last boot
#   bash tools/fast-boot.sh --undo      revert everything
#
# Kept: bluetooth and hciuart (BLE and HFP), NetworkManager (station mode and
# hand-back), ssh, avahi-daemon (raspberrypi.local), time sync and swap.
#
# BOOT_DIR overrides the boot partition (default /boot/firmware, or /boot on
# older images). What this script changed is recorded in STATE_FILE, so that
# --undo reverts that and nothing else.
# ============================================================

set -euo pipefail

MODE=apply
case "${1:-}" in
    "")          ;;
    --dry-run)   MODE=dry ;;
    --undo)      MODE=undo ;;
    --report)    MODE=report ;;
    -h|--help)   sed -n '2,20p' "$0"; exit 0 ;;
    *)           echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
esac

# Units masked by this script. Masked rather than only disabled: another unit
# could otherwise pull them in again.
UNITS=(
    # Wait for the network to be online, up to 60 s or more. hera-device does
    # not need it: it configures the network itself.
    NetworkManager-wait-online.service
    systemd-networkd-wait-online.service
    # No modem, no keyboard, no desktop.
    ModemManager.service
    triggerhappy.service
    triggerhappy.socket
    keyboard-setup.service
    # Maintenance timers. Being persistent, they run right after boot when a
    # run was missed while the device was off, competing with the start-up.
    apt-daily.timer
    apt-daily-upgrade.timer
    man-db.timer
    e2scrub_all.timer
    e2scrub_reap.service
    # Pi 4 and 5 only; absent elsewhere.
    rpi-eeprom-update.service
)

# Firmware options, in the [all] section of config.txt:
#   disable_splash  no rainbow screen (~1 s)
#   boot_delay      no wait before loading the kernel
#   initial_turbo   full CPU clock for the first 30 s of boot, instead of
#                   waiting for the cpufreq governor; safe on every model
FIRMWARE=(disable_splash=1 boot_delay=0 initial_turbo=30)

CLOUD_FLAG=/etc/cloud/cloud-init.disabled
BACKUP_SUFFIX=.hera-fast-boot.bak
STATE_FILE=${STATE_FILE:-/var/lib/hera/fast-boot.state}

if [ -z "${BOOT_DIR:-}" ]; then
    BOOT_DIR=/boot/firmware
    [ -f "$BOOT_DIR/config.txt" ] || BOOT_DIR=/boot
fi
CONFIG="$BOOT_DIR/config.txt"
CMDLINE="$BOOT_DIR/cmdline.txt"

# Run a command quietly, or only show it with --dry-run. Returns its status.
run() {
    if [ "$MODE" = dry ]; then
        echo "    would run: $*"
    else
        "$@" >/dev/null 2>&1
    fi
}

unit_exists() {
    # list-unit-files also lists masked units, unlike `systemctl cat`.
    [ -n "$(systemctl list-unit-files --no-legend "$1" 2>/dev/null)" ]
}

unit_state() {
    systemctl is-enabled "$1" 2>/dev/null || true
}

# Record a change for --undo: "unit NAME PREVIOUS_STATE" or "cloud-init".
record() {
    if [ "$MODE" = dry ]; then
        return
    fi
    sudo mkdir -p "$(dirname "$STATE_FILE")"
    if ! sudo grep -qxF "$1" "$STATE_FILE" 2>/dev/null; then
        echo "$1" | sudo tee -a "$STATE_FILE" >/dev/null
    fi
}

# ------------------------------------------------------------------ report ---

if [ "$MODE" = report ]; then
    systemd-analyze || true
    echo
    echo "Slowest units:"
    systemd-analyze blame --no-pager | head -15 || true
    echo
    echo "Critical chain to hera-device:"
    systemd-analyze critical-chain hera-device.service --no-pager || true
    exit 0
fi

# -------------------------------------------------------------------- undo ---

if [ "$MODE" = undo ]; then
    echo "=== HERA device - restoring the default boot ==="
    if [ -f "$STATE_FILE" ]; then
        while read -r kind name previous; do
            case "$kind" in
                unit)
                    sudo systemctl unmask "$name" >/dev/null 2>&1 || true
                    # Back to its state before this script: enabled again
                    # only if it was enabled (static units need nothing).
                    if [ "$previous" = enabled ]; then
                        sudo systemctl enable "$name" >/dev/null 2>&1 || true
                    fi
                    echo "  restored $name (${previous:-unknown})"
                    ;;
                cloud-init)
                    sudo rm -f "$CLOUD_FLAG"
                    echo "  cloud-init enabled"
                    ;;
            esac
        done < "$STATE_FILE"
        sudo rm -f "$STATE_FILE"
    else
        echo "  no record of masked units ($STATE_FILE): services left as they are"
    fi
    for file in "$CONFIG" "$CMDLINE"; do
        if [ -f "$file$BACKUP_SUFFIX" ]; then
            sudo cp "$file$BACKUP_SUFFIX" "$file"
            sudo rm -f "$file$BACKUP_SUFFIX"
            echo "  $file restored"
        fi
    done
    echo "Done. Reboot to apply."
    exit 0
fi

# ------------------------------------------------------------------- apply ---

[ "$MODE" = dry ] && echo "=== HERA device - shorter boot (dry run) ===" \
                  || echo "=== HERA device - shorter boot ==="

echo "[1/4] Services"
for unit in "${UNITS[@]}"; do
    if ! unit_exists "$unit"; then
        continue
    fi
    state=$(unit_state "$unit")
    if [ "$state" = masked ]; then
        echo "  $unit: already masked"
        continue
    fi
    run sudo systemctl disable --now "$unit" || true
    if run sudo systemctl mask "$unit"; then
        record "unit $unit $state"
        echo "  $unit: masked (was $state)"
    else
        echo "  $unit: cannot be masked" >&2
    fi
done

echo "[2/4] cloud-init"
# Used by Raspberry Pi Imager to apply user, SSH and WiFi settings at first
# boot. Afterwards it only adds time to every boot.
if [ -d /etc/cloud ] && [ ! -f "$CLOUD_FLAG" ]; then
    run sudo touch "$CLOUD_FLAG"
    record cloud-init
    echo "  disabled ($CLOUD_FLAG)"
elif [ -f "$CLOUD_FLAG" ]; then
    echo "  already disabled"
else
    echo "  not installed"
fi

# Back a file up once, before the first change, for --undo.
backup() {
    if [ ! -f "$1$BACKUP_SUFFIX" ]; then
        run sudo cp "$1" "$1$BACKUP_SUFFIX"
    fi
}

echo "[3/4] Firmware ($CONFIG)"
if [ ! -f "$CONFIG" ]; then
    echo "  not found: skipped"
else
    # Work on a copy, then install it in one step.
    tmp=$(mktemp)
    trap 'rm -f "$tmp"' EXIT
    cp "$CONFIG" "$tmp"
    changed=0
    for setting in "${FIRMWARE[@]}"; do
        key=${setting%%=*}
        value=${setting#*=}
        # Uncommented lines setting the key, in any section.
        current=$(grep -E "^[[:space:]]*${key}=" "$tmp" | tail -1 | cut -d= -f2- || true)
        if [ "$current" = "$value" ]; then
            echo "  $key=$value: already set"
            continue
        fi
        if [ -n "$current" ]; then
            # Change every occurrence, wherever it is.
            sed -i -E "s/^([[:space:]]*${key}=).*/\1${value}/" "$tmp"
            echo "  $key: $current -> $value"
        else
            # A line appended to a conditional section such as [pi4] would
            # apply only to that model: append under [all].
            last_section=$(grep -E '^[[:space:]]*\[' "$tmp" | tail -1 | tr -d '[:space:]' || true)
            if [ -n "$last_section" ] && [ "$last_section" != "[all]" ]; then
                printf '\n[all]\n' >> "$tmp"
            fi
            # Make sure the file ends with a newline before appending.
            [ -z "$(tail -c1 "$tmp")" ] || echo >> "$tmp"
            echo "$setting" >> "$tmp"
            echo "  $key=$value: added"
        fi
        changed=1
    done
    if [ "$changed" = 1 ]; then
        backup "$CONFIG"
        run sudo cp "$tmp" "$CONFIG"
    fi
fi

echo "[4/4] Kernel command line ($CMDLINE)"
# `quiet`: no kernel messages on the console, which costs time on the serial
# console. cmdline.txt is a single line and must stay one.
if [ ! -f "$CMDLINE" ]; then
    echo "  not found: skipped"
elif grep -qw quiet "$CMDLINE"; then
    echo "  quiet: already set"
elif [ "$(wc -l < "$CMDLINE")" -gt 1 ]; then
    echo "  $CMDLINE has more than one line: left alone" >&2
else
    backup "$CMDLINE"
    line=$(tr -d '\n' < "$CMDLINE")
    if [ "$MODE" = dry ]; then
        echo "    would write: $line quiet"
    else
        printf '%s quiet\n' "$line" | sudo tee "$CMDLINE" >/dev/null
    fi
    echo "  quiet: added"
fi

if [ "$MODE" = dry ]; then
    echo
    echo "Dry run: nothing was changed."
else
    cat <<'MSG'

Done. Reboot, then measure:

  sudo reboot
  bash tools/fast-boot.sh --report

To revert: bash tools/fast-boot.sh --undo
MSG
fi
