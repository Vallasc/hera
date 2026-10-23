#!/bin/bash
# ============================================================
# HERA device - system setup (Raspberry Pi OS / Debian)
# Installs dependencies, builds hera-device and configures BlueZ.
# Run with: bash setup.sh
# ============================================================

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

echo "=== HERA device - setup (Raspberry Pi OS / Debian) ==="

echo "[1/4] Installing system dependencies..."
sudo apt-get update -qq
# Build toolchain + development headers required by the Makefile:
#   libsystemd-dev   -> sd-bus / sd-event (BlueZ D-Bus integration)
#   libbluetooth-dev -> raw SCO / RFCOMM sockets (-lbluetooth)
#   libsodium-dev    -> encrypted profile (PROTOCOL.md §11); any version.
# Runtime: bluez, hostapd and dnsmasq (AP mode), wpasupplicant (STA mode),
# iw (regulatory domain, AP check, client RSSI) and rfkill.
sudo apt-get install -y \
    build-essential \
    pkg-config \
    libsystemd-dev \
    libbluetooth-dev \
    libsodium-dev \
    bluez \
    bluez-tools \
    hostapd \
    dnsmasq \
    wpasupplicant \
    iw \
    rfkill

# hera-device starts hostapd and dnsmasq itself, with configuration files in
# /run/hera. Mask the system units so they do not compete for the interface.
sudo systemctl disable --now hostapd dnsmasq 2>/dev/null || true
sudo systemctl mask hostapd dnsmasq 2>/dev/null || true

echo "[2/4] Building hera-device..."
make -C "$SCRIPT_DIR" clean >/dev/null 2>&1 || true
make -C "$SCRIPT_DIR"
echo "  Built: $SCRIPT_DIR/hera-device"

echo "[3/4] Configuring Bluetooth..."
sudo rfkill unblock bluetooth 2>/dev/null || true
# Raspberry Pi OS keeps WiFi soft-blocked until a country is set.
sudo rfkill unblock wifi 2>/dev/null || true
sudo systemctl enable bluetooth 2>/dev/null || true

if [ -f /etc/bluetooth/main.conf ]; then
    sudo cp /etc/bluetooth/main.conf /etc/bluetooth/main.conf.bak
    echo "  Backup saved to /etc/bluetooth/main.conf.bak"
fi
sudo tee /etc/bluetooth/main.conf > /dev/null << 'CONF'
[General]
Name = HERA
# Audio/Video - Hands-free (0x200408)
Class = 0x200408
DiscoverableTimeout = 0
PairableTimeout = 0
# Faster connection setup; allow Just Works re-pairing without removing the
# existing bond first.
FastConnectable = true
JustWorksRepairing = always
# Only the hands-free profile is used.
DisablePlugins = a2dp,avrcp

[Policy]
AutoEnable = true

[LE]
# Preferred LE connection parameters (units of 1.25 ms). BLE is only used for
# provisioning; the central decides the final values.
MinConnectionInterval = 12
MaxConnectionInterval = 24
ConnectionLatency = 0
ConnectionSupervisionTimeout = 400
CONF

echo "[4/4] Restarting Bluetooth..."
sudo systemctl restart bluetooth || true

cat <<'MSG'

Setup complete.

Optional configuration (every setting has a working default):

  cp .env.example .env

Then start (root is required for raw Bluetooth / SCO access and for the
real-time audio thread):

  sudo -E ./hera-device

To run it as a service instead. Note that an enabled service starts at every
boot and takes over wlan0, so the Pi will not rejoin your usual WiFi network.
During development, run it by hand.

  sudo mkdir -p /opt/hera && sudo cp hera-device .env* /opt/hera/ 2>/dev/null
  sudo cp hera-device.service /etc/systemd/system/
  sudo systemctl enable --now hera-device

Pair the phone once (it appears as "HERA"), then start the client: it finds
the device over BLE, reads the network state and the credentials, joins the
hotspot and connects (PROTOCOL.md §3.5).

The hotspot passphrase is generated on first start, stored in
/var/lib/hera/ap_passphrase and delivered over BLE. To use a fixed passphrase
(e.g. to join the hotspot from a laptop during bring-up), set it in .env:

  HERA_AP_PASSPHRASE=choose-a-passphrase

Station-mode credentials can be configured the same way:

  HERA_NET_MODE=sta
  HERA_STA_SSID=HomeNetwork
  HERA_STA_PSK=home-network-passphrase

NOTE: the Pi has a single radio. In AP mode wlan0 becomes an access point and
leaves your usual WiFi network, so an SSH session over WiFi will drop when the
service starts. Use Ethernet, a serial console, or SSH to 192.168.73.1 after
joining the hotspot. If the SSH session survives, AP mode is not working.

The session token, for tools/hera_probe:

  sudo xxd -p /var/lib/hera/token
MSG
