import 'dart:math' as math;
import 'dart:typed_data';

/// Streaming sample rate converter for arbitrary rational ratios.
///
/// Implements upsample-by-L, low-pass, downsample-by-M in polyphase form: only
/// the filter taps that fall on actual input samples are evaluated, at a cost
/// of about `2 * halfWidth` multiply-accumulates per output sample,
/// independent of L and M (e.g. 22050 to 8000 Hz is 160:441).
///
/// State persists across calls, so the output does not depend on how the
/// input is split into packets. Algorithmic latency is [halfWidth] input
/// samples (under 0.7 ms at 24 kHz).
class Resampler {
  /// Creates a converter from [inputRate] to [outputRate].
  ///
  /// [halfWidth] is the number of sinc zero crossings on each side, i.e. the
  /// filter half-length in input samples. Larger values give a sharper
  /// transition band at the cost of CPU and latency.
  Resampler({
    required this.inputRate,
    required this.outputRate,
    this.halfWidth = 16,
  })  : assert(inputRate > 0, 'invalid input rate'),
        assert(outputRate > 0, 'invalid output rate'),
        assert(halfWidth >= 1, 'halfWidth must be at least 1') {
    final divisor = _greatestCommonDivisor(inputRate, outputRate);
    _up = outputRate ~/ divisor;
    _down = inputRate ~/ divisor;
    _isIdentity = _up == 1 && _down == 1;
    if (!_isIdentity) {
      _center = halfWidth * _up;
      _taps = _buildTaps();
    }
  }

  final int inputRate;
  final int outputRate;
  final int halfWidth;

  /// Interpolation factor L and decimation factor M, in lowest terms.
  late final int _up;
  late final int _down;

  /// Equal rates: input is passed through unfiltered.
  late final bool _isIdentity;

  /// Index of the centre tap, in the upsampled domain.
  late final int _center;

  /// Low-pass prototype impulse response, normalised per phase.
  late final Float64List _taps;

  /// Input samples still needed. Element `j` is at position `j * _up` in the
  /// upsampled domain.
  final List<double> _pending = [];

  /// Position of the next output sample in the upsampled domain, relative to
  /// `_pending[0]`. Advances by `_down` per output and is rebased whenever
  /// consumed input is discarded.
  int _position = 0;

  /// Converts [input] and returns every output sample that can be computed so
  /// far. The remainder is produced by later calls.
  Float32List process(Float32List input) {
    if (_isIdentity) return input;
    _pending.addAll(input);
    return _emit(untilEndOfStream: false);
  }

  /// Ends the stream: produces the tail, treating samples past the end as
  /// silence, and resets the state. For finite segments only.
  Float32List flush() {
    if (_isIdentity) return Float32List(0);
    final tail = _emit(untilEndOfStream: true);
    reset();
    return tail;
  }

  /// Resets the converter, discarding pending input.
  void reset() {
    _pending.clear();
    _position = 0;
  }

  /// Converts a complete segment, including the tail.
  static Float32List convert(
    Float32List input, {
    required int inputRate,
    required int outputRate,
    int halfWidth = 16,
  }) {
    final resampler = Resampler(
      inputRate: inputRate,
      outputRate: outputRate,
      halfWidth: halfWidth,
    );
    final head = resampler.process(input);
    final tail = resampler.flush();
    if (tail.isEmpty) return head;
    final result = Float32List(head.length + tail.length)
      ..setAll(0, head)
      ..setAll(head.length, tail);
    return result;
  }

  /// Upper estimate of the output length for [inputSamples] input samples.
  /// The exact count depends on the current phase.
  int estimateOutputLength(int inputSamples) =>
      (inputSamples * outputRate / inputRate).ceil();

  // --------------------------------------------------------------------------

  /// Produces all computable output, then discards input that no future
  /// output needs.
  Float32List _emit({required bool untilEndOfStream}) {
    final available = _pending.length;
    final output = <double>[];

    while (true) {
      if (untilEndOfStream) {
        // At end of stream, produce outputs centred within the input.
        if (_position >= available * _up) break;
      } else {
        // While streaming, wait until every input the filter needs has
        // arrived.
        if (_floorDiv(_position + _center, _up) >= available) break;
      }

      final first = math.max(0, _ceilDiv(_position - _center, _up));
      final last = math.min(available - 1, _floorDiv(_position + _center, _up));

      // Outside [first, last] the input is treated as silence; this only
      // happens at the start and at the end of the stream.
      var accumulator = 0.0;
      for (var j = first; j <= last; j++) {
        accumulator += _pending[j] * _taps[_position - j * _up + _center];
      }
      output.add(accumulator);
      _position += _down;
    }

    _discardConsumed();
    return Float32List.fromList(output);
  }

  /// Discards input samples no future output depends on.
  void _discardConsumed() {
    final stillNeeded = math.max(0, _ceilDiv(_position - _center, _up));
    final removable = math.min(stillNeeded, _pending.length);
    if (removable <= 0) return;
    _pending.removeRange(0, removable);
    // Rebase the position to the new `_pending[0]`.
    _position -= removable * _up;
  }

  /// Builds the low-pass prototype: a Blackman-windowed sinc with its cutoff
  /// at the lower of the two Nyquist frequencies, which serves both as the
  /// anti-imaging and the anti-aliasing filter.
  Float64List _buildTaps() {
    final length = 2 * _center + 1;
    final taps = Float64List(length);
    final cutoff = 0.5 / math.max(_up, _down);

    for (var i = 0; i < length; i++) {
      final offset = i - _center;
      final sinc = offset == 0
          ? 2 * cutoff
          : math.sin(2 * math.pi * cutoff * offset) / (math.pi * offset);
      final phase = 2 * math.pi * i / (length - 1);
      final window =
          0.42 - 0.5 * math.cos(phase) + 0.08 * math.cos(2 * phase);
      taps[i] = sinc * window;
    }

    _normalizePhases(taps);
    return taps;
  }

  /// Normalises each of the L polyphase sub-filters to unity DC gain.
  ///
  /// Scaling by L analytically leaves small per-phase gain differences, which
  /// appear as periodic amplitude modulation; normalising each phase avoids
  /// that.
  void _normalizePhases(Float64List taps) {
    final sums = Float64List(_up);
    for (var i = 0; i < taps.length; i++) {
      sums[i % _up] += taps[i];
    }
    for (var i = 0; i < taps.length; i++) {
      final sum = sums[i % _up];
      if (sum != 0) taps[i] /= sum;
    }
  }

  static int _greatestCommonDivisor(int a, int b) {
    while (b != 0) {
      final remainder = a % b;
      a = b;
      b = remainder;
    }
    return a;
  }

  /// Floor division. Dart's `~/` truncates towards zero, which is wrong for
  /// the negative indices that occur at the start of a stream.
  static int _floorDiv(int a, int b) {
    final quotient = a ~/ b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? quotient - 1 : quotient;
  }

  static int _ceilDiv(int a, int b) => -_floorDiv(-a, b);
}
