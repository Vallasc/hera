import 'dart:math' as math;
import 'dart:typed_data';

import 'package:hera_audio/hera_audio.dart';
import 'package:test/test.dart';

Float32List _sine({
  required double frequency,
  required int sampleRate,
  required int count,
  double amplitude = 1.0,
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

void main() {
  group('equal rates', () {
    test('pass the signal through', () {
      final input = _sine(frequency: 440, sampleRate: 16000, count: 256);
      final output = Resampler.convert(
        input,
        inputRate: 16000,
        outputRate: 16000,
      );
      expect(output, orderedEquals(input));
    });
  });

  group('length', () {
    test('follows the rate ratio', () {
      for (final (input, output) in [
        (24000, 16000),
        (8000, 16000),
        (22050, 24000),
        (22050, 8000),
      ]) {
        final samples = _sine(frequency: 200, sampleRate: input, count: input);
        final converted = Resampler.convert(
          samples,
          inputRate: input,
          outputRate: output,
        );
        // One second in, one second out, within rounding.
        expect(
          converted.length,
          closeTo(output, 2),
          reason: '$input to $output Hz',
        );
      }
    });
  });

  group('fidelity', () {
    test('DC gain is exactly one', () {
      // Checks the per-phase normalisation.
      final input = Float32List.fromList(List<double>.filled(2000, 0.5));
      final output = Resampler.convert(
        input,
        inputRate: 22050,
        outputRate: 24000,
      );
      // Skip the edges, where the filter is partially outside the input.
      final middle = output.sublist(200, output.length - 200);
      for (final sample in middle) {
        expect(sample, closeTo(0.5, 0.001));
      }
    });

    test('in-band sine is preserved with no phase shift', () {
      const frequency = 300.0;
      final input = _sine(frequency: frequency, sampleRate: 24000, count: 24000);
      final output = Resampler.convert(
        input,
        inputRate: 24000,
        outputRate: 16000,
      );

      // The filter is linear-phase and centred, so output sample n is at
      // time n / outputRate.
      var maxError = 0.0;
      for (var i = 200; i < output.length - 200; i++) {
        final expected = math.sin(2 * math.pi * frequency * i / 16000);
        maxError = math.max(maxError, (output[i] - expected).abs());
      }
      expect(maxError, lessThan(0.01));
    });
  });

  group('anti-aliasing', () {
    test('frequency above the output Nyquist is suppressed', () {
      // 6 kHz at 22050 Hz converted to 8 kHz; without filtering it would
      // alias to 2 kHz.
      final input = _sine(frequency: 6000, sampleRate: 22050, count: 22050);
      final output = Resampler.convert(
        input,
        inputRate: 22050,
        outputRate: 8000,
      );
      final residual = _rootMeanSquare(output.sublist(400, output.length - 400));
      expect(residual, lessThan(0.02),
          reason: 'residual RMS ${residual.toStringAsFixed(4)}');
    });

    test('in-band frequency passes', () {
      final input = _sine(frequency: 1000, sampleRate: 22050, count: 22050);
      final output = Resampler.convert(
        input,
        inputRate: 22050,
        outputRate: 8000,
      );
      final level = _rootMeanSquare(output.sublist(400, output.length - 400));
      expect(level, closeTo(math.sqrt1_2, 0.02));
    });
  });

  group('streaming', () {
    test('output does not depend on input chunking', () {
      final input = _sine(frequency: 440, sampleRate: 24000, count: 12000);
      final reference = Resampler.convert(
        input,
        inputRate: 24000,
        outputRate: 16000,
      );

      for (final chunkSize in [1, 7, 160, 480, 1024, 4096]) {
        final resampler = Resampler(inputRate: 24000, outputRate: 16000);
        final collected = <double>[];
        for (var offset = 0; offset < input.length; offset += chunkSize) {
          final end = math.min(offset + chunkSize, input.length);
          collected.addAll(resampler.process(input.sublist(offset, end)));
        }
        collected.addAll(resampler.flush());

        expect(collected.length, reference.length,
            reason: 'chunks of $chunkSize samples');
        for (var i = 0; i < collected.length; i++) {
          expect(collected[i], reference[i],
              reason: 'chunks of $chunkSize samples, index $i');
        }
      }
    });

    test('reset restores the initial state', () {
      final input = _sine(frequency: 440, sampleRate: 8000, count: 800);
      final resampler = Resampler(inputRate: 8000, outputRate: 16000);

      final first = resampler.process(input);
      resampler.reset();
      final second = resampler.process(input);

      expect(second, orderedEquals(first));
    });

    test('produces nothing until enough input has arrived', () {
      final resampler = Resampler(inputRate: 22050, outputRate: 8000);
      // Fewer samples than the filter half-width.
      expect(resampler.process(Float32List(4)), isEmpty);
    });
  });
}
