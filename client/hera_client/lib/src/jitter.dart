/// Adaptive target depth for the playback buffer (`PROTOCOL.md` §6.5).
///
/// The target tracks measured interarrival jitter between a floor and a
/// ceiling. The policy is asymmetric:
///
/// - grow as soon as the depth is one block below target; shrink only when it
///   is more than two blocks above;
/// - on underrun, raise the target by two blocks immediately and hold it for
///   [JitterPolicy.holddown];
/// - shrink only by discarding a filler block (§6.3, `flags` bit 0).
///
/// This is the same policy, with the same constants, as the device's
/// `jitter.c`.
library;

enum JitterAction {
  hold,

  /// Insert one filler block.
  grow,

  /// Discard one filler block.
  shrink,
}

class JitterPolicy {
  JitterPolicy({
    required int frameMs,
    required int floorMs,
    required int ceilingMs,
  }) {
    reset(frameMs: frameMs, floorMs: floorMs, ceilingMs: ceilingMs);
  }

  /// Hold-down period after an underrun.
  static const Duration holddown = Duration(seconds: 5);

  /// EWMA gain as a shift: `J += (|D| - J) / 16`, as in RFC 3550 §6.4.1.
  /// J is kept in units of 1/16 ms (RFC 3550 Appendix A.8); with whole
  /// milliseconds the update would round to zero for deviations less than
  /// 16 ms above J.
  static const int jitterShift = 4;

  late int _frameMs;
  late int _floorMs;
  late int _ceilingMs;
  int _targetMs = 0;
  int _jitterX16 = 0;
  DateTime? _lastArrival;
  DateTime? _holddownUntil;

  /// Minimum target while the hold-down is active.
  int _underrunFloorMs = 0;

  int get frameMs => _frameMs;
  int get targetMs => _targetMs;
  int get jitterMs => (_jitterX16 + 8) >> jitterShift;

  void reset(
      {required int frameMs, required int floorMs, required int ceilingMs}) {
    _frameMs = frameMs > 0 ? frameMs : 20;
    // The floor cannot be less than one block.
    _floorMs = floorMs < _frameMs ? _frameMs : floorMs;
    _ceilingMs = ceilingMs < _floorMs ? _floorMs : ceilingMs;
    // Start at the floor; the estimate raises the target as needed.
    _targetMs = _floorMs;
    _jitterX16 = 0;
    _lastArrival = null;
    _holddownUntil = null;
    _underrunFloorMs = 0;
  }

  /// Records a block arrival and updates the jitter estimate.
  void onArrival(DateTime now) {
    final last = _lastArrival;
    if (last != null) {
      final gap = now.difference(last).inMilliseconds;
      // Absolute deviation from the nominal interval.
      var deviation = (gap - _frameMs).abs();
      // Clamp, so that a stream restart does not inflate the estimate.
      final cap = 10 * _frameMs;
      if (deviation > cap) deviation = cap;
      _jitterX16 += deviation - ((_jitterX16 + 8) >> jitterShift);
      if (_jitterX16 < 0) _jitterX16 = 0;
      _recompute(now);
    }
    _lastArrival = now;
  }

  /// Records an underrun: raises the target and starts the hold-down
  /// (§6.5).
  void onUnderrun(DateTime now) {
    _targetMs = _clamp(_targetMs + 2 * _frameMs);
    _underrunFloorMs = _targetMs;
    _holddownUntil = now.add(holddown);
  }

  /// Decides whether to grow, shrink or hold at the current depth.
  JitterAction decide(int depthMs, DateTime now) {
    // Grow when at least one block below target.
    if (depthMs + _frameMs <= _targetMs) return JitterAction.grow;

    final until = _holddownUntil;
    if (until != null && now.isBefore(until)) return JitterAction.hold;

    // Shrink only when more than two blocks above target.
    if (depthMs > _targetMs + 2 * _frameMs) return JitterAction.shrink;

    return JitterAction.hold;
  }

  /// `target = 3 * J + frameMs`, clamped to [floor, ceiling] (§6.5).
  ///
  /// During the hold-down the target may rise but not fall below the value
  /// set by the last underrun.
  void _recompute(DateTime now) {
    var wanted = (3 * _jitterX16 + 8) ~/ 16 + _frameMs;
    final until = _holddownUntil;
    if (until != null && now.isBefore(until) && wanted < _underrunFloorMs) {
      wanted = _underrunFloorMs;
    }
    _targetMs = _clamp(wanted);
  }

  int _clamp(int value) =>
      value < _floorMs ? _floorMs : (value > _ceilingMs ? _ceilingMs : value);
}
