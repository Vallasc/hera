import 'dart:async';
import 'dart:io';
import 'dart:typed_data' show BytesBuilder, Uint8List;

import 'package:flutter/foundation.dart';

import 'channel.dart';
import 'protocol.dart';
import 'sec.dart';

/// [HeraLink] over IP (`PROTOCOL.md` §4): TCP with `TCP_NODELAY` for the
/// control channel, UDP for the audio channel.
class WifiHeraLink extends HeraLinkBase {
  WifiHeraLink._(this._control, this._audio, this._device, this._audioPort);

  final Socket _control;
  final RawDatagramSocket _audio;
  final InternetAddress _device;
  final int _audioPort;

  StreamSubscription<Uint8List>? _controlReader;

  /// Control bytes not yet processed (at most a partial record, up to 2082
  /// bytes).
  final BytesBuilder _pending = BytesBuilder(copy: false);
  StreamSubscription<RawSocketEvent>? _audioReader;
  bool _closing = false;

  /// Opens both sockets. Throws [SocketException] if the control connection
  /// fails.
  static Future<WifiHeraLink> connect({
    required InternetAddress address,
    int controlPort = Hera.defaultControlPort,
    int audioPort = Hera.defaultAudioPort,
    Duration timeout = const Duration(seconds: 5),
  }) async {
    final control =
        await Socket.connect(address, controlPort, timeout: timeout);
    // §4.1.
    control.setOption(SocketOption.tcpNoDelay, true);

    final RawDatagramSocket audio;
    try {
      // Ephemeral port; the device learns it from our first datagram
      // (§4.2.1).
      audio = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
    } catch (error) {
      await control.close();
      control.destroy();
      rethrow;
    }

    final link = WifiHeraLink._(control, audio, address, audioPort);
    link._start();
    return link;
  }

  void _start() {
    // Write errors, e.g. ECONNABORTED when the network goes away, are reported
    // on `done`, not on the stream. Unobserved, they would be unhandled.
    unawaited(_control.done.then((_) {}, onError: (Object error) {
      finish('control: $error');
    }));
    _controlReader = _control.listen(
      _onControlBytes,
      onError: (Object error) => finish('control: $error'),
      onDone: () => finish(null),
      cancelOnError: true,
    );
    _audioReader = _audio.listen((event) {
      if (event != RawSocketEvent.read) return;
      // Drain the socket on every read event.
      //
      // `receive()` can throw, e.g. ECONNABORTED after an ICMP port
      // unreachable caused by an earlier datagram. An exception thrown in the
      // `onData` callback would not reach `onError`, so it is caught here.
      // Audio errors never end the session; the control channel defines it
      // (§4.1).
      try {
        for (var packet = _audio.receive();
            packet != null;
            packet = _audio.receive()) {
          ingestAudioDatagram(packet.data);
        }
      } on SocketException catch (error) {
        countDroppedAudio();
        debugPrint('WifiHeraLink: audio, $error');
      }
    }, onError: (Object error) {
      // Not fatal: the control channel defines the session (§4.1).
      debugPrint('WifiHeraLink: audio, $error');
    });
  }

  @override
  void sendControl(int type, [Uint8List? payload]) {
    if (_closing) return;
    final frame = buildHeraFrame(type: type, payload: payload);
    final keys = sec;
    try {
      // Plaintext only before the keys exist, i.e. for `HELLO`; records
      // afterwards (§11.2).
      _control.add(keys == null
          ? frame
          : sealHeraRecord(keys.controlOut, frame));
    } catch (error) {
      finish('control write: $error');
    }
  }

  @override
  void sendAudio(int type, [Uint8List? payload]) {
    if (_closing) return;
    // No session id yet: the device would discard the datagram.
    if (sessionId == 0) {
      countDroppedAudio();
      return;
    }

    // Nothing is sent in the clear on the audio channel (§11.2).
    final keys = sec;
    if (keys == null) {
      countDroppedAudio();
      return;
    }

    final datagram = sealHeraDatagram(
      keys.audioOut,
      sessionId,
      buildHeraFrame(type: type, payload: payload),
    );
    final blocks =
        type == HeraFrameType.audioTx && payload != null ? payload[3] : 0;
    try {
      final written = _audio.send(datagram, _device, _audioPort);
      // 0 means the socket did not accept the datagram: drop it (§6.6).
      if (written == 0) {
        countDroppedAudio(blocks);
        return;
      }
      countSentAudio(blocks);
    } on SocketException {
      countDroppedAudio(blocks);
    }
  }

  /// Time allowed for a graceful close. A device that has been switched off
  /// never acknowledges, and waiting for it would hold up reconnection.
  static const Duration closeTimeout = Duration(milliseconds: 500);

  @override
  Future<void> close() async {
    if (_closing) return;
    _closing = true;
    // Reported first: whoever waits on `closed` to reconnect must not depend
    // on the socket closing cleanly.
    finish(null);
    await _controlReader?.cancel();
    _controlReader = null;
    await _audioReader?.cancel();
    _audioReader = null;
    _audio.close();
    try {
      await _control.flush().timeout(closeTimeout);
      await _control.close().timeout(closeTimeout);
    } catch (_) {
      // Already closed, or the peer is gone.
    }
    _control.destroy();
    await closeStreams();
  }

  /// Processes control bytes buffered while the keys were being derived,
  /// without waiting for the next socket read.
  @override
  void onSecurityReady() {
    if (_closing || _pending.isEmpty) return;
    _onControlBytes(Uint8List(0));
  }

  /// Receive path of the control channel.
  ///
  /// Before key derivation the stream carries plaintext frames; afterwards it
  /// carries records, each containing one or more frames (§11.2). A record
  /// that fails to open closes the link.
  void _onControlBytes(Uint8List data) {
    _pending.add(data);
    final buffer = _pending.toBytes();
    var offset = 0;

    while (true) {
      final keys = sec;
      if (keys == null) {
        // Plaintext phase (§11.2).
        if (buffer.length - offset < Hera.headerSize) break;
        final length = buffer[offset + 2] | (buffer[offset + 3] << 8);
        if (length > Hera.maxPayload) {
          finish('declared length $length: stream out of sync');
          return;
        }
        final total = Hera.headerSize + length;
        if (buffer.length - offset < total) break;
        ingestControl(Uint8List.sublistView(buffer, offset, offset + total));
        offset += total;

        // Stop if the keys are not available yet. Key derivation is
        // asynchronous, and the device's first record (READY) often arrives
        // in the same TCP segment as its HELLO; it must not be parsed as a
        // plaintext frame. The remaining bytes stay in `_pending` and are
        // processed by onSecurityReady().
        if (sec == null) break;
        continue;
      }

      if (buffer.length - offset < 2) break;
      final recordLength = heraRecordLength(buffer, offset);
      if (recordLength == 0) {
        finish('record length out of range');
        return;
      }
      if (buffer.length - offset < recordLength) break;

      try {
        final plain = openHeraRecord(
          keys.controlIn,
          Uint8List.sublistView(buffer, offset, offset + recordLength),
        );
        offset += recordLength;
        ingestControl(plain);
      } on HeraProtocolException catch (error) {
        finish('record rejected: $error');
        return;
      }
    }

    _pending
      ..clear()
      ..add(Uint8List.sublistView(buffer, offset));
  }
}
