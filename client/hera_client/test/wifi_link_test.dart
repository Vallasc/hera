import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:hera_client/src/protocol.dart';
import 'package:hera_client/src/sec.dart';
import 'package:hera_client/src/wifi_link.dart';

/// Key pairs from RFC 7748 §6.1, as in `sec_test.dart`.
const _clientPriv =
    '77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a';
const _devicePub =
    'de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f';
const _devicePriv =
    '5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb';
const _clientPub =
    '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a';

Uint8List _fromHex(String hex) {
  final clean = hex.replaceAll(' ', '');
  return Uint8List.fromList([
    for (var i = 0; i < clean.length; i += 2)
      int.parse(clean.substring(i, i + 2), radix: 16),
  ]);
}

/// Fake device: accepts one TCP connection and records UDP datagrams.
class _FakeDevice {
  _FakeDevice._(this._server, this._audio);

  final ServerSocket _server;
  final RawDatagramSocket _audio;

  final BytesBuilder control = BytesBuilder(copy: false);
  final List<Uint8List> datagrams = [];

  static Future<_FakeDevice> start() async {
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final audio = await RawDatagramSocket.bind(InternetAddress.loopbackIPv4, 0);
    final device = _FakeDevice._(server, audio);
    server.listen((socket) {
      device.accepted = socket;
      socket.listen(device.control.add);
    });
    audio.listen((event) {
      if (event != RawSocketEvent.read) return;
      for (var p = audio.receive(); p != null; p = audio.receive()) {
        device.datagrams.add(p.data);
      }
    });
    return device;
  }

  /// The accepted socket, for writing to the client.
  Socket? accepted;

  int get controlPort => _server.port;
  int get audioPort => _audio.port;

  void send(Uint8List bytes) => accepted!.add(bytes);

  Future<void> close() async {
    await _server.close();
    _audio.close();
  }
}

/// `EVENT` payload (§8) without the optional string.
Uint8List _event({required int id, int a1 = 0, int a2 = 0}) =>
    Uint8List.fromList([id, a1, a2 & 0xFF, (a2 >> 8) & 0xFF]);

/// Session keys as derived by the client.
Future<HeraSecSession> _clientKeys() async => HeraSecSession.derive(
      own: await HeraKeyPair.fromPrivateKey(_fromHex(_clientPriv)),
      peerPublicKey: _fromHex(_devicePub),
    );

/// Session keys as derived by the device.
Future<HeraSecSession> _deviceKeys() async => HeraSecSession.derive(
      own: await HeraKeyPair.fromPrivateKey(_fromHex(_devicePriv)),
      peerPublicKey: _fromHex(_clientPub),
      weAreDevice: true,
    );

/// Runs the event loop until [test] returns true or a timeout expires.
Future<void> _until(bool Function() test) async {
  for (var i = 0; i < 100 && !test(); i++) {
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
}

void main() {
  late _FakeDevice device;
  late WifiHeraLink link;

  setUp(() async {
    device = await _FakeDevice.start();
    link = await WifiHeraLink.connect(
      address: InternetAddress.loopbackIPv4,
      controlPort: device.controlPort,
      audioPort: device.audioPort,
    );
  });

  tearDown(() async {
    await link.close();
    await device.close();
  });

  group('control channel', () {
    test('without keys, frames are sent in the clear', () async {
      // Only HELLO is sent before the keys exist (§11.2).
      link.sendControl(HeraFrameType.bye);
      await _until(() => device.control.length >= Hera.headerSize);

      final sent = device.control.toBytes();
      expect(sent[0], HeraFrameType.bye, reason: 'TYPE comes first');
      expect(sent.length, Hera.headerSize);
    });

    test('with keys, every frame is sent inside a record', () async {
      link.sec = await _clientKeys();
      link.sendControl(HeraFrameType.bye);
      await _until(() => device.control.length >= 2);

      final sent = device.control.toBytes();
      final recordLength = heraRecordLength(sent, 0);
      expect(recordLength, sent.length, reason: 'one complete record');
      // rec_len (2) + counter (8) + frame (4) + tag (16).
      expect(sent.length, 2 + 8 + Hera.headerSize + 16);
      expect(sent[0], isNot(HeraFrameType.bye),
          reason: 'TYPE is no longer in the clear');
    });

    test('the device opens the record', () async {
      final ours = await _clientKeys();
      final theirs = await _deviceKeys();

      link.sec = ours;
      link.sendControl(HeraFrameType.bye);
      await _until(() => device.control.length >= 2);

      final opened = openHeraRecord(theirs.controlIn, device.control.toBytes());
      expect(opened[0], HeraFrameType.bye);
      expect(opened.length, Hera.headerSize);
    });
  });

  group('handshake', () {
    test('HELLO and READY in one TCP segment', () async {
      // Key derivation is asynchronous, so the READY record that follows the
      // device HELLO in the same segment must be held until the keys exist,
      // not parsed as a plaintext frame.
      final device = await _FakeDevice.start();
      final link = await WifiHeraLink.connect(
        address: InternetAddress.loopbackIPv4,
        controlPort: device.controlPort,
        audioPort: device.audioPort,
      );

      final events = <HeraMessage>[];
      link.control.listen(events.add);
      var closed = false;
      unawaited(link.closed.then((_) => closed = true));

      final theirs = await _deviceKeys();

      final hello = buildHeraFrame(
        type: HeraFrameType.hello,
        payload: buildHeraHelloPayload(
          formats: HeraFormat.allMask,
          durations: Hera.allDurations,
          publicKey: _fromHex(_devicePub),
          tag: Uint8List(16),
          sessionId: 0x11223344,
          role: 1,
          software: 'hera-rpi',
        ),
      );
      final ready = sealHeraRecord(
        theirs.controlOut,
        buildHeraFrame(
          type: HeraFrameType.event,
          payload: _event(
            id: HeraEventId.ready,
            a1: Hera.protocolVersion,
            a2: HeraFormat.pcm16At8k.id,
          ),
        ),
      );

      // Wait for the server to accept the connection.
      await _until(() => device.accepted != null);

      // A single write.
      device.send(Uint8List.fromList([...hello, ...ready]));

      // Set the keys as HeraSession would, which processes the held bytes.
      await _until(() => events.isNotEmpty);
      link.sec = await _clientKeys();
      await _until(() => events.length >= 2);

      expect(closed, isFalse, reason: 'the link must stay open');
      expect(events.first, isA<HeraHello>());
      expect(events.length, greaterThanOrEqualTo(2),
          reason: 'READY must have been read');
      expect(events[1], isA<HeraEvent>());
      expect((events[1] as HeraEvent).id, HeraEventId.ready);

      await link.close();
      await device.close();
    });
  });

  group('audio channel', () {
    test('nothing is sent without keys', () async {
      link.sessionId = 0x11223344;
      link.sendAudio(HeraFrameType.ping);
      await Future<void>.delayed(const Duration(milliseconds: 50));
      expect(device.datagrams, isEmpty);
      expect(link.droppedAudio, greaterThan(0), reason: 'counted as dropped');
    });

    test('datagram is sealed, session_id in the clear', () async {
      link.sec = await _clientKeys();
      link.sessionId = 0x11223344;
      link.sendAudio(HeraFrameType.ping);
      await _until(() => device.datagrams.isNotEmpty);

      final datagram = device.datagrams.single;
      // session_id is sent in the clear and authenticated as AAD (§11.2).
      expect(Uint8List.sublistView(datagram, 0, 4), _fromHex('44332211'));
      expect(datagram.length, 4 + 8 + Hera.headerSize + 16);
      expect(datagram[12], isNot(HeraFrameType.ping),
          reason: 'the frame is encrypted');
    });
  });
}
