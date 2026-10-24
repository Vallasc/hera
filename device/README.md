# hera-device

Reference implementation of the HERA device, in C, for Raspberry Pi OS.

The device runs on a Raspberry Pi as a Bluetooth HFP hands-free unit for a
phone and relays call audio and call control to a client application over
an IP link. It implements version 4 of the protocol specified in
[`PROTOCOL.md`](../PROTOCOL.md), which is normative; this README describes the
implementation only.

Of the link modes in PROTOCOL.md §2, this device implements `AP` and `STA`.
The optional `USB` serial transport is not implemented, so every data path
is encrypted (§11.1).

```
     phone                     device                           client
  ┌────────┐   HFP / SCO    ┌───────────────┐                 ┌──────────┐
  │        │ ─────────────► │  SCO → PCM16  │ ══ UDP 7421 ══► │ playback │
  │        │     (CVSD)     │               │                 │          │
  │        │ ◄───────────── │  PCM16 → SCO  │ ◄═ UDP 7421 ═══ │ mic      │
  └────────┘                │               │                 │          │
                            │ state, cmds   │ ── TCP 7420 ─── │ events   │
                            │ network state │ ·· BLE GATT ··► │ discovery│
                            └───────────────┘                 └──────────┘
                                    │
                         hostapd + dnsmasq (device is the AP)
```

## Design

**No audio codec.** Audio is carried as PCM16 or G.711. With `FMT_PCM16_8K`,
which is selected whenever the SCO link is narrowband (CVSD), conversion is a
`memcpy` in both directions. PROTOCOL.md §6.1 gives the bandwidth figures.

**Access point by default.** The device runs its own WPA2 access point. Its
DHCP server announces no router and no DNS server, so the phone treats the
network as local-only and keeps cellular data as its default route
(PROTOCOL.md §2.4).

**All IP traffic is encrypted.** Each control connection uses a new
ephemeral X25519 key pair; HKDF-SHA256 derives four keys; ChaCha20-Poly1305
protects every control record and audio datagram (§11). The session token is
never sent over IP: it keys the HMAC that authenticates each `HELLO`.

**BLE for provisioning only.** BLE publishes the network state, accepts
WiFi credentials and delivers the token (§3). It stays up in every network
mode as the recovery path, and a timer returns the device to AP mode if it
cannot join a network.

## Architecture

All I/O is blocking and each I/O path has its own thread; there is no
application event loop apart from the D-Bus loop.

| thread | file | role |
|---|---|---|
| main | `bluez.c` | `sd_event` loop for D-Bus (BlueZ, GATT, advertising) |
| AT | `hfp.c` | AT dialogue over RFCOMM; produces events |
| SCO accept | `sco.c` | accepts the SCO link and hands it to the audio engine |
| audio (`SCHED_FIFO`) | `audio.c` | SCO ↔ wire format; the only real-time path |
| link | `link.c` | TCP and UDP sockets, framing, session |

| file | contents |
|---|---|
| `proto.c` | wire format: frame header, `HELLO`, `EVENT`, `CMD`, `ACK`, `BYE`, `STATS`, `CSTATS`, audio payload, format table. No I/O |
| `netcfg.c` | the three GATT payloads (§3.2–§3.4). No I/O |
| `sec.c` | encrypted profile (§11): X25519, HKDF, ChaCha20-Poly1305, anti-replay window |
| `jitter.c` | adaptive jitter buffer policy (§6.5). No I/O |
| `g711.c` | G.711 A-law and µ-law |
| `resample.c` | 8 ↔ 16 kHz half-band FIR, used only when the client requests a rate other than the SCO rate |
| `link.c` | control and audio channels, handshake, resume, takeover, keepalive, audio flow binding |
| `relay.c` | glue: outgoing events, incoming commands, state snapshot, `STATS` |
| `audio.c` | SCO loop, format conversion, uplink jitter buffer |
| `hfp.c` | HFP service level connection and indicator handling |
| `netd.c` | network modes: hostapd/dnsmasq for AP, wpa_supplicant for STA, fallback timer, token and AP passphrase |
| `gatt.c` | BLE advertising and the `NET_STATE`, `NET_CFG` and `SESSION` characteristics |
| `bluez.c` | BlueZ adapter setup, pairing agent, HFP profile registration, auto-reconnect |

Blocks received from the client are converted to SCO PCM on the link thread
when they are queued, so the real-time thread only copies samples out of the
jitter buffer.

### Latency

- The SCO socket is the clock. The audio thread blocks in `recv()` on the
  SCO socket and answers each packet with one of the same size, so both
  directions stay locked to the controller clock.
- The audio path does not allocate. Jitter buffer slots, resampler state and
  work buffers are preallocated.
- Each SCO packet is answered before the received samples are processed.
- Audio is never queued. A datagram the socket does not accept is dropped
  and counted (§6.6).
- The audio thread runs with `SCHED_FIFO` priority (default 20).
- SCO priming is off by default (`HERA_SCO_PRIME_PKTS=0`). Each primed packet
  adds about 3.75 ms of permanent latency (§7.1).
- The block duration is negotiated among 20, 10 and 5 ms (§6.2).
  `HERA_FRAME_MS=0` selects the shortest one the client supports, which
  reduces the transport latency from about 130 ms to about 50 ms at the cost
  of 200 instead of 50 packets per second in each direction.
- The uplink jitter buffer adapts to measured jitter between a floor and a
  ceiling (`jitter.c`, §6.5). It shrinks only by discarding filler blocks.
  `ul_target_ms` and `ul_buffered_ms` in `STATS` show the adaptation.

### SCO codec

Only CVSD is supported. The service reads raw PCM16 from the SCO socket,
which is what the kernel delivers for CVSD; with mSBC it would receive an
encoded transparent stream. The device therefore advertises only CVSD
(`AT+BAC=1`) and its SDP record does not announce wideband speech. Adding
mSBC would require H2 framing and an SBC decoder in `audio.c`; the protocol
already defines `FMT_PCM16_16K` for it.

## Dependencies

```sh
sudo apt update
sudo apt install build-essential pkg-config \
     libsystemd-dev libbluetooth-dev libsodium-dev \
     bluez bluez-tools hostapd dnsmasq wpasupplicant iw rfkill
```

Any libsodium version works: `src/sec.c` implements HKDF on top of
HMAC-SHA256, because libsodium provides HKDF only from 1.0.19 and Raspberry
Pi OS (trixie) ships 1.0.18.

Alternatively, `bash setup.sh` installs the dependencies, builds the binary,
configures BlueZ, and masks the system hostapd and dnsmasq units. The
service starts both daemons itself with its own configuration files.

## Build and test

```sh
make            # hera-device (requires Linux, BlueZ and libsystemd)
make test       # host tests: wire format, §14 vectors, crypto, jitter, G.711, resampler
make test-link  # link.c over localhost: handshake, takeover, resume (Linux only)
make test-tsan  # the same under ThreadSanitizer: unsynchronised accesses
make test-audio # audio.c over a fake SCO socket: both directions, all formats
make tests/link_server  # link.c behind a scripted relay, for client tests
make tools      # tools/hera_probe
```

`tests/link_server` runs the real link (handshake, encryption, resume,
timeouts) on ports 7420/7421 with the token `00 01 … 0f`, sends STATS and
downlink audio, and answers CMD ANSWER/HANGUP. The client package has a test
that runs against it (`hera_client/test/interop_test.dart`, enabled with
`HERA_INTEROP`).

`make test` and `make tools` build on any host, including macOS, since they
only use the portable modules. `make test-link` needs Linux (it uses
`eventfd`) but no Bluetooth: it runs the control channel against test
clients that perform the encrypted handshake, and covers new sessions, the
busy refusal, idle connections, wrong tokens, takeover, resume and
concurrent writers. They need libsodium (`brew install
libsodium` on macOS). The test vectors are those of PROTOCOL.md §14 and
§14.1; the X25519 keys come from RFC 7748 §6.1 and the HKDF implementation is
also checked against RFC 5869 Appendix A.

## Running

Root privileges are required (raw Bluetooth sockets, real-time priority,
network configuration):

```sh
sudo -E ./hera-device
```

As a systemd service:

```sh
sudo mkdir -p /opt/hera && sudo cp hera-device /opt/hera/
sudo cp hera-device.service /etc/systemd/system/
sudo systemctl enable --now hera-device
```

`CMD REBOOT` (§9) and `NET_CFG RESTART` (§3.3.1) make the process exit with
status 75; `Restart=always` in the unit restarts it.

### Single radio

In AP mode the service turns `wlan0` into an access point, so the Pi leaves
any WiFi network it was on. An SSH session over WiFi drops when the service
starts. To keep working on the Pi:

- use Ethernet (a USB adapter on models without a port), a serial console,
  or HDMI and a keyboard; or
- join the `HERA-XXXX` network from a laptop and connect to `192.168.73.1`.
  Set `HERA_AP_PASSPHRASE` first so that the passphrase is known.

For development there is a mode that leaves the network alone:

```sh
HERA_NET_MODE=none
```

No hostapd, no wpa_supplicant, no NetworkManager handover and no address
changes: the Pi stays on its network and the service listens on its current
address. The device reports itself as `STA`, so the client must be set to
the shared-WiFi mode. Requests to switch to AP mode are refused in this
mode; the unchanged state is published again so that the client does not
wait for a timeout.

If an SSH session over WiFi survives in `ap` mode, AP mode is not working:
with a single radio, `wlan0` has returned to station mode, typically because
NetworkManager reclaimed it. The service therefore marks the interface as
unmanaged (`nmcli device set wlan0 managed no`) before starting hostapd and
hands it back on exit. `SIGKILL` skips the hand-back; the systemd unit runs
it in `ExecStopPost`. When running by hand, restore it with:

```sh
sudo nmcli device set wlan0 managed yes
```

### Regaining access to the Pi

1. **Through the hotspot.** If the service is running, join `HERA-XXXX` from a
   laptop and `ssh 192.168.73.1`. The passphrase is delivered to the client
   over BLE; it can be fixed in advance with `HERA_AP_PASSPHRASE`.
2. **Disable the service.** With `WantedBy=multi-user.target` and
   `Restart=always`, an enabled service starts at every boot and takes
   `wlan0` again. From a console or through the hotspot:

   ```sh
   systemctl is-enabled hera-device
   sudo systemctl disable --now hera-device
   sudo nmcli device set wlan0 managed yes
   ```
3. **USB gadget mode.** On a Pi Zero the OTG port can act as a network
   adapter or a serial port. It is enabled by editing two files on the SD
   card's boot partition (FAT, readable on any computer); see below.
4. **Physical console.** HDMI and a keyboard, or the GPIO serial console with
   `enable_uart=1` and a USB-UART adapter.

Rebooting alone does not help while the service is enabled. It does reset an
interface left unmanaged, since that setting is kept in `/run`.

#### USB gadget mode

On the boot partition of the SD card:

**Ethernet over USB** (SSH over the cable):

```
config.txt    (append)                  dtoverlay=dwc2
cmdline.txt   (right after rootwait)    modules-load=dwc2,g_ether
```

`cmdline.txt` must remain a single line. Connect the port labelled `USB`
(not `PWR`) and run `ssh <user>@raspberrypi.local`. macOS and Linux detect
the gadget automatically; Windows needs an RNDIS driver.

**Serial console over USB** (does not depend on networking or mDNS):

```
config.txt    dtoverlay=dwc2
cmdline.txt   modules-load=dwc2,g_serial console=ttyGS0,115200
```

Open `/dev/tty.usbmodem*` (macOS) or `/dev/ttyACM0` (Linux) at 115200 baud.
The device does not use the OTG port for the protocol, so it is free for
this purpose.

### Boot time

On a stock image the device takes about a minute to come up, mostly in
services it does not use. Use Raspberry Pi OS Lite and, after the first boot,
run:

```sh
bash tools/fast-boot.sh --dry-run   # what would change
bash tools/fast-boot.sh
sudo reboot
bash tools/fast-boot.sh --report    # systemd-analyze, slowest units
```

It masks `NetworkManager-wait-online`, ModemManager, the apt, man-db and
e2scrub timers and a few other units, disables cloud-init, sets
`disable_splash=1`, `boot_delay=0` and `initial_turbo=30` in the `[all]`
section of `config.txt` and adds `quiet` to `cmdline.txt`. Bluetooth,
NetworkManager, SSH, avahi and time sync are kept. `--undo` reverts exactly
what the script changed (recorded in `/var/lib/hera/fast-boot.state`, with
backups of the two boot files).

## Configuration

Every setting has a working default and `.env` is optional. Copy
[`.env.example`](.env.example) to `.env` and change only what is needed. The
settings that matter most are `HERA_NET_MODE`, `HERA_FMT` and
`HERA_FRAME_MS`. The two jitter buffer settings are the bounds of the
adaptive target (§6.5).

### WiFi credentials

On first start the device generates the AP passphrase, stores it in
`/var/lib/hera/ap_passphrase` and delivers it over BLE (§3.4). The session
token is handled the same way (`/var/lib/hera/token`, §11.4).

For bring-up, fixed values can be set in `.env`:

```sh
HERA_AP_PASSPHRASE=choose-a-passphrase   # 8-63 printable ASCII characters
HERA_STA_SSID=HomeNetwork                # for HERA_NET_MODE=sta
HERA_STA_PSK=home-network-passphrase
```

- The generated passphrase persists across restarts because phones store
  the network (Android as a network suggestion) and reconnect
  automatically. There is no operation to rotate it (§3.3.1). To change it,
  delete `/var/lib/hera/ap_passphrase`, restart the service and provision
  the phone again.
- Station credentials received over BLE (`NET_CFG`) are stored in
  `/var/lib/hera/station` and take precedence over `HERA_STA_SSID` and
  `HERA_STA_PSK`. `NET_CFG FORGET_STA` deletes the file. The source in use is
  logged at start-up.
- The mode selected by the client (`NET_CFG` or `CMD NET_MODE`) is stored in
  `/var/lib/hera/net_mode` and takes precedence over `HERA_NET_MODE` at the
  next start. Delete the file to return to the configured mode.
- Starts that have not brought the network up are recorded in
  `/var/lib/hera/failed_starts`. After three of them within five minutes the
  device starts in AP mode regardless of the selected mode (§2.5).
- Values are validated at start-up and on receipt: 1–32 characters for an
  SSID, 8–63 for a passphrase, printable ASCII only, without `"` or `\`.
  These characters would break the quoted directives in the hostapd and
  wpa_supplicant configuration files.

## Bring-up tool

`tools/hera_probe` opens both channels, performs the handshake, prints
`EVENT`, `ACK` and `STATS` with per-second rates, and answers every
`AUDIO_RX` with a filler `AUDIO_TX` paced as in §6.4, so that the uplink
counters advance as well.

```sh
# the token, as 32 hex digits
sudo xxd -p /var/lib/hera/token

./tools/hera_probe 192.168.73.1 <token-hex>
./tools/hera_probe 192.168.73.1 <token-hex> answer
```

The probe does not use BLE: the host must already be on the device's
network and know the token. This separates device problems from
provisioning and client problems.

With the SCO link open and 20 ms blocks, `dl_blocks` should increase by 50
per second. PROTOCOL.md §13.1 lists what to check when it does not, and
§13.2 how to compare both sides.

## Implementation status

The modules built by `make test` (`proto.c`, `netcfg.c`, `sec.c`, `jitter.c`,
`stats.c`, `g711.c`, `resample.c`, `util.c`) are covered by host tests, and
`link.c` by `make test-link`. The other modules (`audio.c`, `hfp.c`, `sco.c`,
`bluez.c`, `gatt.c`, `netd.c`, `relay.c`) depend on BlueZ and the radio
hardware and have no automated tests.

### Known deviations from PROTOCOL.md

| section | deviation |
|---|---|
| §2.3, §4.4 | The `USB` transport is not implemented (it is optional). |

`ERROR` 4 is sent when the HFP service level connection with the phone
fails; `ERROR` 7 is sent when a `CMD NET_MODE` change fails and a client has
reconnected in the meantime.

## Notes

- **SCO over HCI.** At start-up the device issues a Broadcom vendor command
  so that SCO samples arrive on the HCI socket instead of the chip's PCM
  pins (§12.2). Some firmware revisions ignore it.
- **One client at a time** (§5.7). A second connection is held pending until
  its `HELLO` is verified: a valid resume of the current session takes it
  over (the previous connection receives `BYE` reason 3), anything else
  authenticated is refused with `BYE` reason 2. Connections that send no
  `HELLO` within `HERA_PEER_TIMEOUT_S` are closed.
- **SSIDs and passphrases never reach a shell** (§11.6). They are written to
  configuration files with mode `0600`, and `netd.c` runs external programs
  with `execv()`.
