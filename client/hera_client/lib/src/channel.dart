import 'dart:async';
import 'dart:typed_data';

import 'protocol.dart';
import 'sec.dart';

/// An open link to the device, carrying the control and audio channels.
///
/// Over WiFi the channels are a TCP socket and a UDP socket (`PROTOCOL.md`
/// §4.1, §4.2); over a serial line they are multiplexed on one byte stream
/// (§4.4). This interface hides the difference.
///
/// Implementations never queue audio (§6.6). Blocks that cannot be written
/// immediately are dropped and counted in [droppedAudio].
abstract class HeraLink {
  /// Decoded control-channel messages. Unknown frame types are omitted.
  Stream<HeraMessage> get control;

  /// Decoded and validated audio blocks.
  Stream<HeraAudioBlock> get audio;

  /// Completes when the link closes, for any reason.
  Future<void> get closed;

  /// The `session_id` assigned by the device, used as the datagram prefix
  /// (§4.2).
  int get sessionId;
  set sessionId(int value);

  /// Negotiated block duration (§6.2), used to validate incoming blocks.
  int get frameMs;
  set frameMs(int value);

  /// Session keys (§11.3). While null, control frames are sent and received
  /// in the clear; this is only the case for `HELLO` (§11.2). Set by the
  /// session after the peer's tag has been verified.
  HeraSecSession? get sec;
  set sec(HeraSecSession? value);

  /// Audio blocks dropped because the channel did not accept them (§6.6), and
  /// received datagrams that failed to open.
  int get droppedAudio;

  /// Audio blocks handed to the channel (`CSTATS.ul_blocks`, §13.2).
  int get sentAudio;

  void sendControl(int type, [Uint8List? payload]);

  /// Sends a frame on the audio channel. It may be dropped (§6.6).
  void sendAudio(int type, [Uint8List? payload]);

  Future<void> close();

  // ---------------------------------------------------------- helpers ---

  /// Sends `HELLO` (§5.3). [tag] is computed with the token (§11.4).
  void hello({
    required int formats,
    required int durations,
    required Uint8List publicKey,
    required Uint8List tag,
    int sessionId = 0,
    bool resume = false,
    String software = 'hera_client',
  }) =>
      sendControl(
        HeraFrameType.hello,
        buildHeraHelloPayload(
          formats: formats,
          durations: durations,
          publicKey: publicKey,
          tag: tag,
          sessionId: sessionId,
          resume: resume,
          software: software,
        ),
      );

  void sendCommand(int id, {int a1 = 0, int a2 = 0, String text = ''}) =>
      sendControl(
        HeraFrameType.command,
        buildHeraMessagePayload(id: id, a1: a1, a2: a2, text: text),
      );

  void bye({int reason = HeraByeReason.normal, String text = ''}) =>
      sendControl(
        HeraFrameType.bye,
        buildHeraByePayload(reason: reason, text: text),
      );

  /// Sends `PING` on the audio channel, which binds the audio flow
  /// (§4.2.1).
  void pingAudio() => sendAudio(HeraFrameType.ping);

  /// Sends an `AUDIO_TX` frame (§6.3).
  void sendAudioBlock({
    required HeraFormat format,
    required int frameMs,
    required Uint8List samples,
    required int timestamp,
    int epoch = 0,
    bool filler = false,
  }) =>
      sendAudio(
        HeraFrameType.audioTx,
        buildHeraAudioPayload(
          format: format,
          frameMs: frameMs,
          samples: samples,
          timestamp: timestamp,
          epoch: epoch,
          filler: filler,
        ),
      );
}

/// Transport-independent part of [HeraLink]: control stream parsing, message
/// dispatch, counters and close handling.
abstract class HeraLinkBase extends HeraLink {
  final HeraFrameParser _parser = HeraFrameParser();
  final StreamController<HeraMessage> _control =
      StreamController<HeraMessage>.broadcast();
  final StreamController<HeraAudioBlock> _audio =
      StreamController<HeraAudioBlock>.broadcast();
  final Completer<void> _closed = Completer<void>();

  int _sessionId = 0;
  int _dropped = 0;
  int _sent = 0;

  @override
  int frameMs = Hera.maxFrameMs;

  HeraSecSession? _sec;

  @override
  HeraSecSession? get sec => _sec;

  @override
  set sec(HeraSecSession? value) {
    final appeared = _sec == null && value != null;
    _sec = value;
    // Key derivation is asynchronous, so control bytes may have arrived in the
    // meantime. They must be processed as records from now on.
    if (appeared) onSecurityReady();
  }

  /// Called when the session keys become available. Transports override it
  /// to process control bytes buffered during the handshake.
  void onSecurityReady() {}

  @override
  Stream<HeraMessage> get control => _control.stream;

  @override
  Stream<HeraAudioBlock> get audio => _audio.stream;

  @override
  Future<void> get closed => _closed.future;

  @override
  int get sessionId => _sessionId;

  @override
  set sessionId(int value) => _sessionId = value;

  @override
  int get droppedAudio => _dropped;

  @override
  int get sentAudio => _sent;

  /// The error that closed the link, if any.
  String? get error => _error;
  String? _error;

  /// Parses plaintext control bytes and dispatches the resulting messages.
  /// A malformed frame closes the link (§5.1).
  void ingestControl(Uint8List data) {
    try {
      for (final frame in _parser.feed(data)) {
        final message = decodeHeraFrame(frame, negotiatedMs: frameMs);
        if (message == null) continue;
        // Audio frames arrive here on the serial transport, where both
        // channels share one stream (§4.4).
        if (message is HeraAudioBlock) {
          _emitAudio(message);
        } else {
          _emitControl(message);
        }
      }
    } on HeraProtocolException catch (error) {
      finish('$error');
    }
  }

  /// Processes one audio datagram. Invalid datagrams are discarded and never
  /// close the link (§4.2).
  void ingestAudioDatagram(Uint8List datagram) {
    final keys = sec;
    if (keys == null) return;
    final frame = openHeraDatagram(keys.audioIn, _sessionId, datagram);
    if (frame == null) {
      countDroppedAudio(0);
      return;
    }
    if (frame.length != Hera.headerSize + _uint16le(frame, 2)) return;
    final message = decodeHeraFrame(frame, negotiatedMs: frameMs);
    switch (message) {
      case HeraAudioBlock block:
        _emitAudio(block);
      case HeraPing ping:
        // A `PONG` confirms that the audio path works in both directions
        // (§4.2.1).
        _emitControl(ping);
      default:
        break;
    }
  }

  static int _uint16le(Uint8List data, int at) =>
      data[at] | (data[at + 1] << 8);

  void _emitControl(HeraMessage message) {
    if (!_control.isClosed) _control.add(message);
  }

  void _emitAudio(HeraAudioBlock block) {
    if (!_audio.isClosed) _audio.add(block);
  }

  void countSentAudio(int blocks) => _sent += blocks;

  void countDroppedAudio([int blocks = 1]) => _dropped += blocks;

  /// Marks the link as closed. Idempotent.
  void finish(String? error) {
    if (_closed.isCompleted) return;
    _error = error;
    _closed.complete();
  }

  /// Closes the streams. Call at the end of `close()`.
  Future<void> closeStreams() async {
    finish(null);
    await _control.close();
    await _audio.close();
  }
}
