import 'dart:math' as math;

import 'package:hera_audio/hera_audio.dart';
import 'package:test/test.dart';

/// Signal-to-noise ratio between original and reconstructed signal, in dB.
double _signalToNoiseDb(List<double> original, List<double> reconstructed) {
  var signal = 0.0;
  var noise = 0.0;
  for (var i = 0; i < original.length; i++) {
    final error = original[i] - reconstructed[i];
    signal += original[i] * original[i];
    noise += error * error;
  }
  return 10 * math.log(signal / noise) / math.ln10;
}

List<double> _sine({
  required double frequency,
  required int sampleRate,
  required int count,
  double amplitude = 0.5,
}) {
  return List<double>.generate(
    count,
    (i) => amplitude * math.sin(2 * math.pi * frequency * i / sampleRate),
  );
}

void main() {
  group('µ-law', () {
    test('encode(decode(code)) == code', () {
      // Except 0x7F (negative zero), which decodes to 0 and re-encodes as
      // 0xFF.
      for (var code = 0; code < 256; code++) {
        if (code == 0x7F) continue;
        expect(
          G711.encodeUlaw(G711.decodeUlaw(code)),
          code,
          reason: 'µ-law code $code',
        );
      }
    });

    test('both zero codes decode to 0; 0 encodes as 0xFF', () {
      expect(G711.decodeUlaw(0x7F), 0, reason: 'negative zero');
      expect(G711.decodeUlaw(0xFF), 0, reason: 'positive zero');
      expect(G711.encodeUlaw(0), 0xFF, reason: 'canonical zero');
    });

    test('decoded values fit in 16 bits', () {
      for (var code = 0; code < 256; code++) {
        final value = G711.decodeUlaw(code);
        expect(value, inInclusiveRange(-32768, 32767));
      }
    });

    test('full scale saturates without sign change', () {
      expect(G711.decodeUlaw(G711.encodeUlaw(32767)), greaterThan(30000));
      expect(G711.decodeUlaw(G711.encodeUlaw(-32768)), lessThan(-30000));
    });

    test('zero round-trips within one quantisation step', () {
      expect(G711.decodeUlaw(G711.encodeUlaw(0)).abs(), lessThan(64));
    });

    test('SNR above 25 dB on a 440 Hz tone', () {
      final original = _sine(frequency: 440, sampleRate: 8000, count: 4000);
      final reconstructed = original
          .map((s) => G711.decodeUlaw(G711.encodeUlaw((s * 32767).round())) / 32767)
          .toList();
      expect(_signalToNoiseDb(original, reconstructed), greaterThan(25));
    });
  });

  group('A-law', () {
    test('encode(decode(code)) == code', () {
      for (var code = 0; code < 256; code++) {
        expect(
          G711.encodeAlaw(G711.decodeAlaw(code)),
          code,
          reason: 'A-law code $code',
        );
      }
    });

    test('decoded values fit in 16 bits', () {
      for (var code = 0; code < 256; code++) {
        expect(G711.decodeAlaw(code), inInclusiveRange(-32768, 32767));
      }
    });

    test('full scale saturates without sign change', () {
      expect(G711.decodeAlaw(G711.encodeAlaw(32767)), greaterThan(30000));
      expect(G711.decodeAlaw(G711.encodeAlaw(-32768)), lessThan(-30000));
    });

    test('SNR above 25 dB on a 440 Hz tone', () {
      final original = _sine(frequency: 440, sampleRate: 8000, count: 4000);
      final reconstructed = original
          .map((s) => G711.decodeAlaw(G711.encodeAlaw((s * 32767).round())) / 32767)
          .toList();
      expect(_signalToNoiseDb(original, reconstructed), greaterThan(25));
    });
  });

  test('µ-law and A-law encodings differ', () {
    final differences = List<int>.generate(256, (i) => i)
        .where((code) => G711.encodeUlaw(code * 100 - 12800) != G711.encodeAlaw(code * 100 - 12800))
        .length;
    expect(differences, greaterThan(200));
  });
}
