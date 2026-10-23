import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:hera_client/hera_client.dart';

Uint8List _hex(String hex) {
  final clean = hex.replaceAll(RegExp(r'\s'), '');
  return Uint8List.fromList([
    for (var i = 0; i < clean.length; i += 2)
      int.parse(clean.substring(i, i + 2), radix: 16),
  ]);
}

String _toHex(Uint8List bytes) =>
    bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

/// Key pairs from RFC 7748 §6.1, as used in `PROTOCOL.md` §14.1.
final _clientPriv =
    _hex('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a');
final _clientPub =
    _hex('8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
final _devicePriv =
    _hex('5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb');
final _devicePub =
    _hex('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f');

void main() {
  group('X25519 against RFC 7748', () {
    test('public key derived from the private key', () async {
      final client = await HeraKeyPair.fromPrivateKey(_clientPriv);
      final device = await HeraKeyPair.fromPrivateKey(_devicePriv);
      expect(_toHex(client.publicKey), _toHex(_clientPub));
      expect(_toHex(device.publicKey), _toHex(_devicePub));
    });
  });

  group('key derivation (§14.1)', () {
    late HeraSecSession client;
    late HeraSecSession device;

    setUp(() async {
      final c = await HeraKeyPair.fromPrivateKey(_clientPriv);
      final d = await HeraKeyPair.fromPrivateKey(_devicePriv);
      client = await HeraSecSession.derive(
        own: c,
        peerPublicKey: _devicePub,
      );
      device = await HeraSecSession.derive(
        own: d,
        peerPublicKey: _clientPub,
        weAreDevice: true,
      );
    });

    test('the four keys match the §14.1 vectors', () {
      // The device test suite checks the same values.
      expect(_toHex(client.controlOut.debugKey),
          '9903b35c0c66fd2d1cef08bb9486cc279fa997cafac6fec911b1fb45ba2c8c2e');
      expect(_toHex(client.controlIn.debugKey),
          '6870f486648a71b38c0a043f01cc5d3b97b6349b037a27038919883850be0e6c');
      expect(_toHex(client.audioOut.debugKey),
          '6f548c424a8b48eea2689677fc6ce25d38fba71d9a20c13e5a5a81d788f21425');
      expect(_toHex(client.audioIn.debugKey),
          '31e46fd34a28bd7e71f441e023e10a5d6e9e25c1a7caec91f222ffe9a42eb3a1');
    });

    test('client and device keys pair up', () {
      expect(_toHex(client.controlOut.debugKey),
          _toHex(device.controlIn.debugKey));
      expect(_toHex(client.controlIn.debugKey),
          _toHex(device.controlOut.debugKey));
      expect(_toHex(client.audioOut.debugKey), _toHex(device.audioIn.debugKey));
      expect(_toHex(client.audioIn.debugKey), _toHex(device.audioOut.debugKey));
    });

    test('control and audio keys differ', () {
      expect(_toHex(client.controlOut.debugKey),
          isNot(_toHex(client.audioOut.debugKey)));
    });

    test('low-order public key is rejected', () async {
      // It yields an all-zero shared secret.
      final c = await HeraKeyPair.fromPrivateKey(_clientPriv);
      expect(
        () => HeraSecSession.derive(own: c, peerPublicKey: Uint8List(32)),
        throwsA(isA<HeraProtocolException>()),
      );
    });
  });

  group('control records (§14.1)', () {
    late HeraSecSession client;
    late HeraSecSession device;

    setUp(() async {
      final c = await HeraKeyPair.fromPrivateKey(_clientPriv);
      final d = await HeraKeyPair.fromPrivateKey(_devicePriv);
      client = await HeraSecSession.derive(own: c, peerPublicKey: _devicePub);
      device = await HeraSecSession.derive(
          own: d, peerPublicKey: _clientPub, weAreDevice: true);
    });

    test('matches the §14.1 record vector', () {
      final frame = buildHeraFrame(
        type: HeraFrameType.command,
        payload: buildHeraMessagePayload(id: HeraCommandId.answer),
      );
      final record = sealHeraRecord(client.controlOut, frame);
      expect(
        _toHex(record),
        '2000'
        '0000000000000000'
        '3d7a7d7f4a5f9c2a'
        'e62662cda71ed597b1813244b22300fc',
      );
      expect(record.length, 34);
    });

    test('the device opens it', () {
      final frame = buildHeraFrame(
        type: HeraFrameType.command,
        payload: buildHeraMessagePayload(id: HeraCommandId.answer),
      );
      final record = sealHeraRecord(client.controlOut, frame);
      expect(openHeraRecord(device.controlIn, record), frame);
    });

    test('sealing the same plaintext twice gives different bytes', () {
      // The counter, and therefore the nonce, advances.
      final frame = buildHeraFrame(type: HeraFrameType.ping);
      final a = sealHeraRecord(client.controlOut, frame);
      final b = sealHeraRecord(client.controlOut, frame);
      expect(a.length, b.length);
      expect(_toHex(a), isNot(_toHex(b)));
    });

    test('tampered record is rejected', () {
      final frame = buildHeraFrame(type: HeraFrameType.ping);
      final record = sealHeraRecord(client.controlOut, frame);
      record[record.length - 3] ^= 0x01;
      expect(() => openHeraRecord(device.controlIn, record),
          throwsA(isA<HeraProtocolException>()));
    });

    test('replayed record is rejected', () {
      // Control counters must strictly increase (§11.2).
      final frame = buildHeraFrame(type: HeraFrameType.ping);
      final record = sealHeraRecord(client.controlOut, frame);
      expect(openHeraRecord(device.controlIn, record), isNotEmpty);
      expect(() => openHeraRecord(device.controlIn, record),
          throwsA(isA<HeraProtocolException>()));
    });

    test('heraRecordLength rejects out-of-range lengths', () {
      final tiny = Uint8List.fromList([0x01, 0x00]);
      expect(heraRecordLength(tiny, 0), 0);
      final huge = Uint8List.fromList([0xFF, 0xFF]);
      expect(heraRecordLength(huge, 0), 0);
    });
  });

  group('audio datagrams (§11.2)', () {
    const sessionId = 0x11223344;
    late HeraSecSession client;
    late HeraSecSession device;

    setUp(() async {
      final c = await HeraKeyPair.fromPrivateKey(_clientPriv);
      final d = await HeraKeyPair.fromPrivateKey(_devicePriv);
      client = await HeraSecSession.derive(own: c, peerPublicKey: _devicePub);
      device = await HeraSecSession.derive(
          own: d, peerPublicKey: _clientPub, weAreDevice: true);
    });

    Uint8List block() => buildHeraFrame(
          type: HeraFrameType.audioTx,
          payload: buildHeraAudioPayload(
            format: HeraFormat.g711a,
            frameMs: 20,
            samples: Uint8List(160)..fillRange(0, 160, 0xD5),
            timestamp: 0,
          ),
        );

    test('28-byte overhead', () {
      final frame = block();
      final dg = sealHeraDatagram(client.audioOut, sessionId, frame);
      expect(dg.length, frame.length + HeraSec.datagramOverhead);
    });

    test('opens', () {
      final frame = block();
      final dg = sealHeraDatagram(client.audioOut, sessionId, frame);
      expect(openHeraDatagram(device.audioIn, sessionId, dg), frame);
    });

    test('wrong session id is rejected', () {
      final dg = sealHeraDatagram(client.audioOut, sessionId, block());
      expect(openHeraDatagram(device.audioIn, 0x99999999, dg), isNull);
    });

    test('reordering within the window is accepted', () {
      final a = sealHeraDatagram(client.audioOut, sessionId, block());
      final b = sealHeraDatagram(client.audioOut, sessionId, block());
      final c = sealHeraDatagram(client.audioOut, sessionId, block());
      expect(openHeraDatagram(device.audioIn, sessionId, c), isNotNull);
      expect(openHeraDatagram(device.audioIn, sessionId, a), isNotNull);
      expect(openHeraDatagram(device.audioIn, sessionId, b), isNotNull);
      // Duplicates are not.
      expect(openHeraDatagram(device.audioIn, sessionId, a), isNull);
    });

    test('counters left of the window are rejected', () {
      final old = sealHeraDatagram(client.audioOut, sessionId, block());
      for (var i = 0; i < 70; i++) {
        sealHeraDatagram(client.audioOut, sessionId, block());
      }
      final far = sealHeraDatagram(client.audioOut, sessionId, block());
      expect(openHeraDatagram(device.audioIn, sessionId, far), isNotNull);
      expect(openHeraDatagram(device.audioIn, sessionId, old), isNull);
    });

    test('truncated datagram returns null', () {
      final dg = sealHeraDatagram(client.audioOut, sessionId, block());
      final cut = Uint8List.sublistView(dg, 0, dg.length - 4);
      expect(openHeraDatagram(device.audioIn, sessionId, cut), isNull);
    });
  });

  group('HELLO tag (§11.4)', () {
    final token = Uint8List.fromList(List.generate(16, (i) => i));

    Future<Uint8List> tag({
      int role = 0,
      int formats = 0x0F,
      int durations = 0x07,
      int flags = 0,
      int sessionId = 0,
      Uint8List? pub,
    }) =>
        heraHelloTag(
          token: token,
          role: role,
          formats: formats,
          durations: durations,
          flags: flags,
          sessionId: sessionId,
          publicKey: pub ?? _clientPub,
        );

    test('same HELLO gives the same tag', () async {
      expect(_toHex(await tag()), _toHex(await tag()));
    });

    test('every authenticated field changes the tag', () async {
      final base = _toHex(await tag());
      expect(_toHex(await tag(pub: _devicePub)), isNot(base));
      expect(_toHex(await tag(role: 1)), isNot(base));
      expect(_toHex(await tag(formats: 0x05)), isNot(base));
      expect(_toHex(await tag(durations: 0x01)), isNot(base));
      expect(_toHex(await tag(flags: 1)), isNot(base));
      expect(_toHex(await tag(sessionId: 42)), isNot(base));
    });

    test('different token gives a different tag', () async {
      final other = await heraHelloTag(
        token: Uint8List.fromList(List.generate(16, (i) => i + 1)),
        role: 0,
        formats: 0x0F,
        durations: 0x07,
        flags: 0,
        sessionId: 0,
        publicKey: _clientPub,
      );
      expect(_toHex(other), isNot(_toHex(await tag())));
    });

    test('heraTagEqual', () {
      final a = Uint8List.fromList(List.generate(16, (i) => i));
      final b = Uint8List.fromList(List.generate(16, (i) => i));
      expect(heraTagEqual(a, b), isTrue);
      b[15] ^= 1;
      expect(heraTagEqual(a, b), isFalse);
      expect(heraTagEqual(a, Uint8List(8)), isFalse);
    });
  });
}
