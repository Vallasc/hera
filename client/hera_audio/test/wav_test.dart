import 'dart:convert';
import 'dart:typed_data';

import 'package:hera_audio/hera_audio.dart';
import 'package:test/test.dart';

void main() {
  group('WAV header', () {
    test('describes mono 16-bit PCM at the given rate', () {
      final bytes = wavBytes(Float32List(800), sampleRate: 16000);
      final view = ByteData.view(bytes.buffer);
      String tag(int at) => ascii.decode(bytes.sublist(at, at + 4));

      expect(tag(0), 'RIFF');
      expect(tag(8), 'WAVE');
      expect(tag(12), 'fmt ');
      expect(tag(36), 'data');

      expect(view.getUint16(20, Endian.little), 1, reason: 'PCM');
      expect(view.getUint16(22, Endian.little), 1, reason: 'mono');
      expect(view.getUint32(24, Endian.little), 16000);
      expect(view.getUint32(28, Endian.little), 32000, reason: 'byte rate');
      expect(view.getUint16(32, Endian.little), 2, reason: 'block align');
      expect(view.getUint16(34, Endian.little), 16, reason: 'bits per sample');
    });

    test('declared lengths match the data', () {
      final bytes = wavBytes(Float32List(1234), sampleRate: 16000);
      final view = ByteData.view(bytes.buffer);

      expect(bytes.length, 44 + 1234 * 2);
      expect(view.getUint32(40, Endian.little), 1234 * 2, reason: 'data chunk');
      expect(view.getUint32(4, Endian.little), bytes.length - 8,
          reason: 'RIFF length');
    });

    test('duration is preserved', () {
      final bytes = wavBytes(Float32List(8000), sampleRate: 16000);
      final view = ByteData.view(bytes.buffer);
      final seconds = view.getUint32(40, Endian.little) /
          view.getUint32(28, Endian.little);
      expect(seconds, closeTo(0.5, 0.001));
    });
  });
}
