import 'package:hera_audio/hera_audio.dart';
import 'package:test/test.dart';

void main() {
  group('plain string convention', () {
    test('parses the three names', () {
      expect(AudioFormat.parse('pcm16'), AudioFormat.pcm16);
      expect(AudioFormat.parse('g711_ulaw'), AudioFormat.ulaw);
      expect(AudioFormat.parse('g711_alaw'), AudioFormat.alaw);
    });
  });

  group('MIME type convention', () {
    test('parses the three types as string or object', () {
      expect(AudioFormat.parse('audio/pcm'), AudioFormat.pcm16);
      expect(AudioFormat.parse('audio/pcmu'), AudioFormat.ulaw);
      expect(AudioFormat.parse('audio/pcma'), AudioFormat.alaw);
      expect(AudioFormat.parse({'type': 'audio/pcmu'}), AudioFormat.ulaw);
    });

    test('accepts an explicit rate for linear PCM', () {
      final format = AudioFormat.parse({'type': 'audio/pcm', 'rate': 16000});
      expect(format.encoding, AudioEncoding.pcm16);
      expect(format.sampleRate, 16000);
    });
  });

  group('defaults and limits', () {
    test('null gives the default', () {
      expect(AudioFormat.parse(null), AudioFormat.pcm16);
      expect(AudioFormat.pcm16.sampleRate, 24000);
    });

    test('G.711 is always 8 kHz', () {
      expect(AudioFormat.ulaw.sampleRate, 8000);
      expect(AudioFormat.alaw.sampleRate, 8000);
      expect(AudioFormat.parse({'type': 'audio/pcma', 'rate': 8000}).sampleRate, 8000);
    });

    test('bytes per sample', () {
      expect(AudioFormat.pcm16.bytesPerSample, 2);
      expect(AudioFormat.ulaw.bytesPerSample, 1);
      expect(AudioFormat.alaw.bytesPerSample, 1);
    });
  });

  group('rejections', () {
    test('G.711 at a rate other than 8 kHz is rejected', () {
      expect(
        () => AudioFormat.parse({'type': 'audio/pcmu', 'rate': 16000}),
        throwsA(isA<AudioFormatException>()),
      );
    });

    test('unknown format lists the supported ones', () {
      expect(
        () => AudioFormat.parse('opus'),
        throwsA(
          isA<AudioFormatException>().having(
            (e) => e.message,
            'message',
            allOf(contains('opus'), contains('audio/pcmu')),
          ),
        ),
      );
    });

    test('rate out of range is rejected', () {
      expect(
        () => AudioFormat.parse({'type': 'audio/pcm', 'rate': 96000}),
        throwsA(isA<AudioFormatException>()),
      );
    });

    test('object without type is rejected', () {
      expect(
        () => AudioFormat.parse({'rate': 24000}),
        throwsA(isA<AudioFormatException>()),
      );
    });
  });

  test('toJson includes the rate for linear PCM only', () {
    expect(AudioFormat.pcm16.toJson(), {'type': 'audio/pcm', 'rate': 24000});
    expect(AudioFormat.ulaw.toJson(), {'type': 'audio/pcmu'});
    expect(AudioFormat.alaw.toJson(), {'type': 'audio/pcma'});
  });
}
