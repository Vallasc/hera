/// Default internal sample rate (16 kHz), as expected by common VAD and
/// speech recognition models.
const int kCoreSampleRate = 16000;

/// Sample encoding on the wire.
enum AudioEncoding {
  /// 16-bit signed linear PCM, little endian.
  pcm16,

  /// G.711 µ-law, 8 bits per sample.
  ulaw,

  /// G.711 A-law, 8 bits per sample.
  alaw,
}

/// Audio format negotiation error. [message] is suitable for returning to a
/// client.
class AudioFormatException implements Exception {
  const AudioFormatException(this.message);

  final String message;

  @override
  String toString() => 'AudioFormatException: $message';
}

/// An audio encoding and sample rate.
///
/// [parse] accepts both naming conventions used by Realtime-style session
/// APIs: plain strings (`pcm16`, `g711_ulaw`, `g711_alaw`) and objects with a
/// MIME type (`{"type": "audio/pcm", "rate": 24000}`, `audio/pcmu`,
/// `audio/pcma`). [toJson] produces the object form.
class AudioFormat {
  const AudioFormat(this.encoding, this.sampleRate);

  /// Linear PCM at 24 kHz, the default when no format is given.
  static const AudioFormat pcm16 = AudioFormat(AudioEncoding.pcm16, 24000);

  /// G.711 µ-law, 8 kHz.
  static const AudioFormat ulaw = AudioFormat(AudioEncoding.ulaw, 8000);

  /// G.711 A-law, 8 kHz.
  static const AudioFormat alaw = AudioFormat(AudioEncoding.alaw, 8000);

  final AudioEncoding encoding;
  final int sampleRate;

  /// Bytes per sample: 2 for linear PCM, 1 for G.711.
  int get bytesPerSample => encoding == AudioEncoding.pcm16 ? 2 : 1;

  /// MIME type name.
  String get mimeType => switch (encoding) {
        AudioEncoding.pcm16 => 'audio/pcm',
        AudioEncoding.ulaw => 'audio/pcmu',
        AudioEncoding.alaw => 'audio/pcma',
      };

  /// Parses a format specification: `null` (the default, [pcm16]), a string
  /// in either convention, or an object with `type` and optional `rate`.
  /// Throws [AudioFormatException] for anything else.
  factory AudioFormat.parse(Object? specification) {
    if (specification == null) return AudioFormat.pcm16;

    if (specification is String) {
      return AudioFormat._fromName(specification, null);
    }

    if (specification is Map) {
      final type = specification['type'];
      if (type is! String) {
        throw const AudioFormatException(
          'audio format without a "type" field; '
          'expected e.g. {"type": "audio/pcm", "rate": 24000}',
        );
      }
      final rate = specification['rate'];
      if (rate != null && rate is! int) {
        throw AudioFormatException(
          'non-numeric sample rate: "$rate"',
        );
      }
      return AudioFormat._fromName(type, rate as int?);
    }

    throw AudioFormatException(
      'cannot parse audio format: "$specification"',
    );
  }

  factory AudioFormat._fromName(String name, int? requestedRate) {
    final encoding = switch (name) {
      'pcm16' || 'audio/pcm' => AudioEncoding.pcm16,
      'g711_ulaw' || 'audio/pcmu' => AudioEncoding.ulaw,
      'g711_alaw' || 'audio/pcma' => AudioEncoding.alaw,
      _ => throw AudioFormatException(
          'unknown audio format: "$name". '
          'Supported: pcm16, g711_ulaw, g711_alaw, '
          'audio/pcm, audio/pcmu, audio/pcma',
        ),
    };

    if (encoding != AudioEncoding.pcm16) {
      // G.711 is defined at 8 kHz only.
      if (requestedRate != null && requestedRate != 8000) {
        throw AudioFormatException(
          '$name supports 8000 Hz only, $requestedRate Hz requested',
        );
      }
      return AudioFormat(encoding, 8000);
    }

    final rate = requestedRate ?? 24000;
    if (rate < 8000 || rate > 48000) {
      throw AudioFormatException(
        'sample rate out of range: $rate Hz, expected 8000 to 48000',
      );
    }
    return AudioFormat(AudioEncoding.pcm16, rate);
  }

  /// Object form, e.g. for a `session.updated` reply.
  Map<String, Object> toJson() => {
        'type': mimeType,
        if (encoding == AudioEncoding.pcm16) 'rate': sampleRate,
      };

  @override
  bool operator ==(Object other) =>
      other is AudioFormat &&
      other.encoding == encoding &&
      other.sampleRate == sampleRate;

  @override
  int get hashCode => Object.hash(encoding, sampleRate);

  @override
  String toString() => '$mimeType@${sampleRate}Hz';
}
