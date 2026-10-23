import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:hera_client/hera_client.dart';

/// Parses hex as written in `PROTOCOL.md` §14: space-separated pairs or a
/// contiguous string (§14.1).
Uint8List _bytes(String hex) {
  final clean = hex.replaceAll(RegExp(r'\s+'), '');
  if (clean.length.isOdd) {
    throw ArgumentError('odd number of hex digits: ${clean.length}');
  }
  return Uint8List.fromList([
    for (var i = 0; i < clean.length; i += 2)
      int.parse(clean.substring(i, i + 2), radix: 16),
  ]);
}

String _toHex(Uint8List bytes) =>
    bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

void main() {
  group('§14 vectors, encoding', () {
    test('client HELLO layout', () {
      // v4: 64-byte fixed part, public key at offset 16, tag at 48; the token
      // is not included (§11.4).
      final pub = Uint8List.fromList(List.generate(32, (i) => 0x20 + i));
      final tag = Uint8List.fromList(List.generate(16, (i) => 0xB0 + i));
      final frame = buildHeraFrame(
        type: HeraFrameType.hello,
        payload: buildHeraHelloPayload(
          formats: HeraFormat.allMask,
          durations: Hera.allDurations,
          publicKey: pub,
          tag: tag,
          software: 'hera_client',
        ),
      );

      expect(frame.length, Hera.headerSize + Hera.helloSize + 11);
      expect(frame[0], HeraFrameType.hello);
      final p = Uint8List.sublistView(frame, Hera.headerSize);
      expect(Uint8List.sublistView(p, 0, 4), _bytes('48 45 52 41'));
      expect(p[4], 4, reason: 'version 4');
      expect(p[5], 0, reason: 'client role');
      expect(p[6], HeraFormat.allMask);
      expect(p[7], Hera.allDurations);
      expect(p.sublist(9, 12), [0, 0, 0], reason: 'reserved bytes');
      expect(Uint8List.sublistView(p, 16, 48), pub);
      expect(Uint8List.sublistView(p, 48, 64), tag);
      expect(utf8.decode(p.sublist(64)), 'hera_client');

      // The token must not appear anywhere in the frame.
      final token = Uint8List.fromList(List.generate(16, (i) => i));
      var found = false;
      for (var i = 0; i + 16 <= frame.length; i++) {
        if (_toHex(Uint8List.sublistView(frame, i, i + 16)) == _toHex(token)) {
          found = true;
        }
      }
      expect(found, isFalse, reason: 'token is never transmitted');
    });

    test('client HELLO matches the §14 bytes', () async {
      // Public key from §14.1, so that the tag is reproducible. The device
      // test suite checks the same bytes.
      final pub = _bytes(
          '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a');
      final token = Uint8List.fromList(List.generate(16, (i) => i));

      final tag = await heraHelloTag(
        token: token,
        role: 0,
        formats: HeraFormat.allMask,
        durations: Hera.allDurations,
        flags: 0,
        sessionId: 0,
        publicKey: pub,
      );
      expect(_toHex(tag), 'dad8a1519105b1d27f84023495ed1f31');

      final frame = buildHeraFrame(
        type: HeraFrameType.hello,
        payload: buildHeraHelloPayload(
          formats: HeraFormat.allMask,
          durations: Hera.allDurations,
          publicKey: pub,
          tag: tag,
          software: 'hera_client',
        ),
      );
      expect(frame.length, 79);
      expect(_toHex(Uint8List.sublistView(frame, 0, 20)),
          '01004b00' '4845524104000f07' '0000000000000000');
      expect(_toHex(Uint8List.sublistView(frame, 52, 68)),
          'dad8a1519105b1d27f84023495ed1f31');
    });

    test('client HELLO, resume', () {
      final frame = buildHeraFrame(
        type: HeraFrameType.hello,
        payload: buildHeraHelloPayload(
          formats: HeraFormat.allMask,
          durations: Hera.allDurations,
          publicKey: Uint8List(32),
          tag: Uint8List(16),
          sessionId: 0x11223344,
          resume: true,
        ),
      );
      final p = Uint8List.sublistView(frame, Hera.headerSize);
      expect(p[8] & 0x01, 1, reason: 'resume flag');
      expect(Uint8List.sublistView(p, 12, 16), _bytes('44 33 22 11'));
    });

    test('CMD ANSWER', () {
      expect(
        buildHeraFrame(
          type: HeraFrameType.command,
          payload: buildHeraMessagePayload(id: HeraCommandId.answer),
        ),
        _bytes('21 00 04 00 01 00 00 00'),
      );
    });

    test('CMD DIAL', () {
      expect(
        buildHeraFrame(
          type: HeraFrameType.command,
          payload: buildHeraMessagePayload(
            id: HeraCommandId.dial,
            text: '0512345',
          ),
        ),
        _bytes('21 00 0b 00 03 00 00 00 30 35 31 32 33 34 35'),
      );
    });

    test('CMD SET_FORMAT', () {
      expect(
        buildHeraFrame(
          type: HeraFrameType.command,
          payload: buildHeraMessagePayload(
            id: HeraCommandId.setFormat,
            a1: HeraFormat.g711a.id,
          ),
        ),
        _bytes('21 00 04 00 60 03 00 00'),
      );
    });

    test('G.711 A-law filler AUDIO_TX', () {
      final frame = buildHeraFrame(
        type: HeraFrameType.audioTx,
        payload: buildHeraAudioPayload(
          format: HeraFormat.g711a,
          frameMs: 20,
          samples: Uint8List(160)..fillRange(0, 160, 0xD5),
          timestamp: 48000,
          epoch: 1,
          filler: true,
        ),
      );
      expect(
        Uint8List.sublistView(frame, 0, 12),
        _bytes('11 00 a8 00 03 14 11 01 80 bb 00 00'),
      );
      expect(frame.length, 172);
      expect(frame.sublist(12).every((b) => b == 0xD5), isTrue);
    });

    test('UDP datagram content', () {
      final frame = buildHeraFrame(
        type: HeraFrameType.audioTx,
        payload: buildHeraAudioPayload(
          format: HeraFormat.g711a,
          frameMs: 20,
          samples: Uint8List(160)..fillRange(0, 160, 0xD5),
          timestamp: 48000,
          epoch: 1,
          filler: true,
        ),
      );
      // Only the frame is checked: on the wire the datagram is sealed
      // (§11.2), and the §14 vector shows its plaintext content.
      expect(
        Uint8List.sublistView(frame, 0, 12),
        _bytes('11 00 a8 00 03 14 11 01 80 bb 00 00'),
      );
      expect(frame.length, 172);
    });

    test('CSTATS', () {
      expect(
        buildHeraFrame(
          type: HeraFrameType.clientStats,
          payload: buildHeraClientStats(
            uptimeMs: 12345,
            downlinkBlocks: 600,
            downlinkLost: 2,
            uplinkBlocks: 601,
            bufferedMs: 40,
            rttMs: 7,
            playing: true,
            microphoneLive: true,
          ),
        ),
        _bytes('''
          32 00 14 00
          39 30 00 00 58 02 00 00 02 00 00 00 59 02 00 00
          28 00 07 03
        '''),
      );
    });

    test('BYE', () {
      expect(
        buildHeraFrame(
          type: HeraFrameType.bye,
          payload: buildHeraByePayload(text: 'stop'),
        ),
        _bytes('05 00 05 00 01 73 74 6f 70'),
      );
    });
  });

  group('§14 vectors, decoding', () {
    test('device HELLO', () {
      // Built by hand: the builder always writes the client role.
      final payload = Uint8List(Hera.helloSize + 14);
      payload.setRange(0, 4, Hera.magic);
      payload[4] = Hera.protocolVersion;
      payload[5] = 1; // device
      payload[6] = HeraFormat.allMask;
      payload[7] = Hera.allDurations;
      payload[12] = 0x44;
      payload[13] = 0x33;
      payload[14] = 0x22;
      payload[15] = 0x11;
      for (var i = 0; i < 32; i++) {
        payload[16 + i] = 0x10 + i;
      }
      for (var i = 0; i < 16; i++) {
        payload[48 + i] = 0xA0 + i;
      }
      payload.setRange(
          Hera.helloSize, payload.length, utf8.encode('hera-rpi 0.4.0'));

      final message = decodeHeraFrame(
        buildHeraFrame(type: HeraFrameType.hello, payload: payload),
      );
      expect(message, isA<HeraHello>());
      final hello = message! as HeraHello;
      expect(hello.version, Hera.protocolVersion);
      expect(hello.isSupported, isTrue);
      expect(hello.fromDevice, isTrue);
      expect(hello.sessionId, 0x11223344);
      expect(hello.resume, isFalse);
      expect(hello.software, 'hera-rpi 0.4.0');
      expect(hello.supportedFormats, HeraFormat.values);
      expect(hello.supportedDurations, [5, 10, 20]);
      expect(hello.publicKey.first, 0x10);
      expect(hello.tag.first, 0xA0);
    });

    test('HELLO without mandatory formats or durations is rejected', () {
      Uint8List hello({int formats = 0x0F, int durations = 0x07}) {
        final payload = Uint8List(Hera.helloSize);
        payload.setRange(0, 4, Hera.magic);
        payload[4] = Hera.protocolVersion;
        payload[6] = formats;
        payload[7] = durations;
        return buildHeraFrame(type: HeraFrameType.hello, payload: payload);
      }

      expect(decodeHeraFrame(hello()), isA<HeraHello>());
      // §5.3: PCM16/8k, G.711 A-law and 20 ms are mandatory.
      expect(() => decodeHeraFrame(hello(formats: 0x02)),
          throwsA(isA<HeraProtocolException>()));
      expect(() => decodeHeraFrame(hello(durations: 0x04)),
          throwsA(isA<HeraProtocolException>()));
    });

    test('EVENT READY carries the selected format', () {
      final message = decodeHeraFrame(_bytes('''
        20 00 12 00 60 04 01 00
        68 65 72 61 2d 72 70 69 20 30 2e 33 2e 30
      '''));
      final event = message! as HeraEvent;
      expect(event.id, HeraEventId.ready);
      expect(event.a1, Hera.protocolVersion);
      expect(event.format, HeraFormat.pcm16At8k);
      expect(event.text, 'hera-rpi 0.3.0');
    });

    test('EVENT AUDIO_FMT carries format, duration and epoch', () {
      final event =
          decodeHeraFrame(_bytes('20 00 04 00 12 01 14 01'))! as HeraEvent;
      expect(event.id, HeraEventId.audioFormat);
      expect(event.format, HeraFormat.pcm16At8k);
      expect(event.a2 & 0xFF, 20, reason: 'duration in the low byte');
      expect(event.epoch, 1);
    });

    test('EVENT AUDIO_OPEN describes the SCO link', () {
      final event =
          decodeHeraFrame(_bytes('20 00 04 00 10 01 50 00'))! as HeraEvent;
      expect(event.id, HeraEventId.audioOpen);
      expect(event.a1, 1, reason: 'CVSD');
      // 8000 / 100: the SCO rate, not the wire format rate (§13.1).
      expect(event.a2, 80);
    });

    test('EVENT CLIP', () {
      final event = decodeHeraFrame(_bytes('''
        20 00 11 00 21 00 00 00
        2b 33 39 30 35 31 32 33 34 35 36 37 38
      '''))! as HeraEvent;
      expect(event.id, HeraEventId.clip);
      expect(event.text, '+390512345678');
    });

    test('AUDIO_RX, PCM16 at 8 kHz', () {
      final frame = Uint8List(332);
      frame.setRange(0, 12, _bytes('10 00 48 01 01 14 10 01 00 00 00 00'));
      final block = decodeHeraFrame(frame)! as HeraAudioBlock;
      expect(block.format, HeraFormat.pcm16At8k);
      expect(block.frameMs, 20);
      expect(block.count, 1);
      expect(block.epoch, 1);
      expect(block.filler, isFalse);
      expect(block.timestamp, 0);
      expect(block.sampleCount, 160);
      expect(block.samples, hasLength(320));
      expect(block.duration, const Duration(milliseconds: 20));
    });

    test('STATS', () {
      final stats = decodeHeraFrame(_bytes('''
        31 00 2c 00
        39 30 00 00 58 02 00 00 59 02 00 00 00 00 00 00
        03 00 00 00 00 00 00 00 02 00 00 00 00 00 00 00
        1e 00 04 00 01 0f 0c d1 01 01 00 00
      '''))! as HeraStats;
      expect(stats.uptimeMs, 12345);
      expect(stats.downlinkBlocks, 600);
      expect(stats.uplinkBlocks, 601);
      expect(stats.downlinkDropped, 0);
      expect(stats.uplinkLost, 3);
      expect(stats.uplinkLate, 0);
      expect(stats.uplinkFill, 2);
      expect(stats.uplinkBad, 0);
      expect(stats.uplinkBufferedMs, 30);
      expect(stats.uplinkJitterMs, 4);
      expect(stats.scoCodec, 1, reason: 'CVSD');
      expect(stats.cpuPercent, 12);
      expect(stats.rssi, -47, reason: 'rssi is a signed byte');
      expect(stats.netMode, HeraNetMode.accessPoint);
      expect(stats.format, HeraFormat.pcm16At8k);
      expect(stats.isSlcUp, isTrue);
      expect(stats.isAudioOpen, isTrue);
      expect(stats.isInCall, isTrue);
      expect(stats.isLinkProtected, isTrue);
    });

    test('BYE', () {
      final bye =
          decodeHeraFrame(_bytes('05 00 05 00 01 73 74 6f 70'))! as HeraBye;
      expect(bye.reason, HeraByeReason.normal);
      expect(bye.text, 'stop');
      // A session closed with BYE is never resumed (§5.6).
      expect(HeraByeReason.allowsResume(bye.reason), isFalse);
    });
  });
}
