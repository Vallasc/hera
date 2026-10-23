import 'dart:convert';
import 'dart:typed_data';

import 'pcm.dart';

/// Encodes [samples] as a mono 16-bit PCM WAV file.
Uint8List wavBytes(Float32List samples, {required int sampleRate}) {
  final data = pcm16BytesOf(samples);
  const headerSize = 44;

  final bytes = Uint8List(headerSize + data.length);
  final view = ByteData.view(bytes.buffer);

  bytes.setAll(0, ascii.encode('RIFF'));
  view.setUint32(4, 36 + data.length, Endian.little);
  bytes.setAll(8, ascii.encode('WAVE'));

  bytes.setAll(12, ascii.encode('fmt '));
  view.setUint32(16, 16, Endian.little); // fmt chunk size
  view.setUint16(20, 1, Endian.little); // PCM
  view.setUint16(22, 1, Endian.little); // mono
  view.setUint32(24, sampleRate, Endian.little);
  view.setUint32(28, sampleRate * 2, Endian.little); // byte rate
  view.setUint16(32, 2, Endian.little); // block align
  view.setUint16(34, 16, Endian.little); // bits per sample

  bytes.setAll(36, ascii.encode('data'));
  view.setUint32(40, data.length, Endian.little);
  bytes.setAll(headerSize, data);

  return bytes;
}
