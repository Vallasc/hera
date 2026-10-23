/// ITU-T G.711 µ-law and A-law.
///
/// Follows the CCITT reference implementation, including segment tables and
/// XOR masks. The device implementation (`g711.c`) uses the same algorithm.
class G711 {
  G711._();

  static const int _bias = 0x84;
  static const int _ulawClip = 8159; // 14-bit domain
  static const int _alawClip = 4095; // 13-bit domain

  static const List<int> _ulawSegmentEnd = [
    0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF,
  ];

  static const List<int> _alawSegmentEnd = [
    0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF,
  ];

  static int _segment(int value, List<int> ends) {
    for (var i = 0; i < ends.length; i++) {
      if (value <= ends[i]) return i;
    }
    return ends.length;
  }

  /// 16-bit linear PCM to µ-law.
  static int encodeUlaw(int pcm) {
    var value = pcm >> 2; // 14 bit
    final int mask;
    if (value < 0) {
      value = -value;
      mask = 0x7F;
    } else {
      mask = 0xFF;
    }
    if (value > _ulawClip) value = _ulawClip;
    value += _bias >> 2;

    final segment = _segment(value, _ulawSegmentEnd);
    if (segment >= 8) return 0x7F ^ mask;
    return ((segment << 4) | ((value >> (segment + 1)) & 0x0F)) ^ mask;
  }

  /// µ-law to 16-bit linear PCM.
  static int decodeUlaw(int code) {
    final value = ~code & 0xFF;
    var magnitude = ((value & 0x0F) << 3) + _bias;
    magnitude <<= (value & 0x70) >> 4;
    return (value & 0x80) != 0 ? _bias - magnitude : magnitude - _bias;
  }

  /// 16-bit linear PCM to A-law.
  static int encodeAlaw(int pcm) {
    var value = pcm >> 3; // 13 bit
    final int mask;
    if (value >= 0) {
      mask = 0xD5;
    } else {
      mask = 0x55;
      value = -value - 1;
    }
    if (value > _alawClip) value = _alawClip;

    final segment = _segment(value, _alawSegmentEnd);
    if (segment >= 8) return 0x7F ^ mask;

    var encoded = segment << 4;
    encoded |= segment < 2 ? (value >> 1) & 0x0F : (value >> segment) & 0x0F;
    return encoded ^ mask;
  }

  /// A-law to 16-bit linear PCM.
  static int decodeAlaw(int code) {
    final value = code ^ 0x55;
    var magnitude = (value & 0x0F) << 4;
    final segment = (value & 0x70) >> 4;
    switch (segment) {
      case 0:
        magnitude += 8;
      case 1:
        magnitude += 0x108;
      default:
        magnitude += 0x108;
        magnitude <<= segment - 1;
    }
    return (value & 0x80) != 0 ? magnitude : -magnitude;
  }
}
