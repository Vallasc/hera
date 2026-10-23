# The HERA Protocol, Version 4

## Abstract

This document specifies version 4 of the HERA protocol. A HERA device acts
as a Bluetooth Hands-Free unit (HFP HF role) for a mobile phone and relays
the audio and control of the phone's calls to a client application over an
IP link. Discovery and provisioning use Bluetooth Low Energy (BLE). All IP
traffic is encrypted and authenticated with keys derived from an ephemeral
X25519 exchange. The audio block duration is negotiated among 20, 10 and
5 ms.

## Status of This Document

This is the normative specification of the HERA protocol, version 4. It is
self-contained: an implementation of the device or of the client needs no
other document. Sections and paragraphs marked *(informative)* give
rationale or implementation guidance and contain no requirements.

The reference implementations are `hera_device` (C, Raspberry Pi OS) and
`hera_client` (Dart/Flutter). Where the reference device deviates from this
specification, the deviation is listed in its README.

## Conventions and Terminology

The key words "MUST", "MUST NOT", "REQUIRED", "SHALL", "SHALL NOT",
"SHOULD", "SHOULD NOT", "RECOMMENDED", "NOT RECOMMENDED", "MAY", and
"OPTIONAL" in this document are to be interpreted as described in BCP 14
[RFC2119] [RFC8174] when, and only when, they appear in all capitals, as
shown here.

All multi-byte integer fields are little endian unless stated otherwise.
`u8`, `u16`, `u32` and `u64` denote unsigned integers of 8, 16, 32 and 64
bits; `i8` denotes a signed 8-bit integer. `‖` denotes concatenation.

The following terms are used:

- **Phone**: the mobile phone whose calls are relayed. It is the HFP Audio
  Gateway.
- **Device**: the HERA device. It is the HFP Hands-Free unit and the IP
  server. The reference hardware is a Raspberry Pi.
- **Client**: the application that connects to the device over IP. In the
  normal configuration it runs on the phone itself.
- **Frame**: the protocol data unit defined in §5.1.
- **Block**: one unit of audio of the negotiated duration (§6.2). A frame
  carrying audio contains one or more blocks.
- **Session**: the protocol state shared by the device and one client,
  established by the `HELLO` exchange (§5.3) and identified by a
  `session_id`.
- **Downlink**: audio from the phone to the client (`AUDIO_RX`).
- **Uplink**: audio from the client to the phone (`AUDIO_TX`).

---

## 1. Overview

The device presents itself to the phone as a Bluetooth Classic hands-free
unit (HFP, HF role). During a call it receives the call audio over SCO and
exchanges it with the client over an IP link.

```
     phone                     device                           client
  ┌────────┐   HFP / SCO    ┌───────────────┐                 ┌──────────┐
  │        │ ─────────────► │  SCO → PCM16  │ ══ UDP 7421 ══► │ playback │
  │        │     (CVSD)     │               │    AUDIO_RX     │ / STT    │
  │        │ ◄───────────── │  PCM16 → SCO  │ ◄═ UDP 7421 ═══ │ TTS /    │
  └────────┘                │               │    AUDIO_TX     │ mic      │
                            │               │                 │          │
                            │ state, cmds   │ ── TCP 7420 ──► │ events   │
                            │               │ ◄─ TCP 7420 ─── │ commands │
                            │               │                 │          │
                            │ network state │ ·· BLE GATT ··► │ discovery│
                            └───────────────┘                 └──────────┘
                                    │
                         hostapd + dnsmasq (device is the AP)
```

- `AUDIO_RX` (device to client) carries the remote party's voice.
- `AUDIO_TX` (client to device) carries the audio the phone receives as the
  hands-free microphone.
- The control channel carries state events (ringing, caller number, audio
  channel state), commands (answer, hang up, dial), statistics and logs.
- BLE is used only for discovery and provisioning. It never carries audio or
  call control.

The phone that places the call and the client host are normally the same
phone: one radio carries HFP/SCO to the device, the other carries WiFi to the
device.

### 1.1 Rationale for an IP Link *(informative)*

The device's Bluetooth controller already carries the SCO link of the call.
SCO reserves slots at fixed intervals and takes precedence over other
traffic, so a data channel on the same radio only gets the remaining
capacity, which cannot be measured while no call is active. Over IP, the
required ~160 kbit/s is under 1% of an 802.11n 1x1 link, and TCP and UDP
sockets are well supported on iOS and Android.

### 1.2 Radio Coexistence *(informative)*

Moving the data link to WiFi does not remove coexistence concerns. The
Raspberry Pi Zero W, Zero 2 W and Pi 3/4/5 use a combined WiFi/Bluetooth
chip with a shared antenna, which during a call serves both the SCO link
and the access point. This SHOULD be measured on the target hardware
before anything else.

Measurement: with a call active for at least 5 minutes, record `dl_blocks`,
`ul_blocks`, `ul_lost`, `ul_jitter_ms` and `rssi` from `STATS` (§10.1), and
`dl_lost` and `rtt_ms` from `CSTATS` (§10.2).

Acceptance criteria: `dl_blocks` and `ul_blocks` increase by 1000 /
`frame_ms` per second (50 per second with 20 ms blocks), loss is below 0.5%,
and the median RTT is below 30 ms.

If the criteria are not met, in order of preference: (1) use `FMT_G711A`
(§6.2), which halves the payload at no CPU cost; (2) run the access point on
a second radio (USB WiFi adapter); (3) run Bluetooth on a USB adapter and
leave the internal radio to WiFi.

---

## 2. Link Modes

The application protocol is identical in all link modes. The modes differ
only in how the link is established and which sockets are used.

| mode | id | link | control | audio | client platforms |
|---|---|---|---|---|---|
| `AP` | 1 | WPA2 access point run by the device | TCP | UDP | iOS, Android |
| `STA` | 2 | device and phone on an existing WiFi network | TCP | UDP | iOS, Android |
| `USB` | 3 | serial line (CDC-ACM or USB-UART bridge) | one byte stream | | Android, desktop |

`AP` is the default mode. `STA` is intended for bench use, with the
limitations given in §2.2.

`USB` is OPTIONAL. A conforming device MAY omit it; §2.3 and §4.4 apply only
to devices that provide it. `USB` is the only mode without encryption
(§11.1); a device that does not implement it has no unencrypted data path.
The reference device does not implement `USB`.

### 2.1 `AP`: The Device Is the Access Point

| parameter | value | reference configuration |
|---|---|---|
| SSID | `HERA-XXXX`, where `XXXX` are the last two bytes of the WiFi MAC address in hexadecimal | `HERA_AP_SSID_PREFIX` |
| security | WPA2-PSK, CCMP | |
| passphrase | 16 characters, generated on first start, delivered over BLE (§3.4) | `HERA_AP_PASSPHRASE` |
| band | 2.4 GHz, channel 6, 20 MHz | `HERA_AP_CHANNEL` |
| device address | `192.168.73.1/24` | `HERA_AP_ADDR` |
| DHCP pool | `.10` to `.60` of the device's /24, 1 h lease | |
| DHCP option 3 (router) | MUST NOT be sent (§2.4) | |
| DHCP option 6 (DNS) | MUST NOT be sent (§2.4) | |

The SSID SHOULD NOT be hidden. Hiding it adds no security and prevents
automatic reconnection on iOS.

In `AP` mode the device address is the first host address of its subnet.
Clients MUST nevertheless read it from `NET_STATE` (§3.2), since the subnet
is configurable.

### 2.2 `STA`: The Device Joins an Existing Network

Credentials are delivered over BLE (§3.3) and persisted by the device. The
address obtained is reported in `NET_STATE`.

All IP traffic is encrypted in this mode as in every other (§11.1). `STA`
nevertheless has the following limitations, which encryption does not
address:

- The control port is reachable from the whole network. Without the token
  no peer can complete `HELLO` (§11.4), but any host can open connections
  that the device has to reject, including during a call.
- Traffic analysis remains possible: other hosts can observe that a call is
  in progress, its duration and its packet timing (§11.6).
- The phone's default route is the shared network, not the cellular
  network. The mechanism of §2.4 requires the device to run the DHCP
  server.

`AP` SHOULD be used for normal operation.

### 2.3 `USB`: Serial Line

A single byte stream over a cable, without a network. Both channels are
multiplexed on it (§4.4).

| parameter | value |
|---|---|
| line speed | 460800 baud, 8N1 |
| format | `FMT_G711A` SHOULD be used |
| block duration | 20 ms only |

*(informative)* At 460800 baud 8N1 the line carries 46 080 bytes per second
in each direction. With 12 bytes of framing per 20 ms block (4 of frame
header and 8 of audio header), the per-direction load is:

| format | bytes/s per direction | line load per direction |
|---|---|---|
| `FMT_PCM16_16K` | 32 600 | 71% |
| `FMT_PCM16_8K` | 16 600 | 36% |
| `FMT_G711A` | 8 600 | 19% |

G.711 leaves the most headroom for control traffic and for bursts after
underruns or reordering.

### 2.4 Default Route

In `AP` mode the phone's WiFi interface is connected to a network that has
no Internet access. If the phone used it as its default route, applications
relying on network services would lose connectivity during the call.

The device MUST therefore present the network as local-only:

- its DHCP server MUST NOT send option 3 (router) or option 6 (DNS
  servers);
- it MUST NOT answer captive portal probes and MUST NOT redirect traffic.

With this configuration iOS and Android classify the network as having no
Internet access, keep the cellular network as the default route, and still
route traffic for the local subnet over WiFi.

Clients SHOULD additionally bind their sockets to the device's network
explicitly (§12.3), so that routing does not depend on operating system
heuristics.

### 2.5 Mode Selection and Recovery

The client selects the mode (§3.3, §9); the device applies the request and
persists it. At startup the reference device behaves as follows:

```
HERA_NET_MODE = ap   (default)  -> AP
HERA_NET_MODE = sta             -> STA with the stored credentials;
                                   no address within HERA_STA_TIMEOUT_S
                                   (default 30 s) -> AP
HERA_NET_MODE = usb             -> serial line, no network
```

A device without a display that accepts WiFi credentials can become
unreachable. A device MUST implement the following safeguards:

1. Fallback timer: if `STA` does not obtain an address within
   `HERA_STA_TIMEOUT_S`, the device MUST switch to `AP` and report the cause
   in `NET_STATE.fail`.
2. BLE MUST remain available in every mode, so that the client can read the
   state and provision the device again.
3. Recovery: after three service starts within five minutes without
   reaching state `up`, the device MUST start in `AP` regardless of the
   configured mode.

Concurrent `AP` and `STA` operation is not supported. On a combined chip it
forces both interfaces onto the same channel and is unreliable; it requires
a second radio.

---

## 3. BLE Discovery and Provisioning

BLE is used to locate the device, to report its network configuration and
to deliver credentials. The traffic is a few hundred bytes at a time and is
not affected by SCO coexistence.

BLE MUST NOT carry audio, call events, commands, statistics or logs. If the
IP link is down, the client is disconnected and SHOULD report this; there is
no reduced mode over BLE.

### 3.1 Advertising and GATT Service

| parameter | value |
|---|---|
| advertised local name | `HERA` |
| service UUID (scan response) | `48455241-0001-4c32-9a3e-8f1d5b7c2e01` |

The local name is carried in the advertising data and the 128-bit service
UUID in the scan response, because both do not fit in one 31-byte
advertising PDU.

| characteristic | UUID | properties |
|---|---|---|
| `NET_STATE` | `48455241-0002-4c32-9a3e-8f1d5b7c2e01` | encrypted read, notify |
| `NET_CFG` | `48455241-0003-4c32-9a3e-8f1d5b7c2e01` | encrypted write |
| `SESSION` | `48455241-0004-4c32-9a3e-8f1d5b7c2e01` | encrypted read |

All three characteristics MUST require an encrypted link. The first access
therefore triggers pairing (LE "Just Works"), and no credential is sent in
the clear. The bond SHOULD be stored, so that pairing happens only once.

### 3.2 `NET_STATE` (Read, Notify)

```
 offset  size      field
 ───────────────────────────────────────────────────────────────────────
   0      1        version     = 0x04
   1      1        mode        1 AP, 2 STA, 3 USB
   2      1        state       0 down, 1 coming up, 2 up, 3 failed
   3      1        fail        failure code (§3.2.1); meaningful only
                               when state = 3
   4      4        ipv4        address bytes in dotted-quad order (not
                               little endian); 00 00 00 00 = no address
   8      2        ctrl_port   TCP port of the control channel
  10      2        audio_port  UDP port of the audio channel
  12      1        ssid_len
  13   ssid_len    ssid        UTF-8, not NUL-terminated: the AP SSID in
                               AP mode, the joined network in STA mode
```

The device MUST send a notification whenever `mode`, `state` or `ipv4`
changes. A client that keeps the GATT connection open is informed of network
changes without polling.

The AP passphrase is not part of `NET_STATE`; it is delivered in `SESSION`
(§3.4).

#### 3.2.1 Failure Codes

| code | meaning |
|---|---|
| 0 | no error |
| 1 | SSID not found |
| 2 | authentication rejected |
| 3 | associated, but no IP address obtained |
| 4 | interface unavailable or driver error |
| 5 | hostapd or dnsmasq failed to start |
| 6 | no stored credentials |

### 3.3 `NET_CFG` (Write)

```
 offset  size      field
 ─────────────────────────────────────────────
   0      1        version = 0x04
   1      1        op      (§3.3.1)
   2      1        ssid_len
   3      1        psk_len
   4   ssid_len    ssid    (UTF-8)
   …    psk_len    psk     (UTF-8)
```

A write whose version differs from 0x04, or whose declared lengths exceed
the written value, MUST be rejected.

#### 3.3.1 Operations

| op | name | fields | effect |
|---|---|---|---|
| 1 | `START_AP` | none | switch to `AP` and persist |
| 2 | `JOIN_STA` | `ssid`, `psk` | store the credentials and switch to `STA`; on failure fall back to `AP` (§2.5) |
| 3 | `FORGET_STA` | none | delete the stored credentials and switch to `AP` |
| 4 | reserved | | MUST NOT be used |
| 5 | `RESTART` | none | restart the HERA service; the network is unchanged |

Operation 4 was `ROTATE` in earlier versions and is reserved so that an
outdated peer sending it has no effect. A device MUST ignore unknown
operations.

*(informative)* `ROTATE` generated a new token and AP passphrase. It was
removed because phones store the network (Android as a network suggestion)
and a changed passphrase forces the user to re-enter it. Since all IP
traffic is encrypted (§11), the WPA2 passphrase grants network access but
not access to call content.

The device MUST acknowledge the GATT write before applying the operation,
since the operation may take the network down, and MUST report progress
through `NET_STATE` notifications.

### 3.4 `SESSION` (Encrypted Read)

```
 offset  size      field
 ─────────────────────────────────────────────
   0      1        version = 0x04
   1     16        token    session token (§11.4)
  17      1        psk_len
  18   psk_len     psk      AP passphrase (UTF-8); psk_len = 0 outside AP
```

In `AP` mode the value is 34 bytes, which exceeds the default ATT MTU of 23
and requires a long read (ATT Read Blob). BlueZ and common BLE libraries
handle this transparently. On Android, clients SHOULD request an MTU of at
least 64.

#### Read Access

The device MUST refuse `SESSION` reads while a session is established
(§5.4) or within its resume window, and MUST allow them otherwise. The
reference device refuses with `org.bluez.Error.NotPermitted`, which reaches
the client as ATT error 0x02 (Read Not Permitted).

*(informative)* The condition is session state, not elapsed time. A
provisioning window measured from boot closes permanently: a phone arriving
later, or the same phone after reinstalling the client, could no longer
provision a device without a display, which §2.5 forbids. The refusal
prevents a second phone from obtaining the token and taking over the
session (§5.7) during a call; counting the resume window as busy prevents a
ten-second opening while a client reconnects. It does not protect the first
pairing against an on-path attacker (§11.6).

### 3.5 First Connection Sequence

```
client   scan, filtering on the HERA service or name
client   GATT connect
client   read NET_STATE              ← triggers pairing (Just Works)
client   read SESSION                ← token and AP passphrase
client   join the device's WiFi network with SSID and passphrase
client   TCP connect to NET_STATE.ipv4 : NET_STATE.ctrl_port
         ─ continue with §5 ─
```

On later connections the bond exists, the token is stored and the phone
remembers the network. The client SHOULD connect over TCP directly and use
BLE only if that fails.

---

## 4. Channels

### 4.1 Control Channel

| parameter | value | reference configuration |
|---|---|---|
| transport | TCP | |
| port | 7420 | `HERA_CTRL_PORT` |
| roles | device listens, client connects | |
| framing | byte stream of frames (§5.1, §4.1.1), carried in records (§11.2) | |
| socket options | `TCP_NODELAY` on both ends | |

The control channel carries `HELLO`, `BYE`, `PING`, `PONG`, `EVENT`, `CMD`,
`ACK`, `LOG`, `STATS` and `CSTATS`. It MUST NOT carry audio.

Both ends MUST set `TCP_NODELAY`. Control frames are small and infrequent;
Nagle's algorithm can delay them by up to 40 ms.

The control channel defines the session: the session exists while the
channel is open, and enters the resume window (§5.4) when it closes.

#### 4.1.1 Reassembly

Frame boundaries do not coincide with read boundaries. Receivers MUST
reassemble frames using `LEN`:

```
buf = []
loop:
    buf += read()                          # any number of bytes
    while len(buf) >= 4:
        n = u16le(buf[2..4])
        if n > 1024: fatal error, close the channel
        if len(buf) < n + 4: break         # incomplete frame
        deliver(buf[0 .. n+4])
        remove the first n+4 bytes from buf
```

A sender MAY combine several frames in one write, and on `USB`, where cost
is per write rather than per byte, SHOULD do so. A receiver MUST NOT assume
any relation between writes and frames.

After key derivation the same reassembly applies to the plaintext of each
record (§11.2).

### 4.2 Audio Channel

| parameter | value | reference configuration |
|---|---|---|
| transport | UDP | |
| device port | 7421 | `HERA_AUDIO_PORT` |
| client port | ephemeral | |
| framing | exactly one frame per datagram | |

The audio channel carries `AUDIO_RX`, `AUDIO_TX`, `PING` and `PONG`. Frames
of any other type received on the audio channel MUST be ignored.

The logical content of a datagram is:

```
 offset  size      field
 ──────────────────────────────────────────
   0      4        session_id  (u32, from the device HELLO)
   4      …        one complete frame (4 + LEN bytes)
```

This is the content, not the wire format. On the wire the frame is
encrypted and the datagram also carries a counter and a tag; §11.2 defines
the actual layout. No unencrypted form of an audio datagram exists.

The `session_id` associates the datagram with a session: it allows late
datagrams of an earlier session to be discarded after a resume, and it
identifies the flow for binding (§4.2.1). It is sent in the clear and
authenticated as additional data, since the receiver needs it to select
the key.

A receiver MUST silently discard a datagram if the tag does not verify, the
`session_id` is unknown, the counter is a replay (§11.2), or the decrypted
frame length differs from `LEN + 4`. Invalid datagrams are not protocol
errors.

*(informative)* The two channels handle errors differently by design. On
the control channel a malformed frame indicates a bug and the channel is
closed; on the audio channel it is treated as noise and discarded.

#### 4.2.1 Audio Flow Binding

The device learns the client's audio address from the first valid datagram
it receives.

1. Once the session is established, the client sends a `PING` on the audio
   channel to `ipv4:audio_port`.
2. The device verifies the datagram and binds the session's audio flow to
   the datagram's source address and port.
3. The device answers with `PONG` to that address. On receiving it, the
   client knows the audio path works in both directions.

The device MUST NOT send `AUDIO_RX` before the flow is bound.

While it is not sending audio, the client MUST repeat the `PING` every
500 ms. This keeps NAT and firewall state alive and lets the device detect a
change of source port. When the device receives a valid datagram of the
current session from a different address, it MUST rebind the flow to that
address. This allows the session to survive a change of network.

### 4.3 Rationale for Two Channels *(informative)*

Audio and control have opposite reliability requirements.

Audio does not benefit from reliability. A 20 ms block that arrives 100 ms
late is useless and is replaced by concealment. TCP cannot drop data: a lost
segment is retransmitted and everything behind it waits, so latency grows
with every loss and does not recover.

Control requires reliability. A lost `ANSWER` is an unanswered call; a lost
`CALL_SETUP` leaves the client in a wrong state indefinitely. Over UDP this
would require reimplementing retransmission.

A single channel would sacrifice one of the two requirements. Two channels
cost one additional socket.

### 4.4 Multiplexing on a Single Stream (`USB`)

On a single byte stream, both channels are multiplexed with these rules:

- two queues, control and audio; the control queue MUST always be served
  first;
- `HELLO`, `BYE`, `EVENT`, `ACK` and `CMD` MUST NOT be dropped;
- when more than 60 ms of audio is queued, the oldest audio frames MUST be
  dropped;
- `LOG`, `STATS` and `CSTATS` MAY be dropped when the queue is full;
- a frame MUST be written completely or not at all.

The `session_id` prefix of §4.2 is not used on `USB`, and audio flow binding
(§4.2.1) does not apply. The device still assigns a `session_id` in its
`HELLO` for uniformity, but resume (§5.4) does not apply: when the cable is
disconnected the client starts a new session. The token is not used on
`USB` (§11.4).

### 4.5 Ports

Clients MUST NOT assume port numbers; they MUST read them from `NET_STATE`
(§3.2). The ports are configurable to avoid conflicts on shared networks.

---

## 5. Frames and Sessions

### 5.1 Frame Format

```
 offset  size  field
 ────────────────────────────────────────────────────────────
   0      1    TYPE     frame type (§5.2)
   1      1    FLAGS    reserved; MUST be 0 on send, ignored on receipt
   2      2    LEN      payload length, 0..1024
   4    LEN    PAYLOAD
```

The total frame size is `LEN + 4`. The maximum payload is 1024 bytes; the
maximum frame is 1028 bytes.

There is no preamble, checksum or sequence number. All transports deliver
intact bytes (TCP and UDP have checksums, the serial line has parity, and
both channels are authenticated, §11). Loss on the control channel cannot
occur, and audio loss is detected from `ts` (§6.3).

A desynchronisation of the control channel can only result from a bug. The
response MUST be to close the channel and reconnect, not to resynchronise.

#### Receive Rules

- The first frame received on a new control channel MUST be a valid `HELLO`
  (§5.3); otherwise the receiver MUST close the channel.
- A frame of unknown `TYPE` MUST be ignored, skipping `LEN` bytes.
- A frame of known `TYPE` received on the wrong channel MUST be ignored.
- A payload shorter than its `TYPE` requires is a fatal error on the
  control channel (the device sends `EVENT ERROR` code 2 if possible, then
  closes) and MUST be silently discarded on the audio channel.
- Bytes beyond the defined fields of a known payload MUST be ignored. This
  allows later versions to extend payloads.

### 5.2 Frame Types

| TYPE | name | direction | channel | payload |
|------|------|-----------|---------|---------|
| `0x01` | `HELLO` | both | control | §5.3 |
| `0x02` | `PING` | both | both | arbitrary, at most 1024 bytes |
| `0x03` | `PONG` | both | both | copy of the `PING` payload |
| `0x05` | `BYE` | both | control | §5.6 |
| `0x10` | `AUDIO_RX` | device → client | audio | §6.3 |
| `0x11` | `AUDIO_TX` | client → device | audio | §6.3 |
| `0x20` | `EVENT` | device → client | control | §8 |
| `0x21` | `CMD` | client → device | control | §9 |
| `0x22` | `ACK` | device → client | control | §9.1 |
| `0x30` | `LOG` | device → client | control | UTF-8 text, not NUL-terminated, at most 256 bytes |
| `0x31` | `STATS` | device → client | control | §10.1 |
| `0x32` | `CSTATS` | client → device | control | §10.2 |

### 5.3 `HELLO` (`0x01`)

Both peers MUST send `HELLO` as the first frame on the control channel,
immediately after the connection is established, without waiting for the
peer's `HELLO`.

```
 offset  size  field
 ────────────────────────────────────────────────────────────────────────
   0      4    magic       "HERA" (48 45 52 41)
   4      1    version     0x04
   5      1    role        0 = client, 1 = device
   6      1    formats     bitmap of supported audio formats (§6.2)
   7      1    durations   bitmap of supported block durations (§6.2)
   8      1    flags       bit 0: resume request (§5.4); other bits 0
   9      3    reserved    zero
  12      4    session_id  client: 0 for a new session, or the session to
                           resume if flags bit 0 is set;
                           device: the assigned session_id
  16     32    pubkey      ephemeral X25519 public key (§11.3)
  48     16    tag         truncated HMAC-SHA256 keyed with the token
                           (§11.4)
  64      …    software    optional UTF-8 string: software name and version
```

The minimum payload is 64 bytes; a shorter `HELLO` is a fatal error.

The token is not carried in `HELLO`. It is only used as the HMAC key for
the tag (§11.4).

`HELLO` is sent in the clear. It contains no secret, and no keys exist yet.
Every later frame on the control channel is encrypted (§11.2), with the
single exception given below.

Processing rules:

- If `magic` or `version` does not match, the receiver MUST close the
  channel. There is no fallback to an earlier version.
- If the tag does not verify, the device MUST close the channel.
- A client `HELLO` whose `durations` does not include bit 0 (20 ms), or
  whose `formats` does not include both bit 0 (`FMT_PCM16_8K`) and bit 2
  (`FMT_G711A`), is non-conforming; the device MUST close the channel.
- In the device `HELLO`, `formats` lists the formats the device supports. The
  client uses it to decide what it may request with `CMD SET_FORMAT`.
- A `HELLO` received on an established session is a fatal error.

Before closing because of an invalid client `HELLO`, the device MAY send one
`EVENT ERROR` frame unencrypted: code 1 for a `magic`, `version`, format or
duration error, code 5 for a tag that does not verify. This is the only
frame other than `HELLO` that can appear in the clear. Since it is not
authenticated, a client MUST NOT act on it other than by treating the
connection as failed.

### 5.4 `READY`, State Snapshot and Resume

After receiving the client `HELLO` and verifying its tag, the device MUST
send its own `HELLO` and derive the session keys (§11.3). It then sends,
encrypted:

1. `EVENT READY`, with `a1` = protocol version (4), `a2` = selected audio
   format (§6.2), and the firmware version as text;
2. the snapshot of the current state (§8.1), containing only the events that
   are true at that moment.

The client MUST NOT interpret the snapshot as transitions that just
occurred. For example, it MUST NOT start ringing on a `CALL_SETUP` received
in the snapshot without `RING`.

The device MUST NOT send `AUDIO_RX` before the audio flow is bound
(§4.2.1).

#### Resume

On IP, the control channel can drop for reasons unrelated to the call: WiFi
reassociation, roaming, the phone switching radios. The call MUST NOT end
because of it.

When the control channel drops without `BYE`, the device:

- MUST NOT end the HFP call and MUST NOT close the SCO link;
- MUST keep the session for the resume window, `HERA_RESUME_WINDOW_S`
  (default 10 s);
- MUST stop sending `AUDIO_RX` and MUST send silence to the phone;
- when the window expires, MUST discard the session, invalidate its
  `session_id`, and apply the configured orphan policy, `HERA_ON_ORPHAN`:
  `silence` (default) or `hangup`.

To resume, the client reconnects the control channel and sends a `HELLO`
with `flags` bit 0 set and the previous `session_id`. The device answers:

- resume accepted: `EVENT READY` with the same `session_id`, then the
  snapshot. The call was never interrupted.
- resume refused (window expired or unknown `session_id`): `EVENT ERROR`
  code 6, then a `READY` for a new session on the same connection. The
  client starts over without reconnecting.

The device MUST restore normal uplink audio when a session is resumed. The
audio flow is bound again by the first `PING` of the resumed session.

A resume uses new ephemeral keys (§11.3).

### 5.5 Keepalive and Peer Timeout

On IP a connection can be dead without either end being notified: a phone
that leaves WiFi range closes nothing.

| channel | sender | interval | frame |
|---|---|---|---|
| control | device | 1 s | `STATS` |
| control | client | 1 s | `CSTATS` |
| audio | client | 500 ms, while not sending audio | `PING` |

The periodic frames act as keepalives; no dedicated timer is needed. A peer
that receives nothing on the control channel for `HERA_PEER_TIMEOUT_S`
(default 5 s) MUST consider the other peer dead and close the channel. The
client then reconnects and resumes; the device enters the resume window.

`PING` and `PONG` are available on both channels and in both directions for
RTT measurement. `PONG` MUST carry an exact copy of the `PING` payload.

### 5.6 `BYE` (`0x05`)

Orderly close. It tells the peer that the close is intentional, so that it
does not attempt to resume.

```
 offset  size  field
 ──────────────────────────────────────
   0      1    reason
   1      …    optional UTF-8 text
```

| reason | meaning | the receiver |
|---|---|---|
| 1 | normal close | MUST NOT resume |
| 2 | busy: another session is in progress | MUST NOT resume; MAY retry later |
| 3 | replaced by a takeover (§5.7) | MUST NOT resume |
| 4 | device reconfiguring or restarting | SHOULD read `SESSION` and `NET_STATE` over BLE, then reconnect |

The sender MAY close the socket immediately after `BYE`. A channel that
closes without `BYE` is the case in which resume is attempted.

### 5.7 Single Session and Takeover

The device serves one session at a time. For a second control connection:

- if its `HELLO` requests a resume of the current session, with a valid
  tag, it is a takeover: the device MUST close the previous connection with
  `BYE` reason 3, bind the session to the new connection and continue. This
  is the normal case of a client that reconnects before the device has
  detected that the previous connection is dead;
- otherwise the device MUST send `BYE` reason 2 on the new connection and
  close it, leaving the current session intact.

The decision MUST be taken after the new `HELLO` has been verified. Closing
the existing connection on `accept()` lets an unauthenticated host
disconnect the client (§11.6).

---

## 6. Audio

### 6.1 Audio Encoding *(informative)*

Audio is not compressed: it is linear PCM or G.711, which is a per-sample
table lookup. The device runs no codec on the audio path.

Each block carries 68 bytes of fixed overhead: 20 bytes of IPv4 header, 8 of
UDP header, 4 of `session_id`, 8 of counter and 16 of Poly1305 tag (§11.2),
plus 4 bytes of frame header and 8 of audio header inside the ciphertext.

With 20 ms blocks (50 datagrams per second in each direction), and assuming
about 28 Mbit/s of usable 802.11n 1x1 throughput:

| format | payload | on the wire | per direction | share of the link |
|---|---|---|---|---|
| `FMT_PCM16_16K` | 640 B | 708 B | 283 kbit/s | ~1.0% |
| `FMT_PCM16_8K` | 320 B | 388 B | 155 kbit/s | ~0.6% |
| `FMT_G711A` | 160 B | 228 B | 91 kbit/s | ~0.3% |

A 16 kbit/s codec would reduce this to about 0.15% of the link, saving less
than half a percentage point, at the cost of:

- device CPU, which is the scarce resource on a single-core ARMv6. Without a
  codec, the real-time path is `recv(SCO)`, accumulate, `sendto()`, and in
  the other direction `recvfrom()`, jitter buffer, `send(SCO)`, with no
  allocation and nothing to tune;
- latency, since a frame-based codec adds algorithmic delay, once for the
  encoder and once for the decoder in each direction;
- speech recognition accuracy. The source is already 8 kHz CVSD telephone
  audio; a second lossy stage degrades formant structure and consonant
  transitions, which acoustic models depend on.

On `USB` bandwidth is a real constraint, and G.711 is used (§2.3).

### 6.2 Formats

| id | bit in `formats` | name | encoding | sample rate | bytes per 20 ms |
|---|---|---|---|---|---|
| `0x01` | 0 | `FMT_PCM16_8K` | PCM16 LE | 8000 | 320 |
| `0x02` | 1 | `FMT_PCM16_16K` | PCM16 LE | 16000 | 640 |
| `0x03` | 2 | `FMT_G711A` | G.711 A-law | 8000 | 160 |
| `0x04` | 3 | `FMT_G711U` | G.711 µ-law | 8000 | 160 |

All formats are mono. The bit in `formats` is `id - 1`; a peer supporting
all formats sends `0x0F`.

`FMT_PCM16_8K` and `FMT_G711A` are mandatory for both device and client. An
implementation that does not support both is not conforming.

#### Block Duration

| duration | bit in `durations` | samples per block at 8 kHz | bytes, PCM16 at 8 kHz |
|---|---|---|---|
| 20 ms | 0 | 160 | 320 |
| 10 ms | 1 | 80 | 160 |
| 5 ms | 2 | 40 | 80 |

20 ms is mandatory for both peers. 10 ms and 5 ms are OPTIONAL.

The client declares the durations it supports in the `HELLO` `durations`
bitmap. The device MUST select the shortest duration supported by both,
unless configured to force a specific supported duration (`HERA_FRAME_MS`).
The selected duration is announced in `EVENT AUDIO_FMT` together with the
format, and is carried in every audio block (§6.3). On `USB` only 20 ms is
used.

*(informative)* The block duration appears four times in the latency budget
(§7). Shorter blocks increase the packet rate (50, 100 or 200 datagrams per
second per direction) and the bandwidth, because the 68 bytes of overhead
are paid per datagram. With `FMT_PCM16_8K`:

| duration | datagrams/s | on the wire | per direction |
|---|---|---|---|
| 20 ms | 50 | 388 B | 155 kbit/s |
| 10 ms | 100 | 228 B | 182 kbit/s |
| 5 ms | 200 | 148 B | 237 kbit/s |

Over WiFi even the worst case is an order of magnitude below what an access
point handles, but on a combined chip sharing airtime with SCO it should be
measured (§1.2).

#### Format Selection

1. The client declares in `HELLO` the formats it supports.
2. The device selects a format among those declared by the client, in this
   order of preference:
   1. the PCM16 format whose rate equals the SCO rate (`FMT_PCM16_8K` for
      8 kHz SCO, `FMT_PCM16_16K` for 16 kHz SCO), which requires no
      conversion;
   2. the other PCM16 format, which requires resampling;
   3. `FMT_G711A`, then `FMT_G711U`.

   On `USB` the order is reversed: G.711 first, then PCM16 (§2.3).

   A device MAY be configured to force a format (`HERA_FMT`); the forced
   format is used only if the client declared it.
3. The device announces the selection in `EVENT READY` (`a2`) and in
   `EVENT AUDIO_FMT` after every `EVENT AUDIO_OPEN`.
4. The client MAY request a different format with `CMD SET_FORMAT` (§9). The
   change takes effect with the next block and is identified by that block's
   `fmt` field.

#### Sample Rate Conversion

The device declares the rate it has and does not convert unless the client
requests a format with a different rate. The client has far more CPU and
normally already resamples on its audio path, so converting 8 to 16 kHz
costs it nothing. The device implements resampling, but uses it only when
the negotiated format's rate differs from the SCO rate.

### 6.3 Audio Payload (`AUDIO_RX` `0x10`, `AUDIO_TX` `0x11`)

An 8-byte header followed by samples:

```
 offset  size  field
 ─────────────────────────────────────────────────────────────────────
   0      1    fmt        format id (§6.2)
   1      1    frame_ms   block duration: 20, 10 or 5 (§6.2)
   2      1    flags      bit 0:    filler (generated, not source audio)
                          bits 1-3: reserved, 0
                          bits 4-7: epoch, incremented at every
                                    AUDIO_OPEN, modulo 16
   3      1    count      number of blocks in the payload (§6.3.1)
   4      4    ts         index of the first sample, at the rate of fmt
   8      …    samples    count × bytes_per_block(fmt, frame_ms) bytes
```

The header is self-describing because an audio datagram and the
`EVENT AUDIO_FMT` that concerns it travel on different channels and can
arrive in either order. Receivers MUST take format and duration from the
block, not from the event.

- `ts` advances by `count × samples_per_block` per frame and restarts at 0
  at every `AUDIO_OPEN`.
- The epoch in the high bits of `flags` disambiguates `ts` restarts. A block
  whose epoch differs from the current one belongs to a new audio stream;
  the receiver MUST reset its buffers and counters without waiting for the
  event.
- `flags` bit 0 marks filler: silence sent to maintain the pacing of §6.4,
  not source audio. Senders MUST set it on filler blocks. It is the only
  indication that lets the receiver shorten its buffer without audible
  effect (§6.5). A sender that sends unmarked silence is not incorrect, but
  prevents the receiver from reducing latency.
- A block whose `frame_ms` differs from the negotiated duration, whose
  `count` is outside the range of §6.3.1, whose `fmt` was not negotiated, or
  whose payload length differs from `8 + count × bytes_per_block` MUST be
  dropped and counted in `ul_bad` (device). The channel MUST NOT be closed.
- The device SHOULD send `count = 1`. On IP the overhead does not justify
  the added latency of batching.

#### 6.3.1 Blocks per Payload

`count` is at least 1 and at most the number of blocks that fit in a 1024-byte
payload, `(1024 - 8) / bytes_per_block`, capped at 4:

| format | 20 ms | 10 ms | 5 ms |
|---|---|---|---|
| `FMT_PCM16_16K` | 640 B → 1 | 320 B → 3 | 160 B → 4 |
| `FMT_PCM16_8K` | 320 B → 3 | 160 B → 4 | 80 B → 4 |
| `FMT_G711A`, `FMT_G711U` | 160 B → 4 | 80 B → 4 | 40 B → 4 |

The cap of 4 limits latency. A sender that has more blocks to send than fit
in one payload (normally only a client compensating for loss, §6.4) MUST
send several frames rather than a larger `count`.

### 6.4 Clocking

The SCO link has a fixed clock. The client MUST NOT pace its uplink with an
independent timer: it would drift relative to the Bluetooth clock, and the
device's jitter buffer would eventually run empty or overflow.

> For every received `AUDIO_RX` frame, the client sends an `AUDIO_TX` with
> the same `count`. For every block detected as lost, it sends one
> additional block.

The client maintains `expected_ts`:

```
on receiving a block (ts, count):
    missing = max(0, (ts - expected_ts) / samples_per_block)
    if 0 < missing <= 8:
        conceal `missing` blocks in playback
        send `missing` additional blocks, batched within the limits of
        §6.3.1, using several frames if needed
    send an AUDIO_TX with the same count
    expected_ts = ts + count * samples_per_block
```

A gap of more than 8 blocks is treated as a restart: `expected_ts` is
realigned without compensation.

When there is no audio to send, the client MUST send silence with `flags`
bit 0 set. The block is sent regardless, so that both ends can verify the
pacing.

#### 6.4.1 Free-Running Fallback

If the downlink stops completely, the client has no pacing reference and
the device's jitter buffer would run empty. Therefore:

- after 60 ms without any `AUDIO_RX` while the audio channel is open, the
  client MUST switch to a local timer with period `frame_ms` and continue
  sending silence;
- on the first block received afterwards, the client MUST resynchronise:
  realign `expected_ts` to the received `ts` and return to the rule of §6.4.

### 6.5 Jitter Buffer and Concealment

The jitter buffer depth is not fixed; it adapts to the jitter observed on
the network. *(informative)* A fixed depth has to be dimensioned for the
worst expected link and costs that latency at all times; in the budget of §7
the two jitter buffers are a large part of what remains after shortening
the block.

Device (uplink, `AUDIO_TX` to SCO):

| parameter | default | reference configuration |
|---|---|---|
| target floor | 20 ms | `HERA_JITTER_PREFILL_MS` |
| target ceiling; beyond it the oldest block is dropped (`ul_late`) | 100 ms | `HERA_JITTER_MAX_MS` |
| underrun | silence for at most 60 ms (`ul_fill`), then refill | |

The target moves between floor and ceiling; `ul_target_ms` in `STATS`
(§10.1) reports it. The floor is never below `frame_ms`.

#### Adaptation Policy

The policy is not normative: each end may adapt as it chooses, and the
sender need not know how. The reference device and client use the same
policy with the same constants, so that the two halves of the path do not
compensate for each other.

- The target follows measured jitter: `target = 3·J + frame_ms`, clamped
  to floor and ceiling, where `J` is a moving average of the deviation from
  the nominal interarrival time, `J += (|D| − J) / 16` as in [RFC3550].
  `J` needs sub-millisecond resolution (the reference implementations keep
  it in units of 1/16 ms, as in [RFC3550] Appendix A.8); in whole
  milliseconds the update rounds to zero for small deviations.
  The additional block covers quantisation against the SCO packet (§7.1). A
  deviation larger than `10·frame_ms` is clamped, so that a stream restart
  does not inflate `J`.
- The buffer grows as soon as its depth is one block below target and
  shrinks only when it is more than two blocks above target.
- An underrun raises the target by two blocks immediately, not through the
  average. For the next 5 s shrinking is disabled and the target is not
  lowered below that value. Without both measures the loop oscillates.
- The buffer shrinks only by discarding a filler block (§6.3, `flags`
  bit 0). Discarding speech causes an audible click.

Client (downlink, `AUDIO_RX` to playback) *(informative)*:

- the same policy, with floor `frame_ms` and ceiling 200 ms; on underrun,
  refill rather than alternating samples and silence;
- on a `ts` gap, conceal the missing duration. Repeating the last block with
  a fade sounds better than silence, but silence is acceptable;
- on iOS, `AVAudioEngine` with `AVAudioSourceNode`, with voice processing
  enabled if the microphone is also used.

### 6.6 Congestion

UDP has no flow control.

> Audio MUST NOT be queued. If the socket does not accept a datagram
> (`EWOULDBLOCK`, `ENOBUFS`), the block MUST be dropped and counted
> (`dl_drop` on the device). It MUST NOT be retried or queued: by the time
> the socket accepts data, the block is stale.

No application-level queue exists on the audio channel. Queues and drop
policies apply only to `USB` (§4.4).

---

## 7. Latency Budget *(informative)*

Round trip phone → device → client → device → phone, excluding processing
in the client application.

| component | 20 ms | 10 ms | 5 ms | adjustable |
|---|---|---|---|---|
| SCO packetisation | ~7.5 | ~7.5 | ~7.5 | no: SCO packet interval |
| accumulation on the device | 20–24 | 10–14 | 5–9 | yes: `frame_ms`, plus quantisation against the SCO packet |
| transit | 1–5 | 1–5 | 1–5 | no: WiFi |
| client jitter buffer | 40 | 20 | 10 | yes, down to the actual network jitter |
| *reverse direction* | | | | |
| accumulation on the client | 20 | 10 | 5 | yes: `frame_ms` |
| transit | 1–5 | 1–5 | 1–5 | no |
| device jitter buffer | 30 | 15 | 10 | yes, down to the actual network jitter |
| SCO packetisation | ~7.5 | ~7.5 | ~7.5 | no |
| **total transport** | **~127–139** | **~72–84** | **~47–59** | |

No component is a codec (§6.1).

### 7.1 Analysis

Four of the eight components scale with `frame_ms`: the two accumulations
and the two jitter buffers. At 20 ms they account for 110–114 ms of the
127–139 ms total. This is why the duration is negotiated (§6.2), and why
5 ms blocks reduce the total by more than half.

The other four components are outside the protocol's control: SCO
packetisation is determined by the Bluetooth controller (a CVSD packet
carries 3 to 3.75 ms of audio, and the budget counts two per direction), and
transit is determined by WiFi.

Accumulation is quantised to the SCO packet. A 5 ms block does not align
with a 3.75 ms packet: sometimes one packet is not enough, sometimes two
leave a remainder. The cost is at most one extra packet, which is the range
shown. Full alignment would require `frame_ms = 7.5`, which is not an
integer number of milliseconds.

The jitter buffers absorb network jitter, so their floor is set by the
jitter and not by `frame_ms`: with 10 ms of peak jitter on a WiFi hop,
10 ms of buffer is needed regardless of the block duration. Their rows in
the table are indicative values and cannot be predicted from configuration.
On a clean link the adaptation of §6.5 reaches the floor; on a poor link it
rises to the ceiling. `ul_target_ms` next to `ul_buffered_ms` in `STATS`
(§10.1) shows the value actually reached.

Shortening the block always reduces latency, because the accumulations are
`frame_ms` by definition. Reducing the buffers reduces latency only as far
as the network allows, which is found by measurement.

SCO priming (`HERA_SCO_PRIME_PKTS`) queues silent packets ahead of the
stream. It is permanent latency, not a start-up buffer: every subsequent
sample is delayed by that number of packet intervals (~3.75 ms each). It was
useful when an encoder could miss deadlines on a single core; without a
codec (§6.1) the default is 0.

---

## 8. Events (`EVENT`, `0x20`)

A 4-byte header followed by an optional UTF-8 string, not NUL-terminated,
of length `LEN - 4`.

```
 offset  size  field
 ──────────────────────────
   0      1    id
   1      1    a1
   2      2    a2
   4      …    text (optional, UTF-8)
```

| id | name | a1 | a2 | text |
|----|------|----|----|------|
| `0x01` | `BT_CONNECTED` | | | phone address `AA:BB:CC:DD:EE:FF` |
| `0x02` | `BT_DISCONNECTED` | | | phone address |
| `0x03` | `SLC_UP` | | | phone address; HFP service level connection established |
| `0x10` | `AUDIO_OPEN` | SCO codec: 1 = CVSD, 2 = mSBC | SCO sample rate / 100 | |
| `0x11` | `AUDIO_CLOSE` | | | |
| `0x12` | `AUDIO_FMT` | format id (§6.2) | `frame_ms \| (epoch << 8)` | |
| `0x20` | `RING` | | | |
| `0x21` | `CLIP` | 0 = call, 1 = call waiting | | caller number |
| `0x22` | `CALL` | 0 = no call, 1 = call active | | |
| `0x23` | `CALL_SETUP` | 0 none, 1 incoming, 2 outgoing (dialling), 3 outgoing (alerting) | | |
| `0x24` | `CALL_HELD` | HFP `callheld` indicator value | | |
| `0x30` | `VOLUME` | 0 = speaker, 1 = microphone | level 0..15 | |
| `0x40` | `VR` | voice recognition 0/1 | | |
| `0x50` | `SERVICE` | network availability | | |
| `0x51` | `SIGNAL` | signal strength 0..5 | | |
| `0x52` | `BATTERY` | battery level 0..5 | | |
| `0x53` | `OPER` | | | operator name |
| `0x60` | `READY` | protocol version (4) | selected format id | firmware version |
| `0x70` | `NET_MODE` | link mode (§2) | | SSID, if applicable |
| `0x7F` | `ERROR` | | error code (§8.2) | description |

`AUDIO_OPEN` reports the codec and rate of the SCO link. The wire format is
reported by `AUDIO_FMT`, which the device MUST send immediately after.
`AUDIO_OPEN` is also the point at which the client resets its buffers and
`ts` counters, and at which the epoch is incremented.

### 8.1 State Snapshot

After `READY`, the device MUST send the events that describe the current
state, in this order, and only those that are true at that moment:

1. `NET_MODE`;
2. `BT_CONNECTED` and, if the service level connection is up, `SLC_UP`;
3. `SERVICE`, `SIGNAL`, `BATTERY`, `OPER`, if known;
4. `CALL_SETUP` if not 0, `CALL`, `CALL_HELD` if not 0;
5. `AUDIO_OPEN` and `AUDIO_FMT`, if the SCO link is open.

### 8.2 `ERROR` Codes

| code | meaning |
|---|---|
| 1 | invalid `HELLO`: magic, version, formats or durations |
| 2 | payload of a known frame type too short |
| 3 | SCO audio error (open or I/O failure) |
| 4 | HFP service error (Bluetooth stack or call handling) |
| 5 | invalid session token: `HELLO` tag does not verify (§11.4) |
| 6 | resume refused: session expired or unknown (§5.4) |
| 7 | network error: mode change failed, see `NET_STATE.fail` |

---

## 9. Commands (`CMD`, `0x21`)

Same 4-byte header as events, with the same optional trailing string.

| id | name | a1 | a2 | text |
|----|------|----|----|------|
| `0x01` | `ANSWER` | | | |
| `0x02` | `HANGUP` | | | rejects an incoming call or ends the active call |
| `0x03` | `DIAL` | | | number to dial |
| `0x04` | `REDIAL` | | | |
| `0x10` | `AUDIO_CONNECT` | | | requests the SCO link to be opened |
| `0x11` | `AUDIO_DISC` | | | |
| `0x20` | `VOL_SPK` | | level 0..15 | |
| `0x21` | `VOL_MIC` | | level 0..15 | |
| `0x30` | `AUTO_ANSWER` | 0/1 | delay in ms (0 = unchanged) | |
| `0x40` | `CONNECT` | | | phone address `AA:BB:CC:DD:EE:FF` |
| `0x41` | `DISCONNECT` | | | disconnects the phone, not the client |
| `0x50` | `VR_START` | | | |
| `0x51` | `VR_STOP` | | | |
| `0x60` | `SET_FORMAT` | format id (§6.2) | | |
| `0x61` | `MUTE_UL` | 1 = send silence to the phone | | |
| `0x70` | `STATS` | | | requests an immediate `STATS` frame |
| `0x71` | `LOG_LEVEL` | 0 off, 1 errors, 2 info, 3 debug | | default 1 |
| `0x72` | `NET_MODE` | 1 = AP, 2 = STA | delay in ms (0 = 500) | for STA: `"ssid\tpsk"` |
| `0x7F` | `REBOOT` | | | restarts the HERA service |

While `MUTE_UL` is active, the device sends silence to the phone regardless
of the received `AUDIO_TX`. The client continues to send `AUDIO_TX` at the
pace of §6.4.

A `SET_FORMAT` for a format the client did not declare in `HELLO`, or that
the device does not support, MUST be refused with `ACK a1 = 1` and MUST NOT
change the format.

`NET_MODE` performs over the control channel what `NET_CFG` performs over
BLE (§3.3), for cases where BLE is unavailable, typically on `USB`. It takes
down the link it arrived on if that link is the network being reconfigured.
The device MUST therefore send `ACK`, then `BYE` reason 4, and apply the
change after `a2` ms.

### 9.1 `ACK` (`0x22`)

The device MUST answer every `CMD` with an `ACK` with the same 4-byte
header:

- `id`: the command identifier;
- `a1`: 0 if the device accepted the command, 1 otherwise (unknown command,
  parameter out of range, or failure of the underlying operation).

The `ACK` reports the outcome on the device, not the effect on the phone.
The effect is reported by `EVENT`s (`CALL`, `AUDIO_OPEN`, `AUDIO_FMT`,
`NET_MODE`), which are the only authoritative source of state.

---

## 10. Statistics

### 10.1 `STATS` (`0x31`), Device to Client

Sent once per second on the control channel after the `HELLO` exchange, and
on request with `CMD STATS`. 44 bytes. A "block" is one audio block of the
negotiated duration (§6.2).

| offset | type | field | meaning |
|--------|------|-------|---------|
| 0  | u32 | `uptime_ms` | milliseconds since the service started |
| 4  | u32 | `dl_blocks` | audio blocks sent to the client |
| 8  | u32 | `ul_blocks` | audio blocks received from the client |
| 12 | u32 | `dl_drop` | blocks not sent because the socket did not accept them (§6.6) |
| 16 | u32 | `ul_lost` | client blocks missing, from `ts` gaps |
| 20 | u32 | `ul_late` | client blocks received and dropped at the buffer ceiling |
| 24 | u32 | `ul_fill` | silence blocks generated on underrun |
| 28 | u32 | `ul_bad` | malformed blocks dropped (§6.3) |
| 32 | u16 | `ul_buffered_ms` | uplink jitter buffer depth |
| 34 | u16 | `ul_jitter_ms` | uplink interarrival jitter, moving average |
| 36 | u8  | `sco_codec` | 0 none, 1 CVSD, 2 mSBC |
| 37 | u8  | `flags` | see below |
| 38 | u8  | `cpu_pct` | CPU usage of the HERA process, 0..100 |
| 39 | i8  | `rssi` | client RSSI in dBm; 0 = unknown |
| 40 | u8  | `net_mode` | link mode (§2) |
| 41 | u8  | `fmt` | audio format in use |
| 42 | u8  | `ul_target_ms` | current target depth of the uplink jitter buffer (§6.5) |
| 43 | u8  | reserved | 0 |

Counters are reset when the service starts, not per session.

`ul_lost` counts blocks that did not arrive (network). `ul_late` counts
blocks that arrived and were dropped because the buffer was at its ceiling.
Filler blocks discarded on purpose to shrink the buffer (§6.5) are not
counted in `ul_late`; that discard is part of normal operation and occurs
continuously during a call.

`ul_target_ms` and `ul_buffered_ms` should track each other. A target held at
the ceiling indicates a link worse than the ceiling allows for.

`flags` bits:

| bit | meaning |
|---|---|
| 0 | `SLC`: HFP service level connection established |
| 1 | SCO audio link open |
| 2 | call in progress |
| 3 | the underlying link is protected: WPA2 in `AP`, cable in `USB` |

Bit 3 is diagnostic. IP traffic is encrypted and authenticated in every mode
(§11.1); the bit only describes the layer below.

### 10.2 `CSTATS` (`0x32`), Client to Device

Sent once per second on the control channel. 20 bytes.

The receive-side counters exist only on the client. With `CSTATS` the
device log contains both sides (§13.2).

| offset | type | field | meaning |
|--------|------|-------|---------|
| 0  | u32 | `uptime_ms` | milliseconds since the session started |
| 4  | u32 | `dl_blocks` | `AUDIO_RX` blocks received |
| 8  | u32 | `dl_lost` | blocks missing, from `ts` gaps |
| 12 | u32 | `ul_blocks` | `AUDIO_TX` blocks handed to the socket |
| 16 | u16 | `dl_buffered_ms` | playback buffer depth |
| 18 | u8  | `rtt_ms` | last measured RTT, saturated at 255 |
| 19 | u8  | `flags` | bit 0 playing, bit 1 microphone active, bit 2 free-running (§6.4.1) |

The device MUST NOT depend on `CSTATS` for its operation. A client that
does not send `CSTATS` is conforming, but loses the comparison of §13.2.

---

## 11. Security

### 11.1 Scope

All IP traffic, on both channels and in both directions, MUST be encrypted
and authenticated with ChaCha20-Poly1305 [RFC8439] using keys derived from
an ephemeral X25519 exchange (§11.3). Encryption is not optional and there
is no unencrypted fallback: a fallback is a downgrade path an attacker can
force.

| mode | encryption | reason |
|---|---|---|
| `AP` | required | WPA2-PSK does not protect stations from each other |
| `STA` | required | the network is not trusted |
| `USB` | none | point-to-point cable |

*(informative)* In WPA2-PSK all stations share the same PMK, and the
pairwise keys are derived from it and from nonces and addresses exchanged
in the clear during the 4-way handshake. Anyone who knows the passphrase,
for example a phone that was authorised in the past, can derive the keys of
any other station, and can force a new handshake with a deauthentication
frame. WPA2 protects against outsiders, not between stations.

### 11.2 Record and Datagram Protection

`HELLO` is sent in the clear (§5.3). From the next frame on, on each
channel:

**Control channel (TCP).** The byte stream becomes a stream of records. The
plaintext of the records is the frame stream of §4.1.1.

```
 offset  size  field
 ──────────────────────────────────────────────────────────────
   0      2    rec_len     (u16) = 8 + len(ciphertext) + 16
   2      8    counter     (u64, per direction, strictly increasing)
  10      n    ciphertext
 10+n    16    tag         Poly1305

 AAD       = bytes 0..9 (rec_len ‖ counter)
 nonce     = 00 00 00 00 ‖ counter                  (12 bytes)
 plaintext = one or more complete HERA frames
```

The maximum `rec_len` is 2080: two maximum-size frames (2056 bytes) plus
the 24 bytes of counter and tag counted by `rec_len`. A complete record is
therefore at most 2082 bytes.

A `rec_len` out of range, a `counter` not greater than the previous one, or
a tag that does not verify MUST be treated as a fatal error and the channel
closed. TCP guarantees ordering, so any of these indicates an attack or a
bug.

**Audio channel (UDP).** The session prefix of §4.2 is extended with the
counter.

```
 offset  size  field
 ──────────────────────────────────────────────────────────────
   0      4    session_id  (u32)
   4      8    counter     (u64, per direction, increasing)
  12      n    ciphertext  (exactly one HERA frame)
 12+n    16    tag         Poly1305

 AAD   = bytes 0..11 (session_id ‖ counter)
 nonce = 00 00 00 00 ‖ counter
```

Anti-replay window: the receiver keeps the highest counter accepted and a
bitmap of the 64 counters below it, as in [RFC4303] §3.4.3. A datagram is
accepted if its counter is above the highest, or within the window and not
yet seen; otherwise it MUST be discarded silently (and counted in `ul_bad`
on the device). The window MUST be updated only after the tag has been
verified. Duplicates can be produced by the network, and without the window
an attacker could replay earlier audio.

Counters are independent per key, that is, per channel and per direction:
four counters and four keys, so no nonce is ever reused under one key.
Counters start at 0 and MUST NOT decrease.

*(informative)* The overhead is 24 bytes per audio datagram (counter and
tag, in addition to the existing `session_id`) and 26 bytes per control
record (`rec_len`, counter and tag), where one record can carry several
frames. For `FMT_PCM16_8K`, a datagram grows from 364 to 388 bytes at 20 ms
(146 to 155 kbit/s) and from 124 to 148 bytes at 5 ms (198 to 237 kbit/s).
In the worst case, 5 ms blocks, this remains under 1% of the link (§6.1).

### 11.3 Key Derivation

Each peer generates a new ephemeral X25519 [RFC7748] key pair for every
control connection, including resumes. The public keys are exchanged in the
two `HELLO` frames (§5.3). Private keys MUST NOT leave memory and MUST be
discarded when the connection ends.

```
shared = X25519(own_private, peer_public)
prk    = HKDF-Extract(salt = pub_client ‖ pub_device, IKM = shared)

K_ctrl_c2d  = HKDF-Expand(prk, "hera/v4/ctrl/c2d",  32)
K_ctrl_d2c  = HKDF-Expand(prk, "hera/v4/ctrl/d2c",  32)
K_audio_c2d = HKDF-Expand(prk, "hera/v4/audio/c2d", 32)
K_audio_d2c = HKDF-Expand(prk, "hera/v4/audio/d2c", 32)
```

HKDF uses SHA-256 [RFC5869]. The `info` strings are ASCII, without a
terminator. The salt is the concatenation of the two public keys, always in
the order client, device, regardless of which `HELLO` arrived first. A peer
MUST reject a peer public key that yields an all-zero shared secret
(low-order point).

`c2d` keys protect client-to-device traffic and `d2c` keys device-to-client
traffic.

*(informative)* Ephemeral keys provide forward secrecy: an attacker who
records traffic and later obtains the token cannot decrypt it. Keys derived
from the token alone would not have this property; a compromised token
would expose all past sessions.

*(informative)* ChaCha20-Poly1305 is used instead of AES-GCM because the
ARMv6 core of the Raspberry Pi Zero W has no AES instructions. In software
ChaCha20 is faster and free of table lookups, whereas table-based AES is
exposed to cache timing side channels. The cost is a few cycles per byte on
about 25 kB/s of audio.

### 11.4 Token and `HELLO` Authentication

The token is a long-term secret of 16 random bytes, generated by the device
on first start and stored persistently (reference implementation:
`HERA_TOKEN_FILE`, default `/var/lib/hera/token`, mode `0600`). It is
delivered to the client through the `SESSION` characteristic (§3.4) and
MUST NOT be sent over the IP link.

The token authenticates the key exchange, preventing an on-path attacker
from substituting its own public key. Each peer computes a tag over its
`HELLO`:

```
tag = HMAC-SHA256(key = token,
        "hera/v4/hello" ‖ role ‖ formats ‖ durations ‖ flags ‖
        session_id ‖ pubkey
      )[0..16]
```

`role`, `formats`, `durations` and `flags` are single bytes as sent in
`HELLO`; `session_id` is 4 bytes, little endian; `pubkey` is 32 bytes. The
tag is the first 16 bytes of the HMAC output [RFC2104].

The tag binds the public key to the token. An on-path attacker cannot
replace a public key without being able to compute the tag; it can only
relay both `HELLO` frames unchanged, in which case the peers derive keys it
does not know. This allows both `HELLO` frames to be sent simultaneously, as
§5.3 requires, without an additional round trip.

Each peer MUST verify the peer's tag before deriving keys, and MUST compare
tags in constant time. If the client tag does not verify, the device sends
`EVENT ERROR` code 5 (§5.3) and closes the channel without further
information. If the device tag does not verify, the client closes the
channel.

The token is not rotated (§3.3.1). It can be changed by deleting the stored
token and restarting the device, which requires access to the device.

On `USB` the token is not used: both peers compute and verify `HELLO` tags
with a token of 16 zero bytes, and no encryption is applied (§11.1).

### 11.5 Handshake Sequence

```
1. client opens the TCP connection
2. client → device : HELLO, cleartext (pubkey_c, tag_c, formats, durations)
3. device → client : HELLO, cleartext (pubkey_d, tag_d, assigned session_id)
4. each peer verifies the other's tag with the token;
   on failure: ERROR 5 (device only) and close
5. both derive the four keys (§11.3)
6. device → client : first record: EVENT READY and the state snapshot
7. client → device : first record
8. client binds the audio flow with the first PING (§4.2.1)
```

Steps 2 and 3 are not ordered with respect to each other. After step 5
nothing is sent in the clear.

### 11.6 Security Considerations

- **BLE is the root of trust.** Encryption protects everything after
  provisioning, not provisioning itself. An attacker who obtains the token
  when it is delivered can later attack the IP exchange as an on-path
  attacker. Bluetooth 4.1 hardware such as the Raspberry Pi Zero W supports
  only LE legacy pairing, and "Just Works" provides no protection against
  an eavesdropper who captures the pairing exchange. Narrowing this window
  is a provisioning matter, not a transport matter: for example a pairing
  window enabled by a button, or initial provisioning over `USB`, where
  there is no radio to observe.
- **Encryption is not authorisation.** Any host that can reach the control
  port can open connections. §5.7 requires the takeover decision to be taken
  after the new `HELLO` has been verified; an implementation that closes
  the existing connection on `accept()` lets an unauthenticated host
  disconnect the client.
- **The unauthenticated `EVENT ERROR`** that a device MAY send before keys
  exist (§5.3) can be forged by an on-path attacker. It can only cause the
  client to abandon a connection that the attacker could also disrupt by
  other means.
- **Denial of service remains possible.** An attacker can flood the ports;
  packets are rejected at the tag check, which is cheap, but they consume
  airtime. On WiFi, unprotected deauthentication frames achieve the same
  with less effort.
- **Traffic analysis.** Encryption does not hide that a call is in progress,
  its duration or its packet timing.
- **Privileges.** The reference device runs as root. It MUST NOT execute
  strings received from the protocol. SSIDs and passphrases received in
  `NET_CFG` or `CMD NET_MODE` MUST be validated (1 to 32 characters for an
  SSID, 8 to 63 for a passphrase, printable ASCII only, excluding `"` and
  `\`) and MUST only be written to configuration files, never passed to a
  shell.

---

## 12. Implementation Notes *(informative)*

### 12.1 Device: Audio Path

The audio thread runs with `SCHED_FIFO` priority and does nothing else:

```
loop:
    n = recv(sco_fd, pcm_in, ...)          /* blocks: this is the clock */
    send(sco_fd, jitter_pop(), n)          /* answer first */
    accumulate pcm_in into blocks of frame_ms
    for each complete block: sendto(audio_fd, frame)   /* non-blocking */
```

No allocation takes place: buffers and jitter buffer slots are preallocated.
The blocking `recv()` on the SCO socket keeps both directions locked to the
controller clock, which an application timer cannot guarantee.

### 12.2 Device: SCO over HCI

On the Broadcom controllers used by the Raspberry Pi, SCO audio may be routed
to the chip's PCM pins instead of HCI. Routing over HCI is configured with a
vendor command (`Write_SCO_PCM_Int_Param`, OGF 0x3F, OCF 0x01C, first
parameter `0x01` = transport). The reference device issues:

```
hcitool cmd 0x3F 0x1C 0x01 0x00 0x00 0x00 0x00
```

Variants with different PCM parameters are also in circulation; with HCI
routing the remaining parameters have no effect on most firmware. Verify on
the target firmware that samples arrive on the SCO socket after the command.

The reference device reads raw PCM16 from the SCO socket, which is what the
kernel delivers for CVSD. With mSBC the socket would carry a transparent
stream that must be decoded. The device therefore advertises only CVSD
(`AT+BAC=1`) and its SDP record does not announce wideband speech, so that
the phone does not negotiate mSBC.

### 12.3 Client: Binding Sockets to the Device Network

This is a common source of failures that do not appear during development.

- **Android**: `WifiNetworkSpecifier` with
  `ConnectivityManager.requestNetwork` gives a local-only network that never
  becomes the default, but requires a system dialog and is therefore not
  suitable for a service that must reconnect on its own.
  `WifiNetworkSuggestion` persists and reconnects automatically, but the
  network becomes a candidate for the default route. A working combination
  is `WifiNetworkSuggestion` for persistence plus `Network.bindSocket` (or
  `bindProcessToNetwork` in the transport isolate) for the HERA sockets,
  with `WifiNetworkSpecifier` as a fallback when the user is present.
  Permissions: `CHANGE_NETWORK_STATE`, `ACCESS_NETWORK_STATE`, and
  `NEARBY_WIFI_DEVICES` from Android 13.
- **iOS**: join the network with `NEHotspotConfiguration`
  (`joinOnce: false`, so that it is remembered). This requires the
  `com.apple.developer.networking.HotspotConfiguration` entitlement.
  `NSLocalNetworkUsageDescription` is required in `Info.plist`; from iOS 14,
  local network sockets fail silently without it.
- **Background execution**: the client must run during the call. Android
  requires a foreground service. On iOS the `audio` background mode with an
  active `AVAudioSession` is appropriate, since the client plays and records
  audio, possibly together with CallKit. This should be prototyped early.

---

## 13. Audio Diagnostics *(informative)*

Diagnose with rates, not absolute values: take two consecutive `STATS`
frames and compute the difference. The reference counter is `dl_blocks`:

> With the SCO link open, `dl_blocks` increases by 1000 / `frame_ms` per
> second (50 per second with 20 ms blocks), regardless of SCO codec and
> negotiated format.

### 13.1 Symptoms

| symptom | indicators | cause | remedy |
|---|---|---|---|
| slow and low-pitched, like a tape played too slowly | `dl_blocks` at the nominal rate, `dl_drop` steady | client plays at the wrong sample rate | take `fmt` from the block (§6.3), not from `AUDIO_OPEN.a2`, which is the SCO rate divided by 100 |
| choppy, with gaps | `CSTATS.dl_lost` increasing, `dl_drop` steady | network loss | §1.2: `rssi`, WiFi channel, SCO coexistence; try G.711 |
| choppy | `dl_drop` increasing | device socket rejects datagrams | interface saturated: check `rssi` and channel |
| choppy | `dl_blocks` below nominal, `cpu_pct` high | device cannot keep real time | thread priority, blocking I/O on the audio path, external load |
| choppy | all counters regular on both sides | client player underrun | buffer 20–40 ms before playback (§6.5) |
| choppy only for the remote party | `ul_blocks` below `dl_blocks` | client does not follow §6.4 | one `AUDIO_TX` per `AUDIO_RX`, plus one per lost block |
| " | `ul_fill` increasing | device jitter buffer runs empty | `CSTATS.flags` bit 2: if the client is free-running, the downlink stopped; otherwise raise `HERA_JITTER_PREFILL_MS` |
| " | `ul_lost` increasing | uplink network loss | as above for the network |
| " | `ul_late` increasing and `ul_buffered_ms` at the ceiling | client sends too fast: local timer instead of §6.4 | §6.4 |
| " | `ul_bad` increasing | malformed blocks: wrong `fmt`, `count` or length | §6.3 |
| latency grows and does not recover | `ul_buffered_ms` rises and stays high | audio is being queued instead of dropped | §6.6 |
| silent | `flags` bit 1 is 0 | SCO link not open | HFP issue, not transport |
| silent | no `AUDIO_RX`, but `STATS` arrives | audio flow not bound | client did not send `PING` on the audio channel (§4.2.1), or the datagram does not reach the device |
| no `STATS` | | `HELLO` not received or invalid, or client not reading the socket | |
| connects and drops immediately | `EVENT ERROR` 5 | wrong token | read `SESSION` again over BLE (§3.4) |

### 13.2 Comparing Both Sides

The difference between `STATS.dl_blocks` and `CSTATS.dl_blocks` over the
same second shows where blocks are lost (nominal rate N = 1000 / `frame_ms`):

| `STATS.dl_blocks` | `CSTATS.dl_blocks` | conclusion |
|---|---|---|
| N | N | transport is fine; any problem is in the client |
| N | < N | downlink network loss |
| < N | equal to `STATS.dl_blocks` | device does not produce: SCO or scheduling |
| < N | < `STATS.dl_blocks` | device does not produce, and blocks are lost |

The same comparison between `CSTATS.ul_blocks` and `STATS.ul_blocks` applies
to the uplink.

---

## 14. Test Vectors

Exact bytes, for testing encoders and parsers without hardware. Both
reference implementations check these vectors in their test suites.

The token does not appear in any of these vectors. It only enters the `tag`
(§11.4). A `HELLO` that contains the token is not a version 4 `HELLO`.

The three `HELLO` vectors use the public keys of §14.1 (from [RFC7748]), so
that the tags are reproducible: token `00 01 … 0f` for the client, an
all-zero token for the device, all formats (`0x0F`) and all durations
(`0x07`).

**Client `HELLO`**: new session, software `"hera_client"`. Payload 75 bytes.

```
01 00 4b 00
48 45 52 41 04 00 0f 07 00 00 00 00 00 00 00 00
85 20 f0 09 89 30 a7 54 74 8b 7d dc b4 3e f7 5a
0d bf 3a 0d 26 38 1a f4 eb a4 a9 8e aa 9b 4e 6a
da d8 a1 51 91 05 b1 d2 7f 84 02 34 95 ed 1f 31
68 65 72 61 5f 63 6c 69 65 6e 74
```

Header `01 00 4b 00` (`TYPE`, `FLAGS`, `LEN = 0x004b = 75`); magic
`"HERA"`; `version = 04`; `role = 00`; `formats = 0f`; `durations = 07`;
`flags = 00`; 3 reserved bytes; `session_id = 00000000`; `pubkey` (32 bytes,
client public key); `tag` (16 bytes); `"hera_client"`.

**Device `HELLO`**: `session_id = 0x11223344`, firmware `"hera-rpi 0.3.0"`.
Payload 78 bytes.

```
01 00 4e 00
48 45 52 41 04 01 0f 07 00 00 00 00 44 33 22 11
de 9e db 7d 7b 7d c1 b4 d3 5b 61 c2 ec e4 35 37
3f 83 43 c8 5b 78 67 4d ad fc 7e 14 6f 88 2b 4f
4a f4 e6 7c 6f c2 bd 17 e5 d8 82 56 17 3c 89 b3
68 65 72 61 2d 72 70 69 20 30 2e 33 2e 30
```

**Client `HELLO`, resume** of `0x11223344`: `flags = 0x01`, no software
string. Payload 64 bytes (fixed part only).

```
01 00 40 00
48 45 52 41 04 00 0f 07 01 00 00 00 44 33 22 11
85 20 f0 09 89 30 a7 54 74 8b 7d dc b4 3e f7 5a
0d bf 3a 0d 26 38 1a f4 eb a4 a9 8e aa 9b 4e 6a
1d fe 17 9e 8c a6 91 ce 86 b8 8c 55 e7 a4 d3 ba
```

The tag differs between the first and the third vector although key and
token are the same: `flags` and `session_id` are authenticated (§11.4), so a
resume request cannot be derived from a new-session `HELLO`.

**`EVENT READY`**: `a1 = 4`, `a2 = 0x0001` (`FMT_PCM16_8K`), firmware
`"hera-rpi 0.3.0"`.

```
20 00 12 00 60 04 01 00
68 65 72 61 2d 72 70 69 20 30 2e 33 2e 30
```

**`EVENT NET_MODE`**: `AP`, SSID `"HERA-4C21"`.

```
20 00 0d 00 70 01 00 00 48 45 52 41 2d 34 43 32 31
```

**`EVENT AUDIO_OPEN`**: SCO CVSD at 8000 Hz, `a1 = 1`, `a2 = 80`.

```
20 00 04 00 10 01 50 00
```

**`EVENT AUDIO_FMT`**: `FMT_PCM16_8K`, 20 ms, epoch 1:
`a1 = 0x01`, `a2 = 20 | (1 << 8) = 0x0114`.

```
20 00 04 00 12 01 14 01
```

**`EVENT CLIP "+390512345678"`**

```
20 00 11 00 21 00 00 00
2b 33 39 30 35 31 32 33 34 35 36 37 38
```

**`CMD ANSWER`**

```
21 00 04 00 01 00 00 00
```

**`CMD DIAL "0512345"`**

```
21 00 0b 00 03 00 00 00 30 35 31 32 33 34 35
```

**`CMD SET_FORMAT`**: `FMT_G711A`.

```
21 00 04 00 60 03 00 00
```

**`AUDIO_RX`**: `FMT_PCM16_8K`, `count = 1`, epoch 1, `ts = 0`. Payload 328
bytes (8 of header, 320 of samples), frame 332 bytes. Samples omitted.

```
10 00 48 01 01 14 10 01 00 00 00 00 <320 bytes of PCM16 LE>
```

Header `10 00 48 01` (`LEN = 0x0148 = 328`); `fmt = 01`; `frame_ms = 14`
(20); `flags = 10` (filler 0, epoch 1 in the high bits); `count = 01`;
`ts = 00 00 00 00`.

**Filler `AUDIO_TX`**: `FMT_G711A`, `count = 1`, epoch 1, filler (`flags`
bit 0), `ts = 48000`. Payload 168 bytes, frame 172 bytes. `0xD5` is A-law
silence.

```
11 00 a8 00 03 14 11 01 80 bb 00 00 <d5 repeated 160 times>
```

`flags = 0x11` = filler and epoch 1. `ts = 48000 = 0x0000BB80`.

**Datagram content** carrying the `AUDIO_TX` above, `session_id =
0x11223344`, before encryption (§4.2): 4 bytes of prefix followed by the
172-byte frame, 176 bytes in total. On the wire this content is sealed as
described in §11.2.

```
44 33 22 11 11 00 a8 00 03 14 11 01 80 bb 00 00 <d5 ×160>
```

**`CSTATS`**: uptime 12345 ms, 600 received, 2 lost, 601 sent, 40 ms
buffered, RTT 7 ms, playing with microphone active (`flags = 0x03`). Payload
20 bytes.

```
32 00 14 00
39 30 00 00 58 02 00 00 02 00 00 00 59 02 00 00
28 00 07 03
```

**`STATS`**: uptime 12345 ms, 600 sent, 601 received, 0 dropped, 3 lost,
0 late, 2 filled, 0 malformed, 30 ms buffered, 4 ms jitter, SCO CVSD,
`flags = 0x0F`, CPU 12%, RSSI −47 dBm, mode `AP`, `FMT_PCM16_8K`,
`ul_target_ms = 0`. Payload 44 bytes.

```
31 00 2c 00
39 30 00 00 58 02 00 00 59 02 00 00 00 00 00 00
03 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
1e 00 04 00 01 0f 0c d1 01 01 00 00
```

**`BYE`**: reason 1, text `"stop"`.

```
05 00 05 00 01 73 74 6f 70
```

**`NET_STATE`** (GATT value, not a frame): `AP`, `up`, `fail = 0`, address
`192.168.73.1`, control port 7420, audio port 7421, SSID `"HERA-4C21"`.
22 bytes.

```
04 01 02 00 c0 a8 49 01 fc 1c fd 1c 09
48 45 52 41 2d 34 43 32 31
```

**`NET_CFG`** (GATT write): `JOIN_STA`, SSID `"CasaWiFi"`, passphrase
`"segreto123"`. 22 bytes.

```
04 02 08 0a
43 61 73 61 57 69 46 69
73 65 67 72 65 74 6f 31 32 33
```

**`SESSION`** (GATT value): token `00 01 … 0f`, passphrase
`"abcd1234efgh5678"`. 34 bytes.

```
04
00 01 02 03 04 05 06 07 08 09 0a 0b 0c 0d 0e 0f
10 61 62 63 64 31 32 33 34 65 66 67 68 35 36 37 38
```

### 14.1 Encrypted Profile Vectors

The X25519 key pairs are those of [RFC7748] §6.1, so that the values can be
checked against an external source. Implementers should first check that
their X25519 reproduces them, then that the derivation reproduces the keys
below.

Implementations that build HKDF on HMAC (libsodium provides HKDF only from
version 1.0.19, and Raspberry Pi OS ships 1.0.18) should first verify it
against [RFC5869] Appendix A. The vectors below show that two
implementations agree, not that they are correct. Test case 1 of the
appendix requests 42 bytes and exercises the multi-block path, which 32-byte
keys never reach.

```
client private  77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a
client public   8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a
device private  5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb
device public   de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f
shared          4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742
```

Derivation with `salt = pub_client ‖ pub_device` (§11.3):

```
prk             c82703b88c64641ec4f2ae4ac70a04407c5a0d76a6acd7168e5dd3a349b6e13c
K_ctrl_c2d      9903b35c0c66fd2d1cef08bb9486cc279fa997cafac6fec911b1fb45ba2c8c2e
K_ctrl_d2c      6870f486648a71b38c0a043f01cc5d3b97b6349b037a27038919883850be0e6c
K_audio_c2d     6f548c424a8b48eea2689677fc6ce25d38fba71d9a20c13e5a5a81d788f21425
K_audio_d2c     31e46fd34a28bd7e71f441e023e10a5d6e9e25c1a7caec91f222ffe9a42eb3a1
```

**Control record** carrying the `CMD ANSWER` frame above (8 bytes),
`counter = 0`, key `K_ctrl_c2d`. `rec_len = 8 + 8 + 16 = 32 = 0x0020`.

```
20 00                                              rec_len
00 00 00 00 00 00 00 00                            counter
3d 7a 7d 7f 4a 5f 9c 2a                            ciphertext (8 bytes)
e6 26 62 cd a7 1e d5 97 b1 81 32 44 b2 23 00 fc    tag
```

34 bytes in total for an 8-byte frame. The AAD is the first ten bytes
(`20 00` and the counter); the nonce is `00 00 00 00` followed by the
counter.

---

## 15. Conformance Checklist

**Device**

1. BLE advertising always active, in every mode (§2.5).
2. `NET_STATE` with notifications, `NET_CFG` and `SESSION` over GATT, all
   requiring encryption (§3.1).
3. In `AP` mode, DHCP without option 3 and without option 6 (§2.4).
4. Fallback from `STA` to `AP` within `HERA_STA_TIMEOUT_S`, with `fail` set
   (§2.5).
5. Recovery after three failed starts (§2.5).
6. `TCP_NODELAY` on the control channel (§4.1).
7. `HELLO` sent without waiting for the client's (§5.3).
8. `HELLO` tag verified in constant time; on failure `ERROR` 5 and close
   (§11.4). The token never appears on the wire.
9. New ephemeral X25519 key pair per connection, including resumes; keys
   derived with HKDF using `pub_client ‖ pub_device` as salt (§11.3). No
   unencrypted fallback.
10. Control records and audio datagrams encrypted from the first frame after
    `HELLO`; independent counters per channel and direction; 64-entry
    anti-replay window on audio (§11.2).
11. `READY` with the selected format, then the state snapshot (§5.4, §8.1).
12. Audio flow bound to the first valid datagram; no `AUDIO_RX` before;
    rebinding when the source address changes (§4.2.1).
13. Control reassembly (§4.1.1); malformed frames fatal on control,
    silently discarded on audio (§5.1).
14. Format selection with the precedence of §6.2, preferring no conversion;
    `AUDIO_FMT` after every `AUDIO_OPEN`.
15. Block duration: the shortest supported by both, unless forced; 20 ms
    mandatory; 20 ms only on `USB` (§6.2).
16. Epoch incremented at every `AUDIO_OPEN`, in `flags` and in `AUDIO_FMT`
    (§6.3).
17. No audio queueing: blocks not accepted are dropped and counted (§6.6).
18. Uplink jitter buffer per §6.5.
19. Resume window with the call kept up; uplink restored on resume (§5.4).
20. Takeover on a valid resume, decided after `HELLO`; `BYE` reason 2
    otherwise (§5.7).
21. `STATS` every second with actual `cpu_pct`, `ul_lost`, `ul_jitter_ms`
    and `rssi` (§10.1).
22. SSIDs and passphrases validated and never passed to a shell (§11.6).

**Client**

1. Address and ports read from `NET_STATE`, never assumed (§4.5).
2. Sockets bound to the device network, so that the cellular network stays
   the default route (§2.4, §12.3).
3. Control reassembly: never assume one frame per read (§4.1.1).
4. `HELLO` sent immediately, with `formats`, `durations`, the ephemeral
   public key and the tag computed with the token (§5.3). The token is not
   transmitted.
5. Records encrypted from the next frame on; own counters per channel and
   direction; no unencrypted fallback (§11.2).
6. State snapshot not treated as transitions (§5.4).
7. `PING` on the audio channel once the session is up, and every 500 ms
   while not sending audio (§4.2.1).
8. Format and duration read from the block header, not from the event;
   playback rate taken from `fmt`, not from `AUDIO_OPEN.a2` (§6.3).
9. New epoch or `ts` restart: reset buffers and counters (§6.3).
10. One `AUDIO_TX` per `AUDIO_RX`, plus one per lost block; silence with
    `flags` bit 0 when there is no audio (§6.4).
11. Free-running fallback after 60 ms, and resynchronisation on the first
    block received (§6.4.1).
12. Playback buffer dimensioned on measured jitter, not on a number of
    blocks (§6.5, §7.1).
13. Resume with the previous `session_id` before starting a new session
    (§5.4).
14. `CSTATS` every second (§10.2).
15. Call state taken from `EVENT`s, not from `ACK`s (§9.1).
16. No resume after `BYE` (§5.6).

---

## References

### Normative References

- [RFC2104] Krawczyk, H., Bellare, M., and R. Canetti, "HMAC: Keyed-Hashing
  for Message Authentication", RFC 2104, February 1997.
- [RFC2119] Bradner, S., "Key words for use in RFCs to Indicate Requirement
  Levels", BCP 14, RFC 2119, March 1997.
- [RFC5869] Krawczyk, H. and P. Eronen, "HMAC-based Extract-and-Expand Key
  Derivation Function (HKDF)", RFC 5869, May 2010.
- [RFC7748] Langley, A., Hamburg, M., and S. Turner, "Elliptic Curves for
  Security", RFC 7748, January 2016.
- [RFC8174] Leiba, B., "Ambiguity of Uppercase vs Lowercase in RFC 2119 Key
  Words", BCP 14, RFC 8174, May 2017.
- [RFC8439] Nir, Y. and A. Langley, "ChaCha20 and Poly1305 for IETF
  Protocols", RFC 8439, June 2018.
- [G.711] ITU-T, "Pulse code modulation (PCM) of voice frequencies",
  Recommendation G.711, November 1988.
- [HFP] Bluetooth SIG, "Hands-Free Profile", version 1.7 or later.

### Informative References

- [RFC3550] Schulzrinne, H., Casner, S., Frederick, R., and V. Jacobson,
  "RTP: A Transport Protocol for Real-Time Applications", STD 64, RFC 3550,
  July 2003.
- [RFC4303] Kent, S., "IP Encapsulating Security Payload (ESP)", RFC 4303,
  December 2005.
