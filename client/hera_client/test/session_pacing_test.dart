import 'dart:async';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:hera_client/hera_client.dart';

/// In-memory link: the test feeds downlink blocks and counts uplink frames.
class _MemoryLink extends HeraLink {
  final StreamController<HeraMessage> _control = StreamController();
  final StreamController<HeraAudioBlock> _audio = StreamController();
  final Completer<void> _closed = Completer();
  int uplink = 0;

  @override
  Stream<HeraMessage> get control => _control.stream;
  @override
  Stream<HeraAudioBlock> get audio => _audio.stream;
  @override
  Future<void> get closed => _closed.future;
  @override
  int sessionId = 0;
  @override
  int frameMs = 20;
  @override
  HeraSecSession? sec;
  @override
  int get droppedAudio => 0;
  @override
  int get sentAudio => uplink;

  @override
  void sendControl(int type, [Uint8List? payload]) {}

  @override
  void sendAudio(int type, [Uint8List? payload]) {
    if (type == HeraFrameType.audioTx) uplink++;
  }

  @override
  Future<void> close() async {
    if (!_closed.isCompleted) _closed.complete();
    await _control.close();
    await _audio.close();
  }

  void event(int id) =>
      _control.add(HeraEvent(id: id, a1: 0, a2: 0, text: ''));

  int _ts = 0;

  /// The next downlink block, contiguous with the previous one unless [skip]
  /// blocks are jumped.
  void downlink({int skip = 0}) {
    const format = HeraFormat.pcm16At8k;
    final samples = format.samplesPerBlock(20);
    _ts += skip * samples;
    _audio.add(HeraAudioBlock(
      format: format,
      frameMs: 20,
      count: 1,
      epoch: 0,
      filler: false,
      timestamp: _ts,
      samples: Uint8List(format.bytesPerBlock(20)),
    ));
    _ts += samples;
  }
}

Future<void> _settle() => Future<void>.delayed(Duration.zero);

void main() {
  late _MemoryLink link;
  late HeraSession session;

  setUp(() async {
    link = _MemoryLink();
    session = HeraSession(
      link: link,
      token: Uint8List(Hera.tokenSize),
      uplink: (blocks, format) => HeraUplink(
          Uint8List(blocks * format.bytesPerBlock(20)),
          filler: true),
    );
    await session.start();
  });

  tearDown(() => session.close(sayBye: false));

  test('one uplink block per downlink block', () async {
    for (var i = 0; i < 10; i++) {
      link.downlink();
    }
    await _settle();
    expect(link.uplink, 10);
  });

  test('blocks queued during a stall do not send the uplink twice', () async {
    link.downlink();
    await _settle();
    expect(link.uplink, 1);

    // Nothing read for 300 ms: the local timer takes over after 60 ms.
    await Future<void>.delayed(const Duration(milliseconds: 300));
    expect(session.isFreeRunning, isTrue);
    final byTimer = link.uplink - 1;
    expect(byTimer, greaterThan(0));

    // The 15 blocks the device sent meanwhile arrive together.
    for (var i = 0; i < 15; i++) {
      link.downlink();
    }
    await _settle();
    expect(session.isFreeRunning, isFalse);
    // 16 blocks of downlink time, 16 blocks of uplink: the timer's blocks
    // count toward the queued ones.
    expect(link.uplink, 16);
  });

  test('a block arriving after AUDIO_CLOSE does not reopen the audio',
      () async {
    link.downlink();
    await _settle();
    link.event(HeraEventId.audioClose);
    await _settle();
    // Read late, after the control event.
    link.downlink();
    await Future<void>.delayed(const Duration(milliseconds: 200));
    expect(session.isAudioOpen, isFalse);
    expect(session.isFreeRunning, isFalse);
    expect(link.uplink, 1, reason: 'nothing sent between calls');
  });

  test('a downlink that really stopped owes nothing on return', () async {
    link.downlink();
    await _settle();
    await Future<void>.delayed(const Duration(milliseconds: 300));
    final before = link.uplink;

    // Resumes much later: timestamps jump past the concealment limit.
    link.downlink(skip: 50);
    link.downlink();
    await _settle();
    expect(link.uplink, before + 2);
  });
}
