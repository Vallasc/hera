// Interoperability with the device's own link code (device/tests/link_server.c).
//
// Skipped unless HERA_INTEROP is set:
//
//   podman run -d --name hera-link -p 7420:7420 -p 7421:7421/udp \
//       <image with tests/link_server> ./tests/link_server
//   HERA_INTEROP=127.0.0.1 HERA_INTEROP_CONTAINER=hera-link flutter test \
//       test/interop_test.dart
//
// With HERA_INTEROP_CONTAINER the device is also frozen (`podman pause`),
// which looks to the client like a device switched off: no FIN, no reply.

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:hera_client/hera_client.dart';

Future<bool> _until(bool Function() test, Duration timeout) async {
  final deadline = DateTime.now().add(timeout);
  while (!test()) {
    if (DateTime.now().isAfter(deadline)) return false;
    await Future<void>.delayed(const Duration(milliseconds: 50));
  }
  return true;
}

void main() {
  final host = Platform.environment['HERA_INTEROP'];
  final container = Platform.environment['HERA_INTEROP_CONTAINER'];

  test('the client works with the device link', () async {
    final statuses = <HeraStatus>[];
    final events = <HeraEvent>[];
    var stats = 0;
    var protectedLink = false;
    var downlink = 0;
    var uplinkCalls = 0;
    HeraConnection? connection;

    connection = HeraConnection(
      transport: HeraTransport.accessPoint,
      provisioner: () async => HeraEndpoint(
        address: InternetAddress(host!),
        token: Uint8List.fromList(List.generate(16, (i) => i)),
      ),
      uplink: (blocks, format) {
        uplinkCalls++;
        final frameMs = connection?.session?.frameMs ?? Hera.maxFrameMs;
        final bytes = blocks * format.bytesPerBlock(frameMs);
        return HeraUplink(
            Uint8List(bytes)..fillRange(0, bytes, format.silenceByte),
            filler: true);
      },
      onStatus: statuses.add,
      retryDelay: const Duration(milliseconds: 300),
      maxRetryDelay: const Duration(seconds: 1),
    );
    connection.sessions.listen((session) {
      session.events.listen(events.add);
      session.stats.listen((s) {
        stats++;
        protectedLink = s.isLinkProtected;
      });
      session.downlink.listen((_) => downlink++);
    });

    await connection.start();
    expect(await _until(() => connection!.status.isConnected,
            const Duration(seconds: 10)),
        isTrue, reason: 'handshake with the device');

    // The snapshot sent on session up (§8.1).
    expect(
        await _until(
            () => events.any((e) => e.id == HeraEventId.audioOpen),
            const Duration(seconds: 3)),
        isTrue,
        reason: 'snapshot events');
    expect(events.map((e) => e.id),
        containsAll([HeraEventId.slcUp, HeraEventId.service, HeraEventId.call]));

    // Eight seconds: STATS keep the client's peer timeout (5 s) satisfied,
    // and audio flows both ways.
    final statsBefore = stats;
    await Future<void>.delayed(const Duration(seconds: 8));
    expect(connection.status.isConnected, isTrue,
        reason: 'still connected after 8 s');
    expect(stats - statsBefore, greaterThanOrEqualTo(6), reason: 'STATS');
    expect(downlink, greaterThan(200), reason: 'downlink audio blocks');
    expect(uplinkCalls, greaterThan(100), reason: 'uplink paced by downlink');
    expect(protectedLink, isTrue, reason: 'STATS flags parsed');

    // A command and its effect.
    events.clear();
    connection.session!.answer();
    expect(
        await _until(
            () => events.any((e) => e.id == HeraEventId.call && e.a1 == 1),
            const Duration(seconds: 3)),
        isTrue,
        reason: 'CALL 1 after ANSWER');

    if (container == null) {
      await connection.stop();
      return;
    }

    // Device frozen: nothing more arrives and nothing is closed.
    await Process.run('podman', ['pause', container]);
    final frozenAt = DateTime.now();
    expect(
        await _until(() => !connection!.status.isConnected,
            const Duration(seconds: 9)),
        isTrue,
        reason: 'device gone noticed');
    final noticed = DateTime.now().difference(frozenAt);
    // ignore: avoid_print
    print('device gone noticed after ${noticed.inMilliseconds} ms');

    // Device back: the client reconnects by itself.
    await Process.run('podman', ['unpause', container]);
    final backAt = DateTime.now();
    expect(
        await _until(() => connection!.status.isConnected,
            const Duration(seconds: 20)),
        isTrue,
        reason: 'reconnected');
    // ignore: avoid_print
    print('reconnected after '
        '${DateTime.now().difference(backAt).inMilliseconds} ms');
    final downlinkBefore = downlink;
    await Future<void>.delayed(const Duration(seconds: 2));
    expect(downlink, greaterThan(downlinkBefore + 50),
        reason: 'audio flows after reconnection');

    await connection.stop();
  },
      skip: host == null ? 'set HERA_INTEROP to run' : false,
      timeout: const Timeout(Duration(minutes: 2)));
}
