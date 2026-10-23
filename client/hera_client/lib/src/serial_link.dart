import 'dart:async';
import 'dart:collection';
import 'dart:typed_data' show BytesBuilder, Uint8List;

import 'package:flutter/foundation.dart';

import 'channel.dart';
import 'protocol.dart';
import 'serial_port.dart';

/// [HeraLink] over a serial line (`PROTOCOL.md` §2.3, §4.4).
///
/// Both channels share one byte stream. Following §4.4 there are two queues:
/// control frames are always written first, and the oldest audio frames are
/// dropped when more than 60 ms of audio is queued. Frames are not encrypted
/// on this transport (§11.1).
class SerialHeraLink extends HeraLinkBase {
  SerialHeraLink(this._connection);

  final SerialConnection _connection;

  /// Audio queue limit: 60 ms, i.e. three 20 ms frames (§4.4).
  static const int maxQueuedAudio = 3;

  /// Audio frames batched into one write. On a serial line the cost is per
  /// write rather than per byte (§4.1.1).
  static const int framesPerWrite = 2;

  /// Writes taking longer than this are logged.
  static const Duration slowWrite = Duration(milliseconds: 200);

  final Queue<Uint8List> _controlQueue = Queue<Uint8List>();
  final Queue<_AudioFrame> _audioQueue = Queue<_AudioFrame>();

  Uint8List? _held;
  Future<void>? _draining;
  Timer? _flush;
  StreamSubscription<Uint8List>? _reader;
  bool _closing = false;

  void start() {
    _reader = _connection.incoming.listen(
      ingestControl,
      onError: (Object error) => finish('$error'),
      onDone: () => finish(null),
      cancelOnError: true,
    );
  }

  @override
  void sendControl(int type, [Uint8List? payload]) {
    if (_closing) return;
    _controlQueue.add(buildHeraFrame(type: type, payload: payload));
    _schedule(urgent: true);
  }

  @override
  void sendAudio(int type, [Uint8List? payload]) {
    if (_closing) return;
    // No session prefix and no audio binding on a serial line (§4.4).
    if (type == HeraFrameType.ping) return;

    final blocks = payload != null ? payload[3] : 0;
    _audioQueue.add(_AudioFrame(
      buildHeraFrame(type: type, payload: payload),
      blocks,
    ));
    while (_audioQueue.length > maxQueuedAudio) {
      countDroppedAudio(_audioQueue.removeFirst().blocks);
    }
    _schedule(urgent: _audioQueue.length >= framesPerWrite);
  }

  /// Writes immediately if [urgent]; otherwise waits up to 20 ms so that
  /// audio frames can be batched.
  void _schedule({required bool urgent}) {
    if (urgent) {
      _flush?.cancel();
      _flush = null;
      _draining ??= _drain();
      return;
    }
    _flush ??= Timer(const Duration(milliseconds: 20), () {
      _flush = null;
      _draining ??= _drain();
    });
  }

  Uint8List? _next() {
    final held = _held;
    if (held != null) {
      _held = null;
      return held;
    }
    if (_controlQueue.isNotEmpty) return _controlQueue.removeFirst();
    if (_audioQueue.isNotEmpty) {
      final frame = _audioQueue.removeFirst();
      countSentAudio(frame.blocks);
      return frame.bytes;
    }
    return null;
  }

  Future<void> _drain() async {
    try {
      while (
          _controlQueue.isNotEmpty || _audioQueue.isNotEmpty || _held != null) {
        final batch = BytesBuilder(copy: false);
        for (var frame = _next(); frame != null; frame = _next()) {
          // Keep each write within one maximum frame size.
          if (batch.isNotEmpty && batch.length + frame.length > Hera.maxFrame) {
            _held = frame;
            break;
          }
          batch.add(frame);
        }
        if (batch.isEmpty) break;

        final started = DateTime.now();
        await _connection.write(batch.takeBytes());
        final took = DateTime.now().difference(started);
        if (took > slowWrite) {
          debugPrint('SerialHeraLink: write took '
              '${took.inMilliseconds} ms; the line is not keeping up');
        }
      }
    } catch (error) {
      // The reader reports the closed link; just discard pending frames.
      debugPrint('SerialHeraLink: write failed, $error');
      _controlQueue.clear();
      _audioQueue.clear();
      _held = null;
    } finally {
      _draining = null;
    }
  }

  @override
  Future<void> close() async {
    if (_closing) return;
    _closing = true;
    _flush?.cancel();
    _flush = null;
    // Drop queued audio and wait for the write in progress.
    _audioQueue.clear();
    await _draining;
    await _reader?.cancel();
    _reader = null;
    await _connection.close();
    await closeStreams();
  }
}

/// A queued audio frame and the number of blocks it carries.
class _AudioFrame {
  const _AudioFrame(this.bytes, this.blocks);

  final Uint8List bytes;
  final int blocks;
}
