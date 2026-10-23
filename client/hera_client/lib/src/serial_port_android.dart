import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:usb_serial/usb_serial.dart';

import 'protocol.dart';
import 'serial_port.dart';

/// Serial driver for Android: USB host mode with a USB-UART bridge.
///
/// Opening a port shows the system permission dialog. If the user selects
/// "always use for this device", the permission persists, which lets a
/// background service reopen the port after a reboot or re-attach.
class AndroidSerialDriver extends SerialDriver {
  @override
  Future<List<SerialDevice>> devices() async {
    final devices = await UsbSerial.listDevices();
    return [
      for (final device in devices)
        SerialDevice(
          id: device.deviceName,
          label: _label(device),
          vendorId: device.vid,
          productId: device.pid,
        ),
    ];
  }

  @override
  Future<SerialConnection> open(
    SerialDevice device, {
    int baudRate = Hera.serialBaudRate,
  }) async {
    final target = (await UsbSerial.listDevices()).firstWhere(
      (candidate) => candidate.deviceName == device.id,
      orElse: () => throw SerialException('${device.label} is not attached.'),
    );

    final UsbPort? port;
    try {
      // Shows the permission dialog if permission was not granted yet.
      port = await target.create();
    } on PlatformException catch (error) {
      throw SerialException(
        'Open refused: ${error.message ?? error.code}',
      );
    }
    if (port == null) {
      throw const SerialException(
        'USB permission denied, or the bridge is not supported.',
      );
    }

    if (!await port.open()) {
      throw SerialException('Cannot open ${device.label}.');
    }
    // On many boards DTR and RTS drive reset and boot mode; keep them
    // deasserted.
    await port.setDTR(false);
    await port.setRTS(false);
    await port.setPortParameters(
      baudRate,
      UsbPort.DATABITS_8,
      UsbPort.STOPBITS_1,
      UsbPort.PARITY_NONE,
    );

    final incoming = port.inputStream;
    if (incoming == null) {
      await port.close();
      throw const SerialException('The port has no input stream.');
    }
    return _AndroidConnection(port, incoming);
  }

  /// Permission can only be obtained by opening the port. The port is closed
  /// again immediately; the permission remains valid until the device is
  /// detached.
  @override
  Future<bool> ensurePermission(SerialDevice device) async {
    try {
      final connection = await open(device);
      await connection.close();
      return true;
    } on SerialException catch (error) {
      debugPrint('AndroidSerialDriver: permission not granted, $error');
      return false;
    }
  }

  @override
  Stream<void> get changes =>
      UsbSerial.usbEventStream?.map((_) {}) ?? const Stream<void>.empty();

  /// The USB device list only contains attached devices.
  @override
  bool get acceptsSingleDevice => true;

  @override
  bool looksLikeBoard(SerialDevice device) =>
      SerialDevice.knownBridgeVendors.contains(device.vendorId);

  static String _label(UsbDevice device) {
    final name = device.productName ?? device.manufacturerName;
    final id = '${_hex(device.vid)}:${_hex(device.pid)}';
    return name == null ? id : '$name ($id)';
  }

  static String _hex(int? value) =>
      value == null ? '????' : value.toRadixString(16).padLeft(4, '0');
}

class _AndroidConnection implements SerialConnection {
  _AndroidConnection(this._port, this.incoming)
      : _writer = CoalescingWriter(_port.write);

  final UsbPort _port;
  final CoalescingWriter _writer;

  @override
  final Stream<Uint8List> incoming;

  @override
  Future<void> write(Uint8List bytes) {
    _writer.add(bytes);
    return _writer.done();
  }

  @override
  Future<void> close() async {
    await _writer.done();
    await _port.close();
  }
}
