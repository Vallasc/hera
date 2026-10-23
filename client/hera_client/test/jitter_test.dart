import 'package:flutter_test/flutter_test.dart';
import 'package:hera_client/src/jitter.dart';

/// [n] arrivals at exactly `frameMs` intervals.
DateTime _steady(JitterPolicy p, DateTime t, int n) {
  for (var i = 0; i < n; i++) {
    t = t.add(Duration(milliseconds: p.frameMs));
    p.onArrival(t);
  }
  return t;
}

/// [n] arrivals alternating [deviationMs] early and late.
DateTime _jittery(JitterPolicy p, DateTime t, int n, int deviationMs) {
  for (var i = 0; i < n; i++) {
    final step = i.isEven
        ? (p.frameMs - deviationMs).clamp(1, 1000)
        : p.frameMs + deviationMs;
    t = t.add(Duration(milliseconds: step));
    p.onArrival(t);
  }
  return t;
}

void main() {
  final t0 = DateTime(2026, 1, 1);

  group('target', () {
    test('starts at the floor', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      expect(p.targetMs, 20);
    });

    test('regular arrivals keep it low', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      _steady(p, t0, 200);
      expect(p.targetMs, lessThanOrEqualTo(40));
    });

    test('jitter raises it, regular arrivals lower it again', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      final t = _jittery(p, t0, 300, 30);
      final high = p.targetMs;
      expect(high, greaterThan(60));
      expect(high, lessThanOrEqualTo(200));

      _steady(p, t, 500);
      expect(p.targetMs, lessThan(high));
    });

    test('small jitter is measured', () {
      // J is kept in 1/16 ms, so deviations of a few ms are tracked.
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      _jittery(p, t0, 400, 6);
      expect(p.jitterMs, inInclusiveRange(4, 8));
      expect(p.targetMs, greaterThanOrEqualTo(32));
    });

    test('ceiling is not exceeded', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 60);
      _jittery(p, t0, 300, 100);
      expect(p.targetMs, 60);
    });

    test('floor is at least one block', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 5, ceilingMs: 200);
      expect(p.targetMs, 20);
    });

    test('a gap of seconds is clamped', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 400);
      var t = _steady(p, t0, 100);
      final calm = p.targetMs;

      t = t.add(const Duration(seconds: 3));
      p.onArrival(t);
      expect(p.targetMs, lessThanOrEqualTo(calm + 120));

      _steady(p, t, 200);
      expect(p.targetMs, lessThanOrEqualTo(calm + 20), reason: 'recovers');
    });
  });

  group('underrun', () {
    test('raises the target by two blocks immediately', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      _steady(p, t0, 100);
      final before = p.targetMs;
      p.onUnderrun(t0);
      expect(p.targetMs, greaterThanOrEqualTo(before + 40));
    });

    test('the increase survives later arrivals', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      var t = _steady(p, t0, 100);
      p.onUnderrun(t);
      final raised = p.targetMs;

      t = _steady(p, t, 50);
      expect(p.targetMs, raised, reason: 'no decrease during hold-down');

      // After the hold-down the estimate takes over again.
      t = t.add(JitterPolicy.holddown + const Duration(seconds: 1));
      _steady(p, t, 50);
      expect(p.targetMs, lessThan(raised));
    });

    test('blocks shrinking during the hold-down', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      p.onUnderrun(t0);
      final deep = p.targetMs + 200;
      expect(p.decide(deep, t0), JitterAction.hold);
      expect(
          p.decide(
              deep, t0.add(JitterPolicy.holddown - const Duration(seconds: 1))),
          JitterAction.hold);
      expect(
          p.decide(
              deep, t0.add(JitterPolicy.holddown + const Duration(seconds: 1))),
          JitterAction.shrink);
    });
  });

  group('decide', () {
    test('asymmetric thresholds', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 40, ceilingMs: 200);
      final later = t0.add(const Duration(minutes: 1));
      expect(p.decide(0, later), JitterAction.grow, reason: 'empty');
      expect(p.decide(20, later), JitterAction.grow, reason: 'one below');
      expect(p.decide(40, later), JitterAction.hold, reason: 'at target');
      expect(p.decide(60, later), JitterAction.hold, reason: 'one above');
      expect(p.decide(80, later), JitterAction.hold, reason: 'two above');
      expect(p.decide(100, later), JitterAction.shrink,
          reason: 'more than two above');
    });

    test('same thresholds with 5 ms blocks', () {
      final p = JitterPolicy(frameMs: 5, floorMs: 5, ceilingMs: 100);
      final later = t0.add(const Duration(minutes: 1));
      expect(p.decide(0, later), JitterAction.grow);
      expect(p.decide(10, later), JitterAction.hold);
      expect(p.decide(20, later), JitterAction.shrink);
    });
  });

  group('reset', () {
    test('changing the duration resets the state', () {
      final p = JitterPolicy(frameMs: 20, floorMs: 20, ceilingMs: 200);
      _jittery(p, t0, 300, 40);
      expect(p.targetMs, greaterThan(60));

      p.reset(frameMs: 5, floorMs: 5, ceilingMs: 100);
      expect(p.frameMs, 5);
      expect(p.targetMs, 5);
      expect(p.jitterMs, 0);
    });
  });
}
