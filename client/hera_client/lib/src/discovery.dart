import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:flutter_blue_plus/flutter_blue_plus.dart';
import 'package:permission_handler/permission_handler.dart';

import 'protocol.dart';
import 'transport.dart';

/// GATT service and characteristic UUIDs (`PROTOCOL.md` §3.1).
abstract final class HeraGatt {
  static final Guid service = Guid('48455241-0001-4C32-9A3E-8F1D5B7C2E01');
  static final Guid netState = Guid('48455241-0002-4C32-9A3E-8F1D5B7C2E01');
  static final Guid netConfig = Guid('48455241-0003-4C32-9A3E-8F1D5B7C2E01');
  static final Guid session = Guid('48455241-0004-4C32-9A3E-8F1D5B7C2E01');
}

/// Device network state (§3.2).
enum HeraNetPhase { down, bringingUp, up, failed }

/// Decoded `NET_STATE` value (§3.2).
@immutable
class HeraNetState {
  const HeraNetState({
    required this.mode,
    required this.phase,
    required this.failure,
    required this.address,
    required this.controlPort,
    required this.audioPort,
    required this.ssid,
  });

  final HeraNetMode? mode;
  final HeraNetPhase phase;

  /// Failure code (§3.2.1). Meaningful only when [phase] is
  /// [HeraNetPhase.failed].
  final int failure;

  /// Device address, or null if it has none.
  final InternetAddress? address;

  final int controlPort;
  final int audioPort;

  /// SSID of the hotspot in `AP` mode, or of the joined network in `STA`
  /// mode.
  final String ssid;

  bool get isUp => phase == HeraNetPhase.up && address != null;

  /// Transport corresponding to [mode].
  HeraTransport? get transport => switch (mode) {
        HeraNetMode.accessPoint => HeraTransport.accessPoint,
        HeraNetMode.station => HeraTransport.station,
        HeraNetMode.usb => HeraTransport.usb,
        null => null,
      };

  static String describeFailure(int code) => switch (code) {
        0 => 'no error',
        1 => 'SSID not found',
        2 => 'authentication rejected',
        3 => 'no IP address obtained',
        4 => 'interface not available',
        5 => 'network service did not start',
        6 => 'no stored credentials',
        _ => 'error $code',
      };

  /// Decodes a `NET_STATE` value. Returns null if it is malformed or has a
  /// different protocol version.
  static HeraNetState? parse(List<int> raw) {
    if (raw.length < 13 || raw[0] != Hera.protocolVersion) return null;
    final data = Uint8List.fromList(raw);
    final ssidLength = data[12];
    if (data.length < 13 + ssidLength) return null;
    final octets = data.sublist(4, 8);
    return HeraNetState(
      mode: HeraNetMode.fromId(data[1]),
      phase: data[2] < HeraNetPhase.values.length
          ? HeraNetPhase.values[data[2]]
          : HeraNetPhase.down,
      failure: data[3],
      address: octets.every((o) => o == 0)
          ? null
          : InternetAddress(octets.join('.')),
      controlPort: data[8] | (data[9] << 8),
      audioPort: data[10] | (data[11] << 8),
      ssid:
          utf8.decode(data.sublist(13, 13 + ssidLength), allowMalformed: true),
    );
  }

  @override
  String toString() => 'HeraNetState(${mode?.label}, ${phase.name}, '
      '${address?.address ?? '—'}:$controlPort, ssid: "$ssid")';
}

/// Credentials read from `SESSION` (§3.4). Applications should store them, so
/// that later connections can skip BLE (§3.5).
@immutable
class HeraCredentials {
  const HeraCredentials({required this.token, required this.passphrase});

  /// 16-byte session token (§11.4).
  final Uint8List token;

  /// WPA2 passphrase of the hotspot. Empty outside `AP` mode.
  final String passphrase;

  static HeraCredentials? parse(List<int> raw) {
    if (raw.length < 1 + Hera.tokenSize + 1) return null;
    if (raw[0] != Hera.protocolVersion) return null;
    final data = Uint8List.fromList(raw);
    final length = data[1 + Hera.tokenSize];
    if (data.length < 2 + Hera.tokenSize + length) return null;
    return HeraCredentials(
      token: data.sublist(1, 1 + Hera.tokenSize),
      passphrase: utf8.decode(
        data.sublist(2 + Hera.tokenSize, 2 + Hera.tokenSize + length),
        allowMalformed: true,
      ),
    );
  }

  @override
  String toString() => 'HeraCredentials(token: ${token.length} bytes, '
      'passphrase: ${passphrase.isEmpty ? 'none' : '${passphrase.length} chars'})';
}

/// `NET_CFG` operations (§3.3.1).
abstract final class HeraNetOp {
  static const int startAccessPoint = 1;
  static const int joinStation = 2;
  static const int forgetStation = 3;
  // 4 is reserved (§3.3.1).
  static const int restart = 5;
}

/// Builds a `NET_CFG` value (§3.3).
Uint8List buildHeraNetConfig({
  required int op,
  String ssid = '',
  String passphrase = '',
}) {
  final name = utf8.encode(ssid);
  final secret = utf8.encode(passphrase);
  if (name.length > 255 || secret.length > 255) {
    throw const HeraProtocolException('SSID or passphrase too long');
  }
  return Uint8List.fromList([
    Hera.protocolVersion,
    op,
    name.length,
    secret.length,
    ...name,
    ...secret,
  ]);
}

/// A HERA device found by a scan.
@immutable
class HeraDevice {
  const HeraDevice({required this.id, required this.name});

  final String id;
  final String name;

  @override
  bool operator ==(Object other) => other is HeraDevice && other.id == id;

  @override
  int get hashCode => id.hashCode;
}

/// BLE discovery and provisioning (§3).
///
/// BLE is used only to find the device, read its network state, deliver
/// credentials and change the network configuration. It carries no audio and
/// no call control. It also serves as the recovery path when the IP link is
/// unavailable (§2.5).
class HeraDiscovery {
  HeraDiscovery({this.scanTimeout = const Duration(seconds: 6)});

  final Duration scanTimeout;

  BluetoothDevice? _device;
  BluetoothCharacteristic? _netState;
  BluetoothCharacteristic? _netConfig;
  BluetoothCharacteristic? _session;
  StreamSubscription<List<int>>? _notifications;

  final StreamController<HeraNetState> _states =
      StreamController<HeraNetState>.broadcast();

  /// `NET_STATE` notifications (§3.2), e.g. a fallback to `AP` after a
  /// failed `STA` join.
  Stream<HeraNetState> get states => _states.stream;

  /// Time allowed for the Bluetooth adapter to report its state.
  static const Duration adapterStateTimeout = Duration(seconds: 5);

  /// Permissions required to scan and connect.
  static const List<Permission> _needed = [
    Permission.bluetoothScan,
    Permission.bluetoothConnect,
  ];

  /// Requests the permissions. Call from the UI only: on Android this shows a
  /// system dialog and requires an Activity. Background code should use
  /// [hasPermissions].
  static Future<bool> ensurePermissions() async {
    if (!Platform.isAndroid && !Platform.isIOS) return true;
    final results = await _needed.request();
    return results.values.every((status) => status.isGranted);
  }

  /// Checks the permissions without requesting them. Safe to call from a
  /// background service.
  static Future<bool> hasPermissions() async {
    if (!Platform.isAndroid && !Platform.isIOS) return true;
    for (final permission in _needed) {
      if (!await permission.status.isGranted) return false;
    }
    return true;
  }

  /// Scans for HERA devices.
  ///
  /// No platform scan filter is used. The name is in the advertising data and
  /// the service UUID in the scan response (§3.1), and platform filters differ
  /// in which of the two they match; results are filtered here instead.
  ///
  /// With [stopOnFirst] the scan ends as soon as a device is found, instead
  /// of running for the whole [scanTimeout].
  Future<List<HeraDevice>> scan({bool stopOnFirst = false}) async {
    if (!await FlutterBluePlus.isSupported) {
      throw const HeraProtocolException(
          'Bluetooth is not supported on this device');
    }
    // Wait for the adapter state instead of reading `adapterStateNow`, which
    // is `unknown` until the plugin has received its first event. The
    // transient `turningOn` state is also waited out.
    final state = await FlutterBluePlus.adapterState
        .firstWhere(
          (state) =>
              state != BluetoothAdapterState.unknown &&
              state != BluetoothAdapterState.turningOn,
          // Avoids a StateError should the stream ever complete.
          orElse: () => FlutterBluePlus.adapterStateNow,
        )
        .timeout(adapterStateTimeout,
            onTimeout: () => FlutterBluePlus.adapterStateNow);
    if (state != BluetoothAdapterState.on) {
      throw HeraProtocolException(switch (state) {
        BluetoothAdapterState.off => 'Bluetooth is off',
        BluetoothAdapterState.unauthorized =>
          'Bluetooth permission not granted',
        // `unknown` means the adapter did not answer, not that it is off.
        BluetoothAdapterState.unknown =>
          'Bluetooth adapter not responding; will retry',
        final other => 'Bluetooth not available: ${other.name}',
      });
    }

    final found = <String, HeraDevice>{};
    for (final device in FlutterBluePlus.connectedDevices) {
      if (_looksLikeHera(device.platformName)) {
        found[device.remoteId.str] = HeraDevice(
          id: device.remoteId.str,
          name: device.platformName,
        );
      }
    }

    final subscription = FlutterBluePlus.onScanResults.listen((results) {
      for (final result in results) {
        final name = result.advertisementData.advName.isNotEmpty
            ? result.advertisementData.advName
            : result.device.platformName;
        final advertisesService = result.advertisementData.serviceUuids
            .any((uuid) => uuid == HeraGatt.service);
        if (!advertisesService && !_looksLikeHera(name)) continue;
        found[result.device.remoteId.str] = HeraDevice(
          id: result.device.remoteId.str,
          name: name.isEmpty ? HeraTransport.deviceName : name,
        );
      }
      if (stopOnFirst && found.isNotEmpty) unawaited(FlutterBluePlus.stopScan());
    });
    if (stopOnFirst && found.isNotEmpty) {
      await subscription.cancel();
      return found.values.toList();
    }
    try {
      await FlutterBluePlus.startScan(timeout: scanTimeout);
      await FlutterBluePlus.isScanning.where((running) => !running).first;
    } finally {
      await subscription.cancel();
    }
    return found.values.toList();
  }

  static bool _looksLikeHera(String name) =>
      name.toUpperCase().startsWith(HeraTransport.deviceName);

  /// GATT status 5 (insufficient authentication) right after connecting:
  /// the link was encrypted with keys the device does not have.
  static const int _gattInsufficientAuthentication = 5;

  static void _throwIfStalePairing(BluetoothDevice device) {
    final reason = device.disconnectReason;
    if (reason?.code == _gattInsufficientAuthentication) {
      debugPrint('HeraDiscovery: disconnected with ${reason?.description}: '
          'stale pairing');
      throw const HeraStalePairingException();
    }
  }

  /// Connects to [device] and discovers the three characteristics.
  Future<void> connect(HeraDevice device) async {
    await disconnect();
    final target = BluetoothDevice.fromId(device.id);
    await target.connect(timeout: const Duration(seconds: 12));
    final List<BluetoothService> services;
    try {
      // NET_STATE and SESSION can exceed the default ATT MTU (§3.4).
      if (Platform.isAndroid) {
        try {
          await target.requestMtu(128);
        } catch (error) {
          debugPrint('HeraDiscovery: MTU request failed, $error');
        }
      }
      services = await target.discoverServices();
    } catch (_) {
      _throwIfStalePairing(target);
      rethrow;
    }
    for (final service in services) {
      if (service.serviceUuid != HeraGatt.service) continue;
      for (final characteristic in service.characteristics) {
        switch (characteristic.characteristicUuid) {
          case final uuid when uuid == HeraGatt.netState:
            _netState = characteristic;
          case final uuid when uuid == HeraGatt.netConfig:
            _netConfig = characteristic;
          case final uuid when uuid == HeraGatt.session:
            _session = characteristic;
        }
      }
    }
    if (_netState == null || _session == null) {
      await target.disconnect();
      throw const HeraProtocolException(
        'the device does not expose the HERA service',
      );
    }
    _device = target;

    final state = _netState!;
    if (state.properties.notify) {
      await state.setNotifyValue(true);
      _notifications = state.onValueReceived.listen((raw) {
        final parsed = HeraNetState.parse(raw);
        if (parsed != null && !_states.isClosed) _states.add(parsed);
      });
    }
  }

  /// Reads `NET_STATE`. Being `encrypt-read`, the first read triggers
  /// pairing (§3.1).
  Future<HeraNetState> readNetState() async {
    final characteristic = _netState;
    if (characteristic == null) {
      throw const HeraProtocolException('no device connected');
    }
    final parsed = HeraNetState.parse(await characteristic.read());
    if (parsed == null) {
      throw const HeraProtocolException('cannot decode NET_STATE (protocol '
          'version mismatch?)');
    }
    return parsed;
  }

  /// Reads the token and passphrase from `SESSION`.
  Future<HeraCredentials> readCredentials() async {
    final characteristic = _session;
    if (characteristic == null) {
      throw const HeraProtocolException('no device connected');
    }

    final List<int> raw;
    try {
      raw = await characteristic.read();
    } on FlutterBluePlusException catch (error) {
      // SESSION is refused while another client holds a session (§3.4).
      if (error.code == _gattReadNotPermitted) {
        throw const HeraProtocolException(
          'the device is in use by another client; it becomes available '
          'when that client disconnects',
        );
      }
      rethrow;
    }

    final parsed = HeraCredentials.parse(raw);
    if (parsed == null) {
      throw const HeraProtocolException('cannot decode SESSION');
    }
    return parsed;
  }

  /// ATT error Read Not Permitted (0x02).
  static const int _gattReadNotPermitted = 2;

  /// Writes `NET_CFG`. The write is acknowledged before the operation is
  /// applied; progress is reported through [states] (§3.3).
  Future<void> configure({
    required int op,
    String ssid = '',
    String passphrase = '',
  }) async {
    final characteristic = _netConfig;
    if (characteristic == null) {
      throw const HeraProtocolException(
        'the device does not expose NET_CFG',
      );
    }
    await characteristic.write(
      buildHeraNetConfig(op: op, ssid: ssid, passphrase: passphrase),
      withoutResponse: false,
    );
  }

  /// Switches the device to `AP` mode (§2.1).
  Future<void> useAccessPoint() => configure(op: HeraNetOp.startAccessPoint);

  /// Makes the device join an existing WiFi network (§2.2).
  Future<void> joinNetwork({
    required String ssid,
    required String passphrase,
  }) =>
      configure(
        op: HeraNetOp.joinStation,
        ssid: ssid,
        passphrase: passphrase,
      );

  Future<void> disconnect() async {
    await _notifications?.cancel();
    _notifications = null;
    _netState = null;
    _netConfig = null;
    _session = null;
    final device = _device;
    _device = null;
    if (device != null) {
      try {
        await device.disconnect();
      } catch (error) {
        debugPrint('HeraDiscovery: disconnect failed, $error');
      }
    }
  }

  Future<void> dispose() async {
    await disconnect();
    await _states.close();
  }
}
