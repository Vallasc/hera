# HERA

HERA turns a Raspberry Pi into a Bluetooth hands-free unit for a phone and
relays the phone's calls, audio and call control, to a client application
over an IP link. A typical client is an application running on the same
phone, for example a voice assistant that answers calls.

```
 phone ──HFP/SCO──► device (Raspberry Pi) ══ TCP control + UDP audio ══► client
                          └──── BLE: discovery and provisioning ────────►
```

- Audio is PCM16 or G.711; no codec runs on the device.
- All IP traffic is encrypted and authenticated (X25519, HKDF-SHA256,
  ChaCha20-Poly1305).
- The device runs its own WiFi access point by default; the phone keeps
  cellular data as its default route.
- The audio block duration is negotiated among 20, 10 and 5 ms; with 5 ms
  blocks the transport latency is about 50 ms.

## Repository layout

| path | contents |
|---|---|
| [`PROTOCOL.md`](PROTOCOL.md) | protocol specification, version 4 (normative) |
| [`device/`](device/) | device implementation in C for Raspberry Pi OS (BlueZ, hostapd, dnsmasq, libsodium) |
| [`client/hera_client/`](client/hera_client/) | Flutter client library |
| [`client/hera_audio/`](client/hera_audio/) | pure Dart audio primitives used by the client (PCM16, G.711, resampling) |

## Getting started

Device, on Raspberry Pi OS:

```sh
cd device
bash setup.sh            # dependencies, build, BlueZ configuration
sudo -E ./hera-device
```

See [`device/README.md`](device/README.md) for configuration, running as a
service and bring-up with `tools/hera_probe`.

Client, in a Flutter application's `pubspec.yaml`:

```yaml
dependencies:
  hera_client:
    git:
      url: https://github.com/<owner>/<repository>.git
      path: client/hera_client
```

See [`client/hera_client/README.md`](client/hera_client/README.md) for usage.

## Tests

```sh
(cd device && make test)                 # any host with libsodium
(cd device && make test-link)            # Linux only
(cd client/hera_audio && dart test)
(cd client/hera_client && flutter test)
```

Both implementations are tested against the byte vectors of PROTOCOL.md §14
and §14.1.
