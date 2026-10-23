import 'dart:async';

import 'package:flutter/foundation.dart';

import 'channel.dart';
import 'jitter.dart';
import 'protocol.dart';
import 'sec.dart';

/// Uplink samples for the phone, and whether they are filler.
@immutable
class HeraUplink {
  const HeraUplink(this.samples, {this.filler = false});

  /// Exactly `blocks * format.bytesPerBlock(frameMs)` bytes.
  final Uint8List samples;

  /// True if the samples are filler sent to keep the pace (`PROTOCOL.md`
  /// §6.3).
  final bool filler;
}

/// Source of uplink audio.
///
/// Called once per received downlink block (§6.4), not when audio becomes
/// available. Must always return the requested number of blocks, padding with
/// silence, otherwise the device's jitter buffer underruns.
typedef HeraUplinkSource = HeraUplink Function(int blocks, HeraFormat format);

/// A gap in the downlink, to be concealed during playback.
@immutable
class HeraGap {
  const HeraGap({
    required this.blocks,
    required this.format,
    required this.frameMs,
  });

  final int blocks;
  final HeraFormat format;
  final int frameMs;

  int get samples => blocks * format.samplesPerBlock(frameMs);
  int get bytes => blocks * format.bytesPerBlock(frameMs);
  Duration get duration => Duration(milliseconds: blocks * frameMs);
}

/// A protocol session over an open [HeraLink].
///
/// Handles the handshake, format negotiation, uplink pacing and statistics.
/// It does not reconnect; when the link closes the session ends, and
/// [HeraConnection] starts a new one or resumes it.
///
/// Uplink pacing follows the downlink (§6.4): one `AUDIO_TX` block is sent per
/// received `AUDIO_RX` block, plus one per block detected as lost. A local
/// timer is only used while the downlink is silent (§6.4.1).
class HeraSession {
  HeraSession({
    required HeraLink link,
    required Uint8List token,
    required this.uplink,
    this.formats = const {
      HeraFormat.pcm16At8k,
      HeraFormat.pcm16At16k,
      HeraFormat.g711a,
      HeraFormat.g711u,
    },
    this.durations = const {20, 10, 5},
    this.resumeSessionId = 0,
    this.software = 'hera_client',
    this.peerTimeout = defaultPeerTimeout,
  })  : _link = link,
        _token = token;

  final HeraLink _link;
  final Uint8List _token;

  /// Source of uplink audio.
  final HeraUplinkSource uplink;

  /// Formats offered in `HELLO` (§6.2).
  final Set<HeraFormat> formats;

  /// Block durations offered in `HELLO` (§6.2). 20 ms is always added.
  final Set<int> durations;

  /// Session to resume, or 0 for a new session (§5.4).
  final int resumeSessionId;

  final String software;

  /// Time allowed for `READY` to arrive.
  static const Duration handshakeTimeout = Duration(seconds: 3);

  /// `HERA_PEER_TIMEOUT_S` (§5.5).
  static const Duration defaultPeerTimeout = Duration(seconds: 5);

  /// Silence on the control channel after which the device is considered
  /// gone and the session is closed (§5.5). The device sends `STATS` every
  /// second, so silence means the path is broken: over WiFi a lost network
  /// does not close the socket, and without this check the session would
  /// look alive indefinitely.
  final Duration peerTimeout;

  /// Time since the last control frame from the device.
  final Stopwatch _sinceHeard = Stopwatch();

  /// Downlink silence after which the uplink switches to a local timer
  /// (§6.4.1).
  static const Duration downlinkSilence = Duration(milliseconds: 60);

  /// `PING` interval on the audio channel when no audio is flowing
  /// (§4.2.1).
  static const Duration audioKeepalive = Duration(milliseconds: 500);

  /// Gaps longer than this many blocks are treated as a restart and not
  /// compensated (§6.4).
  static const int maxConcealedBlocks = 8;

  final StreamController<HeraEvent> _events =
      StreamController<HeraEvent>.broadcast();
  final StreamController<HeraAudioBlock> _downlink =
      StreamController<HeraAudioBlock>.broadcast();
  final StreamController<HeraGap> _gaps = StreamController<HeraGap>.broadcast();
  final StreamController<HeraStats> _stats =
      StreamController<HeraStats>.broadcast();

  StreamSubscription<HeraMessage>? _control;
  StreamSubscription<HeraAudioBlock>? _audio;
  Timer? _handshake;
  Timer? _keepalive;
  Timer? _freeRun;
  Timer? _reporter;

  /// Call and state events (§8).
  Stream<HeraEvent> get events => _events.stream;

  /// Validated downlink blocks. The format is given by each block.
  Stream<HeraAudioBlock> get downlink => _downlink.stream;

  /// Downlink gaps to conceal during playback (§6.5).
  Stream<HeraGap> get gaps => _gaps.stream;

  /// Device statistics, once per second (§10.1).
  Stream<HeraStats> get stats => _stats.stream;

  /// Completes when the session ends.
  Future<void> get closed => _link.closed;

  /// Current `session_id`, used to resume the session (§5.4).
  int get sessionId => _sessionId;
  int _sessionId = 0;

  /// Ephemeral key pair of this session (§11.3).
  HeraKeyPair? _keyPair;

  /// Negotiated format, from `READY` and later blocks (§6.2).
  HeraFormat? get format => _format;
  HeraFormat? _format;

  /// Negotiated block duration (§6.2).
  int get frameMs => _frameMs;
  int _frameMs = Hera.maxFrameMs;

  /// Adaptive policy for the playback buffer (§6.5). The session feeds it
  /// arrival times; the player reads the target depth from it.
  final JitterPolicy playback = JitterPolicy(
    frameMs: Hera.maxFrameMs,
    floorMs: Hera.maxFrameMs,
    ceilingMs: 200,
  );

  /// Device software version, from `HELLO` or `READY`.
  String get firmware => _firmware;
  String _firmware = '';

  /// True once `READY` has been received.
  bool get isReady => _ready;
  bool _ready = false;

  /// True while the uplink is paced by a local timer (§6.4.1).
  bool get isFreeRunning => _freeRunning;
  bool _freeRunning = false;

  /// True while the phone's audio channel is open.
  bool get isAudioOpen => _audioOpen;
  bool _audioOpen = false;

  /// Cumulative audio blocks handed to the link (§13.2).
  int get sentBlocks => _link.sentAudio;

  /// Blocks dropped locally because the link did not accept them (§6.6).
  int get droppedBlocks => _link.droppedAudio;

  int _epoch = 0;
  int _uplinkTimestamp = 0;
  int? _expectedTimestamp;

  // `CSTATS` counters (§10.2).
  final Stopwatch _uptime = Stopwatch();
  int _downlinkBlocks = 0;
  int _downlinkLost = 0;
  int _uplinkBlocks = 0;
  int _rttMs = 0;
  DateTime? _pingSentAt;

  /// Milliseconds of audio buffered by the player, reported in `CSTATS`.
  int bufferedMs = 0;

  /// Whether the uplink currently carries non-filler audio, reported in
  /// `CSTATS`.
  bool microphoneLive = false;

  /// Starts the session: generates the ephemeral key pair and sends `HELLO`
  /// (§5.3).
  Future<void> start() async {
    _uptime.start();
    _control = _link.control.listen(_onControl);
    _audio = _link.audio.listen(_onAudioBlock);

    _keyPair = await HeraKeyPair.generate();
    final formatMask = formats.fold(0, (mask, format) => mask | format.mask);
    final durationMask = durations.fold(
      Hera.mandatoryDurations,
      (mask, ms) => mask | Hera.durationBit(ms),
    );
    final flags = resumeSessionId != 0 ? 0x01 : 0x00;
    final tag = await heraHelloTag(
      token: _token,
      role: 0,
      formats: formatMask,
      durations: durationMask,
      flags: flags,
      sessionId: resumeSessionId,
      publicKey: _keyPair!.publicKey,
    );

    // Sent without waiting for the device's HELLO (§5.3).
    _link.hello(
      formats: formatMask,
      durations: durationMask,
      publicKey: _keyPair!.publicKey,
      tag: tag,
      sessionId: resumeSessionId,
      resume: resumeSessionId != 0,
      software: software,
    );

    _handshake = Timer(handshakeTimeout, () {
      if (_ready) return;
      debugPrint('HeraSession: no READY within '
          '${handshakeTimeout.inSeconds} s');
      unawaited(close());
    });

    // CSTATS once per second; also serves as keepalive (§5.5). The same tick
    // checks the peer timeout.
    _sinceHeard.start();
    _reporter = Timer.periodic(const Duration(seconds: 1), (_) {
      if (_ready && _sinceHeard.elapsed > peerTimeout) {
        debugPrint('HeraSession: nothing from the device for '
            '${_sinceHeard.elapsedMilliseconds} ms, closing (§5.5)');
        // No BYE: it would end the session without a resume window (§5.6),
        // and if the device was only unreachable for a while it would get
        // the BYE late and discard a session that could have been resumed.
        unawaited(close(sayBye: false));
        return;
      }
      _report();
    });
  }

  /// Answers the incoming call.
  void answer() {
    debugPrint('HeraSession: -> ANSWER');
    _link.sendCommand(HeraCommandId.answer);
  }

  /// Rejects the incoming call or ends the active one.
  void hangup() {
    debugPrint('HeraSession: -> HANGUP');
    _link.sendCommand(HeraCommandId.hangup);
  }

  /// Dials [number].
  void dial(String number) =>
      _link.sendCommand(HeraCommandId.dial, text: number);

  /// Sends `CMD MUTE_UL` (§9): the device sends silence to the phone instead
  /// of the uplink audio.
  void muteUplink(bool muted) =>
      _link.sendCommand(HeraCommandId.muteUplink, a1: muted ? 1 : 0);

  /// Sends `CMD SET_FORMAT` (§9). The change takes effect with the next
  /// block, whose `fmt` field identifies it.
  void requestFormat(HeraFormat format) =>
      _link.sendCommand(HeraCommandId.setFormat, a1: format.id);

  /// Closes the session. With [sayBye] a session that reached READY is
  /// ended with BYE, which discards it on the device (§5.6); without, it
  /// stays resumable.
  Future<void> close({bool sayBye = true}) async {
    _handshake?.cancel();
    _keepalive?.cancel();
    _freeRun?.cancel();
    _reporter?.cancel();
    _handshake = _keepalive = _freeRun = _reporter = null;
    _uptime.stop();
    if (_ready && sayBye) _link.bye();
    await _control?.cancel();
    await _audio?.cancel();
    _control = null;
    _audio = null;
    await _link.close();
    await _events.close();
    await _downlink.close();
    await _gaps.close();
    await _stats.close();
  }

  // -------------------------------------------------------------- control ---

  void _onControl(HeraMessage message) {
    _sinceHeard.reset();
    switch (message) {
      case HeraHello hello:
        if (!hello.isSupported) {
          debugPrint(
              'HeraSession: unsupported protocol v${hello.version}');
          unawaited(close());
          return;
        }
        // The session id is taken from the device HELLO.
        _sessionId = hello.sessionId;
        _link.sessionId = hello.sessionId;
        if (hello.software.isNotEmpty) _firmware = hello.software;
        unawaited(_completeHandshake(hello));

      case HeraEvent event:
        _onEvent(event);

      case HeraStats stats:
        if (!_stats.isClosed) _stats.add(stats);

      case HeraBye bye:
        debugPrint(
            'HeraSession: closed by the device: ${bye.description}');
        unawaited(close());

      case HeraPing ping when ping.isReply:
        final sentAt = _pingSentAt;
        if (sentAt != null) {
          _rttMs = DateTime.now().difference(sentAt).inMilliseconds;
          _pingSentAt = null;
        }

      case HeraLogLine line:
        debugPrint('[hera] ${line.text}');

      case HeraAck ack when !ack.ok:
        debugPrint('HeraSession: command '
            '0x${ack.command.toRadixString(16)} rejected by the device');

      default:
        break;
    }
  }

  /// Verifies the device's tag and derives the session keys (§11.4,
  /// §11.3).
  Future<void> _completeHandshake(HeraHello hello) async {
    final own = _keyPair;
    if (own == null) return;

    // Verify the tag before deriving any key (§11.4).
    final expected = await heraHelloTag(
      token: _token,
      role: hello.role,
      formats: hello.formats,
      durations: hello.durations,
      flags: hello.flags,
      sessionId: hello.sessionId,
      publicKey: hello.publicKey,
    );
    if (!heraTagEqual(expected, hello.tag)) {
      debugPrint('HeraSession: device HELLO tag does not verify (wrong '
          'token or on-path attacker)');
      unawaited(close());
      return;
    }

    try {
      _link.sec = await HeraSecSession.derive(
        own: own,
        peerPublicKey: hello.publicKey,
      );
    } on HeraProtocolException catch (error) {
      debugPrint('HeraSession: $error');
      unawaited(close());
      return;
    }

    // The device announces the selected duration in AUDIO_FMT; until then
    // assume the mandatory 20 ms.
    _applyDuration(Hera.maxFrameMs);
  }

  /// Applies a new block duration and resets the playback policy.
  void _applyDuration(int ms) {
    if (!Hera.isValidDuration(ms) || ms == _frameMs) return;
    _frameMs = ms;
    _link.frameMs = ms;
    playback.reset(frameMs: ms, floorMs: ms, ceilingMs: 200);
    _resetAudio();
  }

  void _onEvent(HeraEvent event) {
    // Every event is logged; they are infrequent.
    debugPrint('HeraSession: <- ${event.name} a1=${event.a1} a2=${event.a2}'
        '${event.text.isEmpty ? '' : ' ${event.text}'}');
    switch (event.id) {
      case HeraEventId.ready:
        _ready = true;
        _handshake?.cancel();
        _handshake = null;
        _format = event.format;
        if (event.text.isNotEmpty) _firmware = event.text;
        // Bind the audio flow (§4.2.1).
        _bindAudio();

      case HeraEventId.audioFormat:
        _format = event.format ?? _format;
        _applyDuration(event.a2 & 0xFF);
        _epoch = event.epoch;

      case HeraEventId.audioOpen:
        // Reset buffers and counters (§8).
        _audioOpen = true;
        _resetAudio();

      case HeraEventId.audioClose:
        _audioOpen = false;
        _stopFreeRun();
        _resetAudio();

      case HeraEventId.error when event.a2 == HeraErrorCode.resumeRefused:
        // The device continues with a new session on the same connection
        // (§5.4).
        debugPrint('HeraSession: resume refused, new session');
    }
    if (!_events.isClosed) _events.add(event);
  }

  // ---------------------------------------------------------------- audio ---

  /// Sends the `PING` that binds the audio flow and repeats it as a
  /// keepalive.
  void _bindAudio() {
    _ping();
    _keepalive?.cancel();
    _keepalive = Timer.periodic(audioKeepalive, (_) {
      // Not needed while audio blocks are flowing.
      if (_audioOpen && !_freeRunning) return;
      _ping();
    });
  }

  void _ping() {
    _pingSentAt = DateTime.now();
    _link.pingAudio();
  }

  void _onAudioBlock(HeraAudioBlock block) {
    // A new epoch means a new audio stream; reset without waiting for the
    // event, which may arrive later on the control channel (§6.3).
    if (block.epoch != _epoch) {
      _epoch = block.epoch;
      _resetAudio();
    }
    if (!_audioOpen) _audioOpen = true;
    _format = block.format;
    _downlinkBlocks += block.count;

    // Downlink is flowing: leave free-running mode and re-arm the timer.
    _stopFreeRun();
    _armFreeRun();

    final expected = _expectedTimestamp;
    var missing = 0;
    if (expected != null && block.timestamp > expected) {
      final gap = (block.timestamp - expected) ~/
          block.format.samplesPerBlock(block.frameMs);
      // Long gaps are a restart, not loss to compensate (§6.4).
      missing = gap > maxConcealedBlocks ? 0 : gap;
      _downlinkLost += gap;
    }
    _expectedTimestamp = block.timestamp +
        block.count * block.format.samplesPerBlock(block.frameMs);

    if (missing > 0 && !_gaps.isClosed) {
      _gaps.add(HeraGap(
        blocks: missing,
        format: block.format,
        frameMs: block.frameMs,
      ));
    }
    if (!_downlink.isClosed) _downlink.add(block);

    // One block per received block, plus one per lost block (§6.4).
    _sendUplink(block.count + missing, block.format);
  }

  /// Sends [blocks] uplink blocks, split into several frames if they do not
  /// fit in one payload (§6.3.1).
  void _sendUplink(int blocks, HeraFormat format) {
    final cap = format.maxBlocksPerPayload(_frameMs);
    var remaining = blocks;
    while (remaining > 0) {
      final batch = remaining > cap ? cap : remaining;
      final wanted = batch * format.bytesPerBlock(_frameMs);
      var source = uplink(batch, format);
      if (source.samples.length != wanted) {
        // Wrong length from the source: send filler of the right length
        // instead of a block the device would drop.
        debugPrint('HeraSession: uplink source returned '
            '${source.samples.length} bytes instead of $wanted');
        source = HeraUplink(_silence(wanted, format), filler: true);
      }
      _link.sendAudioBlock(
        format: format,
        frameMs: _frameMs,
        samples: source.samples,
        timestamp: _uplinkTimestamp,
        epoch: _epoch,
        filler: source.filler,
      );
      _uplinkTimestamp += batch * format.samplesPerBlock(_frameMs);
      _uplinkBlocks += batch;
      remaining -= batch;
    }
  }

  /// Silence in [format].
  static Uint8List _silence(int bytes, HeraFormat format) =>
      Uint8List(bytes)..fillRange(0, bytes, format.silenceByte);

  /// Arms the switch to free-running pacing if the downlink stops
  /// (§6.4.1).
  void _armFreeRun() {
    _freeRun?.cancel();
    if (!_audioOpen) return;
    _freeRun = Timer(downlinkSilence, _startFreeRun);
  }

  void _startFreeRun() {
    if (_freeRunning || !_audioOpen) return;
    _freeRunning = true;
    debugPrint('HeraSession: downlink silent, switching to free-running '
        'pacing');
    _freeRun = Timer.periodic(Duration(milliseconds: _frameMs), (_) {
      final format = _format;
      if (format == null || !_audioOpen) return;
      _sendUplink(1, format);
    });
  }

  void _stopFreeRun() {
    if (!_freeRunning) return;
    _freeRunning = false;
    _freeRun?.cancel();
    _freeRun = null;
  }

  void _resetAudio() {
    _uplinkTimestamp = 0;
    _expectedTimestamp = null;
  }

  // ----------------------------------------------------------- statistics ---

  void _report() {
    if (!_ready) return;
    _link.sendControl(
      HeraFrameType.clientStats,
      buildHeraClientStats(
        uptimeMs: _uptime.elapsedMilliseconds,
        downlinkBlocks: _downlinkBlocks,
        downlinkLost: _downlinkLost,
        uplinkBlocks: _uplinkBlocks,
        bufferedMs: bufferedMs,
        rttMs: _rttMs,
        playing: bufferedMs > 0,
        microphoneLive: microphoneLive,
        freeRunning: _freeRunning,
      ),
    );
  }
}
