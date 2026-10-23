import 'dart:convert';
import 'dart:typed_data';

import 'package:hera_audio/hera_audio.dart';

/// Wire format of the HERA protocol, version 4.
///
/// Serialisation only: no I/O, no global state, no Flutter dependency. Tested
/// against the vectors of `PROTOCOL.md` §14.
abstract final class Hera {
  static const int protocolVersion = 4;

  /// `TYPE`, `FLAGS`, `LEN` (§5.1).
  static const int headerSize = 4;

  static const int maxPayload = 1024;
  static const int maxFrame = headerSize + maxPayload;

  /// `"HERA"`, the first four bytes of a `HELLO` payload.
  static const List<int> magic = [0x48, 0x45, 0x52, 0x41];

  /// Size of the fixed part of a `HELLO` payload; an optional string follows.
  ///
  /// The token is not part of `HELLO`; it is only used as the HMAC key for
  /// the tag (§11.4).
  static const int helloSize = 64;

  /// Session token length (§11.4).
  static const int tokenSize = 16;

  /// Audio payload header size (§6.3).
  static const int audioHeaderSize = 8;

  /// Longest block duration, for sizing buffers (§6.2).
  static const int maxFrameMs = 20;

  /// Block durations defined in §6.2, in ms.
  static const List<int> durations = [20, 10, 5];

  /// Bit for [ms] in the `HELLO` `durations` bitmap, or 0 if invalid.
  static int durationBit(int ms) => switch (ms) {
        20 => 0x01,
        10 => 0x02,
        5 => 0x04,
        _ => 0,
      };

  /// 20 ms is mandatory (§6.2).
  static const int mandatoryDurations = 0x01;

  /// Bitmap with all durations set.
  static const int allDurations = 0x07;

  static bool isValidDuration(int ms) => durationBit(ms) != 0;

  /// Durations set in [mask], shortest first.
  static List<int> durationsFromMask(int mask) =>
      [5, 10, 20].where((ms) => mask & durationBit(ms) != 0).toList();

  /// Session id prefix of a UDP datagram (§4.2).
  static const int sessionIdSize = 4;

  static const int defaultControlPort = 7420;
  static const int defaultAudioPort = 7421;

  /// Serial line speed in `USB` mode (§2.3).
  static const int serialBaudRate = 460800;

  /// `STATS` (§10.1) and `CSTATS` (§10.2) payload sizes.
  static const int statsSize = 44;
  static const int clientStatsSize = 20;
}

/// Frame types (§5.2).
abstract final class HeraFrameType {
  static const int hello = 0x01;
  static const int ping = 0x02;
  static const int pong = 0x03;
  static const int bye = 0x05;
  static const int audioRx = 0x10;
  static const int audioTx = 0x11;
  static const int event = 0x20;
  static const int command = 0x21;
  static const int ack = 0x22;
  static const int log = 0x30;
  static const int stats = 0x31;
  static const int clientStats = 0x32;
}

/// Audio formats (§6.2): linear PCM or G.711.
enum HeraFormat {
  pcm16At8k(0x01, AudioEncoding.pcm16, 8000),
  pcm16At16k(0x02, AudioEncoding.pcm16, 16000),
  g711a(0x03, AudioEncoding.alaw, 8000),
  g711u(0x04, AudioEncoding.ulaw, 8000);

  const HeraFormat(this.id, this.encoding, this.sampleRate);

  final int id;
  final AudioEncoding encoding;
  final int sampleRate;

  /// Bit index in the `HELLO` `formats` bitmap: `id - 1`.
  int get bit => id - 1;
  int get mask => 1 << bit;

  /// Samples in one block of [frameMs].
  int samplesPerBlock(int frameMs) => sampleRate * frameMs ~/ 1000;

  /// Bytes in one block of [frameMs].
  int bytesPerBlock(int frameMs) => encoding == AudioEncoding.pcm16
      ? samplesPerBlock(frameMs) * 2
      : samplesPerBlock(frameMs);

  /// Bytes per second of audio, independent of the block duration.
  int get bytesPerSecond =>
      encoding == AudioEncoding.pcm16 ? sampleRate * 2 : sampleRate;

  /// Maximum number of blocks in one payload (§6.3.1): as many as fit in
  /// 1024 bytes, at most 4. For `FMT_PCM16_16K` at 20 ms this is 1.
  int maxBlocksPerPayload(int frameMs) {
    final fits =
        (Hera.maxPayload - Hera.audioHeaderSize) ~/ bytesPerBlock(frameMs);
    return fits > 4 ? 4 : fits;
  }

  /// Byte value for silence: 0x00 for PCM, 0xD5 for A-law, 0xFF for µ-law.
  int get silenceByte => switch (encoding) {
        AudioEncoding.pcm16 => 0x00,
        AudioEncoding.alaw => 0xD5,
        AudioEncoding.ulaw => 0xFF,
      };

  /// The equivalent [AudioFormat].
  AudioFormat get audioFormat => AudioFormat(encoding, sampleRate);

  /// Formats every implementation must support (§6.2).
  static const Set<HeraFormat> mandatory = {pcm16At8k, g711a};

  /// Bitmap with all formats set.
  static int get allMask =>
      values.fold(0, (mask, format) => mask | format.mask);

  static HeraFormat? fromId(int id) {
    for (final format in values) {
      if (format.id == id) return format;
    }
    return null;
  }

  /// Formats set in [mask], in id order.
  static List<HeraFormat> fromMask(int mask) => [
        for (final format in values)
          if (mask & format.mask != 0) format,
      ];
}

/// Event identifiers (§8).
abstract final class HeraEventId {
  static const int btConnected = 0x01;
  static const int btDisconnected = 0x02;
  static const int slcUp = 0x03;
  static const int audioOpen = 0x10;
  static const int audioClose = 0x11;
  static const int audioFormat = 0x12;
  static const int ring = 0x20;
  static const int clip = 0x21;
  static const int call = 0x22;
  static const int callSetup = 0x23;
  static const int callHeld = 0x24;
  static const int volume = 0x30;
  static const int voiceRecognition = 0x40;
  static const int service = 0x50;
  static const int signal = 0x51;
  static const int battery = 0x52;
  static const int operatorName = 0x53;
  static const int ready = 0x60;
  static const int netMode = 0x70;
  static const int error = 0x7F;

  static const Map<int, String> names = {
    btConnected: 'BT_CONNECTED',
    btDisconnected: 'BT_DISCONNECTED',
    slcUp: 'SLC_UP',
    audioOpen: 'AUDIO_OPEN',
    audioClose: 'AUDIO_CLOSE',
    audioFormat: 'AUDIO_FMT',
    ring: 'RING',
    clip: 'CLIP',
    call: 'CALL',
    callSetup: 'CALL_SETUP',
    callHeld: 'CALL_HELD',
    volume: 'VOLUME',
    voiceRecognition: 'VR',
    service: 'SERVICE',
    signal: 'SIGNAL',
    battery: 'BATTERY',
    operatorName: 'OPER',
    ready: 'READY',
    netMode: 'NET_MODE',
    error: 'ERROR',
  };
}

/// Command identifiers (§9).
abstract final class HeraCommandId {
  static const int answer = 0x01;
  static const int hangup = 0x02;
  static const int dial = 0x03;
  static const int redial = 0x04;
  static const int audioConnect = 0x10;
  static const int audioDisconnect = 0x11;
  static const int volumeSpeaker = 0x20;
  static const int volumeMicrophone = 0x21;
  static const int autoAnswer = 0x30;
  static const int connect = 0x40;
  static const int disconnect = 0x41;
  static const int voiceRecognitionStart = 0x50;
  static const int voiceRecognitionStop = 0x51;
  static const int setFormat = 0x60;
  static const int muteUplink = 0x61;
  static const int requestStats = 0x70;
  static const int logLevel = 0x71;
  static const int netMode = 0x72;
  static const int reboot = 0x7F;
}

/// `CALL_SETUP` values (§8).
abstract final class HeraCallSetup {
  static const int none = 0;
  static const int incoming = 1;
  static const int outgoing = 2;
  static const int alerting = 3;
}

/// Link modes (§2).
enum HeraNetMode {
  accessPoint(1, 'Device hotspot'),
  station(2, 'Shared WiFi'),
  usb(3, 'USB cable');

  const HeraNetMode(this.id, this.label);

  final int id;
  final String label;

  static HeraNetMode? fromId(int id) {
    for (final mode in values) {
      if (mode.id == id) return mode;
    }
    return null;
  }
}

/// `EVENT ERROR` codes (§8.2).
abstract final class HeraErrorCode {
  static const int badHello = 1;
  static const int shortPayload = 2;
  static const int scoAudio = 3;
  static const int hfpService = 4;
  static const int badToken = 5;
  static const int resumeRefused = 6;
  static const int network = 7;

  static String describe(int code) => switch (code) {
        badHello => 'handshake rejected',
        shortPayload => 'payload too short',
        scoAudio => 'call audio error',
        hfpService => 'Bluetooth service error',
        badToken => 'invalid session token',
        resumeRefused => 'session resume refused',
        network => 'device network error',
        _ => 'error $code',
      };
}

/// `BYE` reasons (§5.6).
abstract final class HeraByeReason {
  static const int normal = 1;
  static const int busy = 2;
  static const int takenOver = 3;
  static const int reconfiguring = 4;

  /// Whether a resume may be attempted after [reason]. Always false: a `BYE`
  /// is a deliberate close (§5.6).
  static bool allowsResume(int reason) => false;

  static String describe(int reason) => switch (reason) {
        normal => 'normal close',
        busy => 'another client is connected',
        takenOver => 'replaced by another client',
        reconfiguring => 'the device is reconfiguring',
        _ => 'reason $reason',
      };
}

/// A malformed frame on the control channel. The channel must be closed
/// (§5.1).
class HeraProtocolException implements Exception {
  const HeraProtocolException(this.message);

  final String message;

  @override
  String toString() => 'HeraProtocolException: $message';
}

// ----------------------------------------------------------------- encoding ---

/// Builds a complete frame.
Uint8List buildHeraFrame({required int type, Uint8List? payload}) {
  final length = payload?.length ?? 0;
  if (length > Hera.maxPayload) {
    throw HeraProtocolException(
      'payload of $length bytes, maximum '
      '${Hera.maxPayload}',
    );
  }
  final frame = Uint8List(Hera.headerSize + length);
  frame[0] = type;
  frame[1] = 0;
  frame[2] = length & 0xFF;
  frame[3] = (length >> 8) & 0xFF;
  if (payload != null && length > 0) {
    frame.setRange(Hera.headerSize, frame.length, payload);
  }
  return frame;
}

/// `HELLO` payload (§5.3). [tag] is the HMAC computed with the token
/// (§11.4).
Uint8List buildHeraHelloPayload({
  required int formats,
  required int durations,
  required Uint8List publicKey,
  required Uint8List tag,
  int sessionId = 0,
  bool resume = false,
  int role = 0,
  String software = '',
}) {
  if (publicKey.length != 32) {
    throw HeraProtocolException(
      'public key is ${publicKey.length} bytes, expected 32',
    );
  }
  if (tag.length != 16) {
    throw HeraProtocolException(
        'tag is ${tag.length} bytes, expected 16');
  }
  final text = utf8.encode(software);
  final payload = Uint8List(Hera.helloSize + text.length);
  payload.setRange(0, 4, Hera.magic);
  payload[4] = Hera.protocolVersion;
  payload[5] = role;
  payload[6] = formats & 0xFF;
  payload[7] = durations & 0xFF;
  payload[8] = resume ? 0x01 : 0x00;
  // 9..11 reserved, already zero
  _putUint32(payload, 12, sessionId);
  payload.setRange(16, 48, publicKey);
  payload.setRange(48, 64, tag);
  if (text.isNotEmpty) payload.setRange(Hera.helloSize, payload.length, text);
  return payload;
}

/// Payload shared by `EVENT`, `CMD` and `ACK`: `id`, `a1`, `a2` and an
/// optional string (§8, §9).
Uint8List buildHeraMessagePayload({
  required int id,
  int a1 = 0,
  int a2 = 0,
  String text = '',
}) {
  final body = utf8.encode(text);
  final payload = Uint8List(4 + body.length);
  payload[0] = id;
  payload[1] = a1;
  payload[2] = a2 & 0xFF;
  payload[3] = (a2 >> 8) & 0xFF;
  if (body.isNotEmpty) payload.setRange(4, payload.length, body);
  return payload;
}

/// `BYE` payload (§5.6).
Uint8List buildHeraByePayload({
  int reason = HeraByeReason.normal,
  String text = '',
}) {
  final body = utf8.encode(text);
  final payload = Uint8List(1 + body.length);
  payload[0] = reason;
  if (body.isNotEmpty) payload.setRange(1, payload.length, body);
  return payload;
}

/// Audio payload (§6.3).
///
/// [samples] must be a whole number of blocks; `count` is derived from its
/// length. Throws [HeraProtocolException] otherwise, since the peer would
/// silently drop the block.
Uint8List buildHeraAudioPayload({
  required HeraFormat format,
  required int frameMs,
  required Uint8List samples,
  required int timestamp,
  int epoch = 0,
  bool filler = false,
}) {
  if (!Hera.isValidDuration(frameMs)) {
    throw HeraProtocolException('invalid block duration: $frameMs ms');
  }
  final block = format.bytesPerBlock(frameMs);
  if (samples.isEmpty || samples.length % block != 0) {
    throw HeraProtocolException(
      '${samples.length} bytes is not a whole number of $block-byte blocks',
    );
  }
  final count = samples.length ~/ block;
  if (count > format.maxBlocksPerPayload(frameMs)) {
    throw HeraProtocolException(
      '$count blocks do not fit in one payload: ${format.name} at $frameMs ms '
      'allows ${format.maxBlocksPerPayload(frameMs)}',
    );
  }
  final payload = Uint8List(Hera.audioHeaderSize + samples.length);
  payload[0] = format.id;
  payload[1] = frameMs;
  payload[2] = (filler ? 0x01 : 0x00) | ((epoch & 0x0F) << 4);
  payload[3] = count;
  _putUint32(payload, 4, timestamp);
  payload.setRange(Hera.audioHeaderSize, payload.length, samples);
  return payload;
}

/// `CSTATS` payload (§10.2).
Uint8List buildHeraClientStats({
  required int uptimeMs,
  required int downlinkBlocks,
  required int downlinkLost,
  required int uplinkBlocks,
  int bufferedMs = 0,
  int rttMs = 0,
  bool playing = false,
  bool microphoneLive = false,
  bool freeRunning = false,
}) {
  final payload = Uint8List(Hera.clientStatsSize);
  _putUint32(payload, 0, uptimeMs);
  _putUint32(payload, 4, downlinkBlocks);
  _putUint32(payload, 8, downlinkLost);
  _putUint32(payload, 12, uplinkBlocks);
  payload[16] = bufferedMs & 0xFF;
  payload[17] = (bufferedMs >> 8) & 0xFF;
  payload[18] = rttMs > 255 ? 255 : rttMs;
  payload[19] = (playing ? 0x01 : 0) |
      (microphoneLive ? 0x02 : 0) |
      (freeRunning ? 0x04 : 0);
  return payload;
}

void _putUint32(Uint8List out, int at, int value) {
  out[at] = value & 0xFF;
  out[at + 1] = (value >> 8) & 0xFF;
  out[at + 2] = (value >> 16) & 0xFF;
  out[at + 3] = (value >> 24) & 0xFF;
}

int _uint16(Uint8List data, int at) => data[at] | (data[at + 1] << 8);

int _uint32(Uint8List data, int at) =>
    data[at] |
    (data[at + 1] << 8) |
    (data[at + 2] << 16) |
    (data[at + 3] << 24);

// ----------------------------------------------------------------- decoding ---

/// A decoded frame.
sealed class HeraMessage {
  const HeraMessage();
}

/// `HELLO` (§5.3). From the device it carries the assigned `session_id`.
class HeraHello extends HeraMessage {
  const HeraHello({
    required this.version,
    required this.role,
    required this.formats,
    required this.durations,
    required this.flags,
    required this.sessionId,
    required this.resume,
    required this.publicKey,
    required this.tag,
    required this.software,
  });

  final int version;
  final int role;

  /// Bitmap of formats supported by the peer.
  final int formats;

  /// Bitmap of block durations supported by the peer (§6.2).
  final int durations;

  /// `flags` as received; part of the HMAC input (§11.4).
  final int flags;

  final int sessionId;
  final bool resume;

  /// The peer's ephemeral X25519 public key (§11.3).
  final Uint8List publicKey;

  /// HMAC tag over the `HELLO` fields. Must be verified before any key is
  /// derived (§11.4).
  final Uint8List tag;

  final String software;

  bool get isSupported => version == Hera.protocolVersion;
  bool get fromDevice => role == 1;

  List<HeraFormat> get supportedFormats => HeraFormat.fromMask(formats);
  List<int> get supportedDurations => Hera.durationsFromMask(durations);
}

/// An `EVENT` (§8). `CMD` uses the same layout (§9).
class HeraEvent extends HeraMessage {
  const HeraEvent({
    required this.id,
    required this.a1,
    required this.a2,
    required this.text,
  });

  final int id;
  final int a1;
  final int a2;
  final String text;

  String get name => HeraEventId.names[id] ?? '0x${id.toRadixString(16)}';

  /// Format announced by `AUDIO_FMT` (`a1`) or `READY` (`a2`).
  HeraFormat? get format =>
      HeraFormat.fromId(id == HeraEventId.ready ? a2 : a1);

  /// Epoch from the high bits of `a2`, for `AUDIO_FMT` (§6.3).
  int get epoch => (a2 >> 8) & 0x0F;

  @override
  String toString() =>
      '$name(a1: $a1, a2: $a2${text.isEmpty ? '' : ', "$text"'})';
}

/// Command acknowledgement (§9.1).
class HeraAck extends HeraMessage {
  const HeraAck({required this.command, required this.ok});

  final int command;
  final bool ok;
}

/// Orderly close (§5.6).
class HeraBye extends HeraMessage {
  const HeraBye({required this.reason, required this.text});

  final int reason;
  final String text;

  String get description =>
      text.isEmpty ? HeraByeReason.describe(reason) : text;
}

/// `PING` or `PONG`. A `PONG` echoes the `PING` payload (§5.5).
class HeraPing extends HeraMessage {
  const HeraPing({required this.payload, required this.isReply});

  final Uint8List payload;
  final bool isReply;
}

/// An audio payload (§6.3).
class HeraAudioBlock extends HeraMessage {
  const HeraAudioBlock({
    required this.format,
    required this.frameMs,
    required this.count,
    required this.epoch,
    required this.filler,
    required this.timestamp,
    required this.samples,
  });

  /// Format taken from the block header, which is authoritative (§6.3).
  final HeraFormat format;

  final int frameMs;
  final int count;

  /// Incremented at every `AUDIO_OPEN`. A new epoch means a new audio
  /// stream; buffers and counters are reset (§6.3).
  final int epoch;

  /// True if the block is filler rather than audio from the source.
  final bool filler;

  final int timestamp;
  final Uint8List samples;

  /// Number of samples at the rate of [format].
  int get sampleCount => count * format.samplesPerBlock(frameMs);

  /// Bytes in one block of this format and duration.
  int get bytesPerBlock => format.bytesPerBlock(frameMs);

  /// Total duration of the payload.
  Duration get duration => Duration(milliseconds: count * frameMs);
}

/// `STATS` from the device (§10.1).
class HeraStats extends HeraMessage {
  const HeraStats({
    required this.uptimeMs,
    required this.downlinkBlocks,
    required this.uplinkBlocks,
    required this.downlinkDropped,
    required this.uplinkLost,
    required this.uplinkLate,
    required this.uplinkFill,
    required this.uplinkBad,
    required this.uplinkBufferedMs,
    required this.uplinkJitterMs,
    required this.scoCodec,
    required this.flags,
    required this.cpuPercent,
    required this.rssi,
    required this.netMode,
    required this.format,
    this.uplinkTargetMs = 0,
  });

  final int uptimeMs;
  final int downlinkBlocks;
  final int uplinkBlocks;
  final int downlinkDropped;

  /// Blocks missing from the uplink (gaps in `ts`).
  final int uplinkLost;

  /// Blocks received and dropped at the jitter buffer ceiling (§10.1).
  final int uplinkLate;

  final int uplinkFill;
  final int uplinkBad;
  final int uplinkBufferedMs;
  final int uplinkJitterMs;
  final int scoCodec;
  final int flags;
  final int cpuPercent;

  /// Client RSSI in dBm, or 0 if unknown.
  final int rssi;

  final HeraNetMode? netMode;
  final HeraFormat? format;

  /// Current target depth of the device's adaptive jitter buffer (§6.5).
  final int uplinkTargetMs;

  bool get isSlcUp => flags & 0x01 != 0;
  bool get isAudioOpen => flags & 0x02 != 0;
  bool get isInCall => flags & 0x04 != 0;
  bool get isLinkProtected => flags & 0x08 != 0;
}

/// A log line from the device (§5.2).
class HeraLogLine extends HeraMessage {
  const HeraLogLine(this.text);

  final String text;
}

/// Splits a byte stream into frames (§4.1.1). Read boundaries do not match
/// frame boundaries.
class HeraFrameParser {
  final List<int> _buffer = [];

  /// Appends [data] and returns the complete frames.
  ///
  /// Throws [HeraProtocolException] if a declared length exceeds the maximum
  /// payload; the caller must close the channel.
  Iterable<Uint8List> feed(Uint8List data) {
    _buffer.addAll(data);
    final frames = <Uint8List>[];
    while (_buffer.length >= Hera.headerSize) {
      final length = _buffer[2] | (_buffer[3] << 8);
      if (length > Hera.maxPayload) {
        throw HeraProtocolException(
          'declared length $length exceeds '
          '${Hera.maxPayload}: stream out of sync',
        );
      }
      final total = Hera.headerSize + length;
      if (_buffer.length < total) break;
      frames.add(Uint8List.fromList(_buffer.sublist(0, total)));
      _buffer.removeRange(0, total);
    }
    return frames;
  }

  void reset() => _buffer.clear();
}

/// Decodes a complete frame. Returns null for unknown types, which are
/// ignored (§5.1).
HeraMessage? decodeHeraFrame(Uint8List frame, {int? negotiatedMs}) {
  if (frame.length < Hera.headerSize) {
    throw const HeraProtocolException('frame shorter than the header');
  }
  final type = frame[0];
  final length = _uint16(frame, 2);
  if (frame.length < Hera.headerSize + length) {
    throw const HeraProtocolException('truncated frame');
  }
  final payload = Uint8List.sublistView(
    frame,
    Hera.headerSize,
    Hera.headerSize + length,
  );

  switch (type) {
    case HeraFrameType.hello:
      return _decodeHello(payload);
    case HeraFrameType.ping:
    case HeraFrameType.pong:
      return HeraPing(
        payload: Uint8List.fromList(payload),
        isReply: type == HeraFrameType.pong,
      );
    case HeraFrameType.bye:
      _require(payload, 1, 'BYE');
      return HeraBye(reason: payload[0], text: _text(payload, 1));
    case HeraFrameType.event:
      _require(payload, 4, 'EVENT');
      return HeraEvent(
        id: payload[0],
        a1: payload[1],
        a2: _uint16(payload, 2),
        text: _text(payload, 4),
      );
    case HeraFrameType.ack:
      _require(payload, 4, 'ACK');
      return HeraAck(command: payload[0], ok: payload[1] == 0);
    case HeraFrameType.audioRx:
    case HeraFrameType.audioTx:
      return decodeHeraAudioPayload(payload, negotiatedMs: negotiatedMs);
    case HeraFrameType.log:
      return HeraLogLine(_text(payload, 0));
    case HeraFrameType.stats:
      return _decodeStats(payload);
    default:
      return null;
  }
}

/// Decodes a plaintext datagram: session id prefix followed by one frame
/// (§4.2). Returns null if the datagram is invalid or for another session;
/// such datagrams are discarded silently.
HeraMessage? decodeHeraDatagram(Uint8List datagram, {required int sessionId}) {
  if (datagram.length < Hera.sessionIdSize + Hera.headerSize) return null;
  if (_uint32(datagram, 0) != sessionId) return null;
  final frame = Uint8List.sublistView(datagram, Hera.sessionIdSize);
  if (frame.length != Hera.headerSize + _uint16(frame, 2)) return null;
  try {
    return decodeHeraFrame(frame);
  } on HeraProtocolException {
    return null;
  }
}

/// Decodes an audio payload. Returns null if it is malformed; the block is
/// dropped and the channel stays open (§6.3).
HeraAudioBlock? decodeHeraAudioPayload(Uint8List payload, {int? negotiatedMs}) {
  if (payload.length < Hera.audioHeaderSize) return null;
  final format = HeraFormat.fromId(payload[0]);
  if (format == null) return null;
  final frameMs = payload[1];
  if (!Hera.isValidDuration(frameMs)) return null;
  // Blocks with a duration other than the negotiated one are dropped (§6.3).
  if (negotiatedMs != null && frameMs != negotiatedMs) return null;
  final count = payload[3];
  if (count < 1 || count > format.maxBlocksPerPayload(frameMs)) return null;
  final expected = Hera.audioHeaderSize + count * format.bytesPerBlock(frameMs);
  if (payload.length != expected) return null;

  return HeraAudioBlock(
    format: format,
    frameMs: frameMs,
    count: count,
    epoch: (payload[2] >> 4) & 0x0F,
    filler: payload[2] & 0x01 != 0,
    timestamp: _uint32(payload, 4),
    samples: Uint8List.sublistView(payload, Hera.audioHeaderSize),
  );
}

HeraHello _decodeHello(Uint8List payload) {
  _require(payload, Hera.helloSize, 'HELLO');
  for (var i = 0; i < 4; i++) {
    if (payload[i] != Hera.magic[i]) {
      throw const HeraProtocolException('HELLO without the "HERA" magic');
    }
  }
  final formats = payload[6];
  final durations = payload[7];
  // Mandatory formats and the 20 ms duration must be offered (§5.3).
  if (formats & 0x05 != 0x05) {
    throw const HeraProtocolException('HELLO without the mandatory formats');
  }
  if (durations & Hera.mandatoryDurations != Hera.mandatoryDurations) {
    throw const HeraProtocolException('HELLO without the mandatory 20 ms');
  }
  return HeraHello(
    version: payload[4],
    role: payload[5],
    formats: formats,
    durations: durations,
    flags: payload[8],
    sessionId: _uint32(payload, 12),
    resume: payload[8] & 0x01 != 0,
    publicKey: Uint8List.sublistView(payload, 16, 48),
    tag: Uint8List.sublistView(payload, 48, 64),
    software: _text(payload, Hera.helloSize),
  );
}

HeraStats _decodeStats(Uint8List payload) {
  _require(payload, Hera.statsSize, 'STATS');
  return HeraStats(
    uptimeMs: _uint32(payload, 0),
    downlinkBlocks: _uint32(payload, 4),
    uplinkBlocks: _uint32(payload, 8),
    downlinkDropped: _uint32(payload, 12),
    uplinkLost: _uint32(payload, 16),
    uplinkLate: _uint32(payload, 20),
    uplinkFill: _uint32(payload, 24),
    uplinkBad: _uint32(payload, 28),
    uplinkBufferedMs: _uint16(payload, 32),
    uplinkJitterMs: _uint16(payload, 34),
    scoCodec: payload[36],
    flags: payload[37],
    cpuPercent: payload[38],
    rssi: payload[39] >= 0x80 ? payload[39] - 0x100 : payload[39],
    netMode: HeraNetMode.fromId(payload[40]),
    format: HeraFormat.fromId(payload[41]),
    uplinkTargetMs: payload[42],
  );
}

void _require(Uint8List payload, int size, String what) {
  if (payload.length < size) {
    throw HeraProtocolException(
      '$what of ${payload.length} bytes, at least '
      '$size required',
    );
  }
}

/// The optional trailing string of a payload. Not NUL-terminated; its length
/// follows from `LEN` (§8).
String _text(Uint8List payload, int from) => payload.length <= from
    ? ''
    : utf8.decode(Uint8List.sublistView(payload, from), allowMalformed: true);
