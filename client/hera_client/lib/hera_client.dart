/// Client implementation of the HERA protocol, version 4 (`PROTOCOL.md`).
///
/// A HERA device is a Bluetooth hands-free unit that relays call audio and
/// call control to a client over IP. This library implements the client side:
/// BLE discovery and provisioning, joining the device's network, the control
/// (TCP) and audio (UDP) channels, and automatic reconnection.
///
/// [HeraConnection] is the main entry point. It exposes call events and audio
/// blocks; transport details are hidden below [HeraLink].
library;

export 'package:hera_audio/hera_audio.dart' show AudioEncoding, AudioFormat;

export 'src/channel.dart';
export 'src/connection.dart';
export 'src/discovery.dart';
export 'src/protocol.dart';
export 'src/sec.dart';
export 'src/serial_port.dart' show SerialDevice, SerialDriver, SerialException;
export 'src/session.dart';
export 'src/status.dart';
export 'src/transport.dart';
export 'src/serial_link.dart';
export 'src/wifi_link.dart';
