import 'dart:typed_data';

/// Scale factor between 16-bit integer samples and normalised floats.
///
/// 32767 is used in both directions, which keeps the conversion symmetric and
/// free of DC offset at the cost of never producing -32768 from -1.0.
const double kFullScale = 32767.0;

/// Converts normalised samples to 16-bit little-endian linear PCM.
Uint8List pcm16BytesOf(Float32List samples) {
  final bytes = Uint8List(samples.length * 2);
  for (var i = 0; i < samples.length; i++) {
    final value = toPcm16(samples[i]);
    bytes[i * 2] = value & 0xFF;
    bytes[i * 2 + 1] = (value >> 8) & 0xFF;
  }
  return bytes;
}

/// Converts a normalised sample to a 16-bit integer, with saturation. The
/// resampler can overshoot slightly beyond ±1.0 on steep transients.
int toPcm16(double sample) {
  final scaled = (sample * kFullScale).round();
  if (scaled > 32767) return 32767;
  if (scaled < -32768) return -32768;
  return scaled;
}

