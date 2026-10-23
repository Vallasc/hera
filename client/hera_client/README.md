# hera_client

Flutter client for the HERA protocol, version 4 ([`PROTOCOL.md`](../../PROTOCOL.md)).

A HERA device is a Bluetooth hands-free unit for a phone that relays call
audio and call control over IP. This package implements the client side:
BLE discovery and provisioning, joining the device's network, the encrypted
control (TCP) and audio (UDP) channels, session resume and automatic
reconnection. An optional USB serial transport is also included.

The package handles call events and audio blocks only. Speech recognition,
synthesis and any conversation logic belong to the application.

## Usage

```dart
final connection = HeraConnection(
  transport: HeraTransport.accessPoint,
  provisioner: () async => endpoint,      // from BLE, or from stored values
  uplink: (blocks, format) => HeraUplink(samples),
  onStatus: (status) => print(status.summary),
);

connection.sessions.listen((session) {
  session.events.listen((event) { /* RING, CLIP, CALL, ... */ });
  session.downlink.listen((block) { /* samples, in the block's format */ });
});

await connection.start();
```

`provisioner` returns a `HeraEndpoint` (address, ports and token). It is
called before every connection attempt; `HeraDiscovery` provides the BLE
operations needed to build one (`scan`, `connect`, `readNetState`,
`readCredentials`).

`uplink` is called by the session once for every received downlink block
(PROTOCOL.md §6.4); it is not called when the application has audio
available. It must always return exactly the requested number of blocks,
padding with silence and setting `filler: true` when there is no speech.
Returning fewer blocks makes the device's jitter buffer underrun.

## Structure

| file | contents |
|---|---|
| `protocol.dart` | wire format: frame header, `HELLO`, `EVENT`, `CMD`, `ACK`, `BYE`, `STATS`, `CSTATS`, audio payload, format and duration tables, stream parser. No I/O |
| `sec.dart` | encrypted profile (§11): X25519, HKDF-SHA256, ChaCha20-Poly1305, anti-replay window |
| `jitter.dart` | adaptive playback buffer policy (§6.5) |
| `channel.dart` | `HeraLink` interface and the transport-independent part: dispatch and counters |
| `wifi_link.dart` | TCP control channel (with `TCP_NODELAY`) and UDP audio channel |
| `serial_link.dart` | single-stream transport with the two queues of §4.4 |
| `serial_port*.dart` | serial port access: USB host on Android, libserialport on desktop |
| `discovery.dart` | BLE scan and the `NET_STATE`, `NET_CFG` and `SESSION` characteristics |
| `session.dart` | handshake, resume, keepalive, uplink pacing and free-running fallback |
| `connection.dart` | supervisor: keeps the link open, resumes or restarts sessions |
| `status.dart` | display state, serialisable across isolates |

## Notes

- **Format and duration come from the block header**, not from events.
  `AUDIO_FMT` and the audio datagrams travel on different channels and can
  arrive in either order (§6.3). `AUDIO_OPEN` reports the SCO rate divided
  by 100; using it as the playback rate produces the "slow and low-pitched"
  fault of §13.1.
- **Uplink pacing follows the downlink**, not a local timer (§6.4). If the
  downlink stops for 60 ms the session switches to a local timer and
  resynchronises on the next received block (§6.4.1).
- **A dropped connection does not end the call.** The device keeps the
  session for its resume window (10 s by default); `HeraConnection` attempts
  a resume before starting a new session (§5.4).
- **All IP traffic is encrypted** with per-connection X25519 keys and
  ChaCha20-Poly1305, in both directions, with no unencrypted fallback
  (§11). The token from `SESSION` is never transmitted; it keys the `HELLO`
  tag (§11.4). The serial transport is not encrypted (§11.1).
- **The block duration is negotiated** (§6.2). The client offers the
  durations in `HeraConnection.durations` and the device selects the
  shortest common one. After that, `session.frameMs` is authoritative; using
  a constant block size produces blocks the device drops.
- **The playback buffer adapts.** `session.playback` is the policy of §6.5:
  the session feeds it arrival times, and the player reads the target depth
  from it. It shrinks only on filler blocks.

## Dependencies

- `hera_audio` (in this repository) for `AudioFormat` and `AudioEncoding`.
- `flutter_blue_plus` is pinned to 1.x; version 2.0 and later use a
  different licence.
- `cryptography` provides the primitives of §11 in pure Dart.
- `usb_serial` and `flutter_libserialport` provide the serial transport.

## Tests

```sh
flutter test
```

The tests include the byte vectors of PROTOCOL.md §14 and §14.1. The device
test suite (`make test`) checks the same vectors, so the two implementations
are checked against the same bytes, including the HKDF output of the
`cryptography` package against the device's implementation.
