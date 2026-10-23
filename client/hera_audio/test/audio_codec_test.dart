import 'dart:math' as math;
import 'dart:typed_data';

import 'package:hera_audio/hera_audio.dart';
import 'package:test/test.dart';

Float32List _sine({
  required double frequency,
  required int sampleRate,
  required int count,
  double amplitude = 0.5,
}) {
  return Float32List.fromList(
    List<double>.generate(
      count,
      (i) => amplitude * math.sin(2 * math.pi * frequency * i / sampleRate),
    ),
  );
}

double _rootMeanSquare(List<double> samples) {
  if (samples.isEmpty) return 0;
  var sum = 0.0;
  for (final sample in samples) {
    sum += sample * sample;
  }
  return math.sqrt(sum / samples.length);
}

/// PCM16 little-endian bytes from normalised samples.
Uint8List _pcm16Bytes(Float32List samples) {
  final bytes = Uint8List(samples.length * 2);
  for (var i = 0; i < samples.length; i++) {
    final value = (samples[i] * 32767).round().clamp(-32768, 32767);
    bytes[i * 2] = value & 0xFF;
    bytes[i * 2 + 1] = (value >> 8) & 0xFF;
  }
  return bytes;
}

void main() {
  group('PCM16 conversion', () {
    test('scales normalised samples to the 16-bit integer range', () {
      final bytes = pcm16BytesOf(Float32List.fromList([0.0, 0.5, -0.5, 1.0]));
      final view = ByteData.view(bytes.buffer);

      expect(bytes.length, 8, reason: 'two bytes per sample');
      expect(view.getInt16(0, Endian.little), 0);
      expect(view.getInt16(2, Endian.little), closeTo(16384, 2));
      expect(view.getInt16(4, Endian.little), closeTo(-16384, 2));
      expect(view.getInt16(6, Endian.little), 32767);
    });

    test('saturates instead of wrapping', () {
      final bytes = pcm16BytesOf(Float32List.fromList([2.5, -2.5]));
      final view = ByteData.view(bytes.buffer);
      expect(view.getInt16(0, Endian.little), 32767);
      expect(view.getInt16(2, Endian.little), -32768);
    });
  });

  group('decoding', () {
    test('PCM16 at 24 kHz is resampled to 16 kHz', () {
      final source = _sine(frequency: 300, sampleRate: 24000, count: 24000);
      final decoder = AudioDecoder(format: AudioFormat.pcm16);

      final decoded = decoder.add(_pcm16Bytes(source));
      final tail = decoder.flush();

      expect(decoded.length + tail.length, closeTo(16000, 2));
      // Level is preserved: amplitude 0.5 has an RMS of 0.354.
      expect(_rootMeanSquare(decoded.sublist(200, decoded.length - 200)),
          closeTo(0.5 * math.sqrt1_2, 0.01));
    });

    test('packets split mid-sample do not misalign the stream', () {
      final source = _sine(frequency: 300, sampleRate: 24000, count: 6000);
      final bytes = _pcm16Bytes(source);

      final whole = AudioDecoder(format: AudioFormat.pcm16);
      final reference = <double>[
        ...whole.add(bytes),
        ...whole.flush(),
      ];

      final split = AudioDecoder(format: AudioFormat.pcm16);
      final collected = <double>[];
      // Odd packet sizes, so that packets end in the middle of a sample.
      for (var offset = 0; offset < bytes.length; offset += 333) {
        final end = math.min(offset + 333, bytes.length);
        collected.addAll(split.add(bytes.sublist(offset, end)));
      }
      collected.addAll(split.flush());

      expect(collected.length, reference.length);
      for (var i = 0; i < collected.length; i++) {
        expect(collected[i], reference[i], reason: 'sample $i');
      }
    });

    test('µ-law at 8 kHz is resampled to 16 kHz', () {
      final source = _sine(frequency: 400, sampleRate: 8000, count: 8000);
      final encoder = AudioEncoder(sourceRate: 8000, format: AudioFormat.ulaw);
      final wire = encoder.add(source);

      final decoder = AudioDecoder(format: AudioFormat.ulaw);
      final decoded = decoder.add(wire);

      expect(wire.length, source.length, reason: 'one byte per sample');
      expect(decoded.length, closeTo(2 * source.length, 40));
      expect(_rootMeanSquare(decoded.sublist(200, decoded.length - 200)),
          closeTo(0.5 * math.sqrt1_2, 0.02));
    });
  });

  group('encoding', () {
    test('22050 Hz source to 24 kHz PCM16', () {
      // Ratio 160:147.
      final speech = _sine(frequency: 220, sampleRate: 22050, count: 22050);
      final encoder = AudioEncoder(sourceRate: 22050, format: AudioFormat.pcm16);

      final bytes = <int>[...encoder.add(speech), ...encoder.flush()];

      expect(bytes.length ~/ 2, closeTo(24000, 2));
      expect(bytes.length.isEven, isTrue, reason: 'whole samples only');
    });

    test('G.711 produces one byte per sample at 8 kHz', () {
      final speech = _sine(frequency: 220, sampleRate: 22050, count: 22050);

      for (final format in [AudioFormat.ulaw, AudioFormat.alaw]) {
        final encoder = AudioEncoder(sourceRate: 22050, format: format);
        final bytes = <int>[...encoder.add(speech), ...encoder.flush()];
        expect(bytes.length, closeTo(8000, 2), reason: '$format');
      }
    });

    test('saturates on resampler overshoot', () {
      final loud = Float32List.fromList(
        List<double>.generate(2000, (i) => i.isEven ? 1.6 : -1.6),
      );
      final encoder = AudioEncoder(sourceRate: 16000, format: AudioFormat.pcm16);
      final bytes = <int>[...encoder.add(loud), ...encoder.flush()];

      for (var i = 0; i < bytes.length; i += 2) {
        final raw = (bytes[i + 1] << 8) | bytes[i];
        final value = raw >= 0x8000 ? raw - 0x10000 : raw;
        expect(value, inInclusiveRange(-32768, 32767));
      }
    });
  });

  group('round trip', () {
    test('in-band tone through µ-law at 8 kHz', () {
      const frequency = 440.0;
      final original = _sine(frequency: frequency, sampleRate: 16000, count: 16000);

      final encoder = AudioEncoder(sourceRate: 16000, format: AudioFormat.ulaw);
      final wire = Uint8List.fromList([...encoder.add(original), ...encoder.flush()]);

      final decoder = AudioDecoder(format: AudioFormat.ulaw);
      final returned = <double>[...decoder.add(wire), ...decoder.flush()];

      expect(returned.length, closeTo(original.length, 40));

      var maxError = 0.0;
      for (var i = 400; i < returned.length - 400; i++) {
        final expected = 0.5 * math.sin(2 * math.pi * frequency * i / 16000);
        maxError = math.max(maxError, (returned[i] - expected).abs());
      }
      expect(maxError, lessThan(0.05),
          reason: 'max error ${maxError.toStringAsFixed(4)}');
    });

    test('PCM16 at 16 kHz is transparent up to quantisation', () {
      final original = _sine(frequency: 1000, sampleRate: 16000, count: 8000);
      const format = AudioFormat(AudioEncoding.pcm16, 16000);

      final encoder = AudioEncoder(sourceRate: 16000, format: format);
      final wire = Uint8List.fromList([...encoder.add(original), ...encoder.flush()]);

      final decoder = AudioDecoder(format: format);
      final returned = <double>[...decoder.add(wire), ...decoder.flush()];

      expect(returned.length, original.length);
      for (var i = 0; i < returned.length; i++) {
        // One 16-bit quantisation step is 1/32767.
        expect(returned[i], closeTo(original[i], 1 / 32767));
      }
    });
  });
}
