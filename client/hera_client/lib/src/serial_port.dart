import 'dart:async';
import 'dart:io';
import 'dart:typed_data' show BytesBuilder;

import 'package:flutter/foundation.dart';

import 'protocol.dart';
import 'serial_port_android.dart';
import 'serial_port_desktop.dart';

/// A serial port known to the system, not yet opened.
@immutable
class SerialDevice {
  const SerialDevice({
    required this.id,
    required this.label,
    this.vendorId,
    this.productId,
  });

  /// Identifier used to open the port: the port name on desktop, the USB
  /// device name on Android.
  final String id;

  /// Display name.
  final String label;

  final int? vendorId;
  final int? productId;

  /// USB vendor ids of common USB-UART bridges: Silicon Labs (CP210x),
  /// QinHeng (CH340/CH9102), FTDI, Prolific, and Espressif for boards with
  /// native USB.
  static const Set<int> knownBridgeVendors = {
    0x10C4,
    0x1A86,
    0x0403,
    0x067B,
    0x303A,
  };

  /// True if [label] or [id] contains [text], ignoring case.
  bool mentions(String text) =>
      label.toLowerCase().contains(text.toLowerCase()) ||
      id.toLowerCase().contains(text.toLowerCase());

  @override
  bool operator ==(Object other) => other is SerialDevice && other.id == id;

  @override
  int get hashCode => id.hashCode;
}

/// An open serial connection.
abstract class SerialConnection {
  /// Received bytes. Closes or emits an error when the device is
  /// detached.
  Stream<Uint8List> get incoming;

  /// Writes [bytes]; completes when they have been handed to the driver, so
  /// that callers can avoid queueing stale audio (§4.4).
  Future<void> write(Uint8List bytes);

  Future<void> close();
}

/// Merges writes issued while a previous write is in progress.
///
/// Useful where the cost is per write rather than per byte. No timer is
/// involved: when no write is in progress, data is written immediately.
class CoalescingWriter {
  CoalescingWriter(this._write);

  final Future<void> Function(Uint8List bytes) _write;
  final BytesBuilder _pending = BytesBuilder(copy: false);
  Future<void>? _flushing;

  void add(Uint8List bytes) {
    _pending.add(bytes);
    _flushing ??= _flush();
  }

  /// Completes when nothing is pending.
  Future<void> done() => _flushing ?? Future<void>.value();

  Future<void> _flush() async {
    try {
      while (_pending.isNotEmpty) {
        await _write(_pending.takeBytes());
      }
    } catch (error) {
      // The reader reports the closed link; discard pending bytes.
      debugPrint('CoalescingWriter: $error');
      _pending.clear();
    } finally {
      _flushing = null;
    }
  }
}

/// Error opening or using a serial port. [message] is suitable for
/// display.
class SerialException implements Exception {
  const SerialException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Serial port access for the current platform.
abstract class SerialDriver {
  /// Returns the driver for the current platform, or null if serial ports
  /// are not available (iOS).
  ///
  /// On Android the port is a USB host device and requires a user permission;
  /// on desktop platforms it is a system port opened through libserialport.
  static SerialDriver? forPlatform() {
    if (Platform.isAndroid) return AndroidSerialDriver();
    if (Platform.isMacOS || Platform.isLinux || Platform.isWindows) {
      return DesktopSerialDriver();
    }
    return null;
  }

  /// Reason why the serial transport is unavailable, for display. Applies to
  /// the serial transport only.
  static String get unsupportedReason => switch (Platform.operatingSystem) {
        'ios' => 'USB serial is not available to apps on iOS; '
            'use one of the WiFi modes.',
        final other => 'Serial ports are not available on $other.',
      };

  /// True if [device] looks like a HERA board (e.g. a known USB-UART bridge
  /// vendor). Used to pick a port automatically.
  bool looksLikeBoard(SerialDevice device);

  Future<List<SerialDevice>> devices();

  Future<SerialConnection> open(
    SerialDevice device, {
    int baudRate = Hera.serialBaudRate,
  });

  /// Ensures the app may open [device], requesting permission if needed.
  ///
  /// On Android the permission dialog requires a foreground Activity, while a
  /// background service may need to reopen the port later; call this from the
  /// UI beforehand. Platforms without permissions return true without opening
  /// the port.
  Future<bool> ensurePermission(SerialDevice device) async => true;

  /// Emits when the device list changes, where the platform reports it.
  /// Elsewhere it never emits and callers rely on periodic retries.
  Stream<void> get changes => const Stream<void>.empty();

  /// True if a single listed device can be assumed to be the board even if
  /// unrecognised. True on Android, where only attached USB devices are
  /// listed; false on desktop, where virtual ports are listed too.
  bool get acceptsSingleDevice => false;

  /// Selects the port to open: [preferredId] if present, otherwise the first
  /// recognised bridge (Android renames devices on every re-attach). Returns
  /// null rather than opening an arbitrary port.
  Future<SerialDevice?> resolve(String? preferredId) async {
    final devices = await this.devices();
    if (preferredId != null && preferredId.isNotEmpty) {
      for (final device in devices) {
        if (device.id == preferredId) return device;
      }
    }
    for (final device in devices) {
      if (looksLikeBoard(device)) return device;
    }
    if (acceptsSingleDevice && devices.length == 1) return devices.single;
    return null;
  }

  void dispose() {}
}
