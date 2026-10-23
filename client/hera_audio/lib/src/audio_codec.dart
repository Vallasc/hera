import 'dart:typed_data';

import 'audio_format.dart';
import 'g711.dart';
import 'pcm.dart';
import 'resampler.dart';

/// Decodes wire-format audio into mono float samples at [targetRate]
/// (default [kCoreSampleRate]).
///
/// State is kept across packets: a packet need not contain a whole number of
/// samples, and the resampler is continuous across calls.
class AudioDecoder {
  AudioDecoder({required this.format, this.targetRate = kCoreSampleRate})
      : _resampler = Resampler(
          inputRate: format.sampleRate,
          outputRate: targetRate,
        );

  final AudioFormat format;
  final int targetRate;
  final Resampler _resampler;

  /// Low byte left over from a PCM16 packet of odd length. Dropping it would
  /// misalign every following sample.
  int? _pendingByte;

  /// Decodes one packet and returns the resampled samples.
  Float32List add(Uint8List bytes) => _resampler.process(_toLinear(bytes));

  /// Ends the stream and returns the resampler tail.
  Float32List flush() {
    _pendingByte = null;
    return _resampler.flush();
  }

  void reset() {
    _pendingByte = null;
    _resampler.reset();
  }

  Float32List _toLinear(Uint8List bytes) {
    if (format.encoding != AudioEncoding.pcm16) {
      final decode = format.encoding == AudioEncoding.ulaw
          ? G711.decodeUlaw
          : G711.decodeAlaw;
      final samples = Float32List(bytes.length);
      for (var i = 0; i < bytes.length; i++) {
        samples[i] = decode(bytes[i]) / kFullScale;
      }
      return samples;
    }

    final carry = _pendingByte;
    final total = bytes.length + (carry != null ? 1 : 0);
    final count = total ~/ 2;
    _pendingByte = total.isOdd ? bytes[bytes.length - 1] : null;

    final samples = Float32List(count);
    var offset = 0; // read position in `bytes`
    for (var i = 0; i < count; i++) {
      final int low;
      final int high;
      if (i == 0 && carry != null) {
        low = carry;
        high = bytes[offset++];
      } else {
        low = bytes[offset++];
        high = bytes[offset++];
      }
      // Little endian, sign-extended.
      final value = (high << 8) | low;
      samples[i] = (value >= 0x8000 ? value - 0x10000 : value) / kFullScale;
    }
    return samples;
  }
}

/// Encodes mono float samples at [sourceRate] into [format].
class AudioEncoder {
  AudioEncoder({required this.sourceRate, required this.format})
      : _resampler = Resampler(
          inputRate: sourceRate,
          outputRate: format.sampleRate,
        );

  final int sourceRate;
  final AudioFormat format;
  final Resampler _resampler;

  /// Encodes [samples] and returns the wire bytes.
  Uint8List add(Float32List samples) => _fromLinear(_resampler.process(samples));

  /// Ends a segment: returns the resampler tail and resets the state. Call at
  /// the end of a segment, not between consecutive chunks of one stream.
  Uint8List flush() => _fromLinear(_resampler.flush());

  void reset() => _resampler.reset();

  Uint8List _fromLinear(Float32List samples) {
    if (format.encoding != AudioEncoding.pcm16) {
      final encode = format.encoding == AudioEncoding.ulaw
          ? G711.encodeUlaw
          : G711.encodeAlaw;
      final bytes = Uint8List(samples.length);
      for (var i = 0; i < samples.length; i++) {
        bytes[i] = encode(toPcm16(samples[i]));
      }
      return bytes;
    }

    return pcm16BytesOf(samples);
  }
}
