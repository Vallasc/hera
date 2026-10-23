import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter_libserialport/flutter_libserialport.dart';

import 'protocol.dart';
import 'serial_port.dart';

/// Serial driver for macOS, Windows and Linux, using libserialport.
///
/// No permission is required. Attach events are not reported; the port list
/// is read again when needed.
class DesktopSerialDriver extends SerialDriver {
  @override
  bool looksLikeBoard(SerialDevice device) =>
      SerialDevice.knownBridgeVendors.contains(device.vendorId) ||
      RegExp(
        r'usbserial|usbmodem|wchusbserial|ttyUSB|ttyACM|^COM\d+$',
      ).hasMatch(device.id);
  @override
  Future<List<SerialDevice>> devices() async {
    final devices = <SerialDevice>[];
    for (final name in SerialPort.availablePorts) {
      final port = SerialPort(name);
      try {
        devices.add(
          SerialDevice(
            id: name,
            label: _label(name, port),
            vendorId: port.vendorId,
            productId: port.productId,
          ),
        );
      } finally {
        port.dispose();
      }
    }
    return devices;
  }

  @override
  Future<SerialConnection> open(
    SerialDevice device, {
    int baudRate = Hera.serialBaudRate,
  }) async {
    final port = SerialPort(device.id);
    if (!port.openReadWrite()) {
      port.dispose();
      throw SerialException(
        'Cannot open ${device.id}: '
        '${SerialPort.lastError?.message ?? 'port busy or missing'}',
      );
    }

    final config = port.config;
    config.baudRate = baudRate;
    config.bits = 8;
    config.parity = SerialPortParity.none;
    config.stopBits = 1;
    config.setFlowControl(SerialPortFlowControl.none);
    // On many boards DTR and RTS drive reset and boot mode.
    config.dtr = SerialPortDtr.off;
    config.rts = SerialPortRts.off;
    port.config = config;
    config.dispose();

    return _DesktopConnection(port);
  }

  static String _label(String name, SerialPort port) {
    final description = port.productName ?? port.description;
    return description == null || description.isEmpty
        ? name
        : '$name — $description';
  }
}

class _DesktopConnection implements SerialConnection {
  _DesktopConnection(this._port) : _reader = SerialPortReader(_port);

  final SerialPort _port;
  final SerialPortReader _reader;

  @override
  Stream<Uint8List> get incoming => _reader.stream;

  @override
  Future<void> write(Uint8List bytes) async {
    try {
      // Blocking write with a short timeout: at 460800 baud a 342-byte
      // write takes 7.4 ms.
      final written = _port.write(bytes, timeout: 200);
      if (written != bytes.length) {
        debugPrint('serial: wrote $written of ${bytes.length} bytes');
      }
    } on SerialPortError catch (error) {
      debugPrint('serial: write failed, $error');
    }
  }

  @override
  Future<void> close() async {
    _reader.close();
    _port.close();
    _port.dispose();
  }
}
