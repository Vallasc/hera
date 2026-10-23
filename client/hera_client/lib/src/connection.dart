import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';

import 'channel.dart';
import 'protocol.dart';
import 'serial_link.dart';
import 'serial_port.dart';
import 'session.dart';
import 'status.dart';
import 'transport.dart';
import 'wifi_link.dart';

/// Runs [open], which creates sockets, so that those sockets reach the
/// device.
///
/// Needed where the device's network is not the default route, e.g. an
/// Android local-only WiFi connection: sockets must then be bound to that
/// network when they are created.
typedef HeraSocketScope = Future<R> Function<R>(Future<R> Function() open);

/// Address, ports and token of a device.
@immutable
class HeraEndpoint {
  const HeraEndpoint({
    required this.address,
    required this.token,
    this.controlPort = Hera.defaultControlPort,
    this.audioPort = Hera.defaultAudioPort,
    this.socketScope,
  });

  final InternetAddress address;

  /// Session token read from the `SESSION` characteristic (`PROTOCOL.md`
  /// §3.4).
  final Uint8List token;

  final int controlPort;
  final int audioPort;

  /// Wraps socket creation; null if the device is on the default route.
  final HeraSocketScope? socketScope;

  @override
  String toString() => '${address.address}:$controlPort';
}

/// Returns the endpoint to connect to.
///
/// Called before every connection attempt, since the endpoint may change
/// (network mode, DHCP lease, network re-join). The implementation decides
/// whether to use BLE (§3) or stored values.
typedef HeraProvisioner = Future<HeraEndpoint> Function();

/// Keeps a connection to the device open and reconnects when it drops.
///
/// After a drop, a session that had reached `READY` is resumed first (§5.4);
/// the device keeps the call up during its resume window. If the resume
/// fails, a new session is started. Failed attempts back off exponentially.
class HeraConnection {
  HeraConnection({
    required this.transport,
    required this.uplink,
    required this.onStatus,
    this.provisioner,
    this.preferredPort,
    this.formats = const {
      HeraFormat.pcm16At8k,
      HeraFormat.pcm16At16k,
      HeraFormat.g711a,
      HeraFormat.g711u,
    },
    this.durations = const {20, 10, 5},
    this.retryDelay = const Duration(seconds: 3),
    this.maxRetryDelay = const Duration(seconds: 10),
    this.peerTimeout = HeraSession.defaultPeerTimeout,
    SerialDriver? serialDriver,
  }) : _serialDriver = serialDriver ??
            (transport == HeraTransport.usb
                ? SerialDriver.forPlatform()
                : null);

  final HeraTransport transport;

  /// Source of uplink audio (§6.4).
  final HeraUplinkSource uplink;

  final ValueChanged<HeraStatus> onStatus;

  /// Required for WiFi transports; unused for USB.
  final HeraProvisioner? provisioner;

  /// Preferred serial port. On Android the name changes on every re-attach;
  /// if it is not found, the first recognised bridge is used.
  final String? preferredPort;

  /// Formats offered in `HELLO` (§6.2).
  final Set<HeraFormat> formats;

  /// Block durations offered in `HELLO` (§6.2). Applications whose audio
  /// path cannot handle short blocks should restrict this set.
  final Set<int> durations;

  final Duration retryDelay;

  /// Upper bound of the backoff. Kept short: a device that is booting should
  /// be found soon after it comes up, not at the end of a long wait.
  final Duration maxRetryDelay;

  /// See [HeraSession.peerTimeout].
  final Duration peerTimeout;
  final SerialDriver? _serialDriver;

  final StreamController<HeraSession> _sessions =
      StreamController<HeraSession>.broadcast();

  /// Every newly established session, including resumed ones.
  Stream<HeraSession> get sessions => _sessions.stream;

  HeraSession? _session;
  StreamSubscription<void>? _changes;
  Timer? _retry;
  bool _running = false;
  bool _connecting = false;

  /// Session to resume after a recent drop, or 0.
  int _resumeId = 0;
  Uint8List? _token;

  /// Consecutive failures; the retry delay doubles from [retryDelay] up to
  /// [maxRetryDelay].
  int _failures = 0;

  HeraStatus _status = const HeraStatus();
  HeraStatus get status => _status;

  HeraSession? get session => _session;

  Future<void> start() async {
    if (_running) return;
    _running = true;
    _publish(_status.copyWith(transport: transport));

    if (transport == HeraTransport.usb) {
      final driver = _serialDriver;
      if (driver == null) {
        _publish(_status.copyWith(
          phase: LinkPhase.unsupported,
          error: SerialDriver.unsupportedReason,
        ));
        return;
      }
      _changes = driver.changes.listen((_) {
        if (_session == null) unawaited(_connect());
      });
    } else if (provisioner == null) {
      _publish(_status.copyWith(
        phase: LinkPhase.unsupported,
        error: 'no provisioner to obtain address and credentials',
      ));
      return;
    }
    await _connect();
  }

  Future<void> stop() async {
    _running = false;
    _retry?.cancel();
    _retry = null;
    await _changes?.cancel();
    _changes = null;
    await _teardown();
    _serialDriver?.dispose();
    await _sessions.close();
  }

  /// Answers the incoming call, if a session is open.
  void answer() => _session?.answer();

  /// Rejects the incoming call or ends the active one.
  void hangup() => _session?.hangup();

  Future<void> _connect() async {
    if (!_running || _connecting || _session != null) return;
    _connecting = true;
    try {
      final HeraLink link;
      final String endpoint;

      if (transport == HeraTransport.usb) {
        final device = await _serialDriver!.resolve(preferredPort);
        if (device == null) {
          // No device attached yet; retry later.
          _publish(_status.copyWith(
            phase: LinkPhase.searching,
            clearError: true,
          ));
          _scheduleRetry();
          return;
        }
        _publish(_status.copyWith(
          phase: LinkPhase.connecting,
          endpoint: device.label,
          clearError: true,
        ));
        final serial = SerialHeraLink(
          await _serialDriver.open(device, baudRate: Hera.serialBaudRate),
        )..start();
        link = serial;
        endpoint = device.label;
        // On USB the token is not verified (§11.4); an all-zero token is
        // used for the HELLO tag.
        _token = Uint8List(Hera.tokenSize);
      } else {
        _publish(_status.copyWith(
          phase: _resumeId == 0 ? LinkPhase.provisioning : LinkPhase.resuming,
          clearError: true,
        ));
        final target = await provisioner!();
        _token = target.token;
        _publish(_status.copyWith(
          phase: _resumeId == 0 ? LinkPhase.connecting : LinkPhase.resuming,
          endpoint: '$target',
        ));
        Future<WifiHeraLink> open() => WifiHeraLink.connect(
              address: target.address,
              controlPort: target.controlPort,
              audioPort: target.audioPort,
            );
        final scope = target.socketScope;
        link = scope == null ? await open() : await scope(open);
        endpoint = '$target';
      }

      final session = HeraSession(
        link: link,
        token: _token!,
        uplink: uplink,
        formats: formats,
        durations: durations,
        resumeSessionId: _resumeId,
        peerTimeout: peerTimeout,
      );
      _session = session;
      // start() generates the key pair and sends HELLO.
      await session.start();
      _failures = 0;


      debugPrint('HeraConnection: connected to $endpoint'
          '${_resumeId == 0 ? '' : ' (resuming $_resumeId)'}');
      _publish(_status.copyWith(
        phase: LinkPhase.connected,
        endpoint: endpoint,
        clearError: true,
      ));
      if (!_sessions.isClosed) _sessions.add(session);
      unawaited(session.closed.then((_) => _onDropped(session)));
    } catch (error) {
      debugPrint('HeraConnection: $error');
      _publish(_status.copyWith(
        phase: LinkPhase.searching,
        error: '$error',
      ));
      _scheduleRetry(afterFailure: true);
    } finally {
      _connecting = false;
    }
  }

  Future<void> _onDropped(HeraSession dropped) async {
    if (!identical(_session, dropped)) return;
    // Try to resume on the next attempt (§5.4), but only if the session had
    // reached READY. A session id from a failed handshake was never
    // established, and resuming it would loop without backoff.
    _resumeId = dropped.isReady ? dropped.sessionId : 0;
    await _teardown();
    if (!_running) return;
    _publish(_status.copyWith(
      phase: _resumeId == 0 ? LinkPhase.searching : LinkPhase.resuming,
      call: CallPhase.idle,
      clearCaller: true,
      clearFormat: true,
    ));
    // A session that never became ready counts as a failure for backoff.
    _scheduleRetry(afterFailure: _resumeId == 0);
  }

  Future<void> _teardown() async {
    final session = _session;
    _session = null;
    await session?.close();
  }

  Duration _backoff() {
    final delay = retryDelay * (1 << _failures.clamp(0, 4));
    return delay > maxRetryDelay ? maxRetryDelay : delay;
  }

  void _scheduleRetry({bool afterFailure = false}) {
    _retry?.cancel();
    if (!_running) return;
    if (afterFailure) _failures++;

    // Resume attempts are retried quickly, to stay within the device's
    // resume window (§5.4).
    final delay = _resumeId != 0 && !afterFailure
        ? const Duration(milliseconds: 300)
        : _backoff();
    _retry = Timer(delay, () {
      // Give up resuming after repeated failures.
      if (_resumeId != 0 && _failures > 2) _resumeId = 0;
      unawaited(_connect());
    });
  }

  void _publish(HeraStatus status) {
    _status = status;
    onStatus(status);
  }
}
