/// Audio primitives: PCM16 conversion, G.711, sample rate conversion and WAV
/// encoding.
///
/// Pure Dart, with no Flutter dependency, no I/O and no global state.
library;

export 'src/audio_codec.dart';
export 'src/audio_format.dart';
export 'src/g711.dart';
export 'src/pcm.dart';
export 'src/resampler.dart';
export 'src/wav.dart';
