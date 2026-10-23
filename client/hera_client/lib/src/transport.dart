/// Link mode (`PROTOCOL.md` §2). The application protocol is the same in all
/// modes; only link establishment and the number of sockets differ.
enum HeraTransport {
  /// The device is a WPA2 access point (§2.1). Default mode. The device's
  /// DHCP server announces no gateway, so the phone keeps cellular data as its
  /// default route (§2.4).
  accessPoint(
    'Hotspot',
    'The device is the access point. Default mode.',
    twoChannels: true,
  ),

  /// Device and phone on the same existing WiFi network (§2.2). Intended for
  /// bench use: the control port is reachable from the whole network and the
  /// phone's default route is not cellular.
  station(
    'Shared WiFi',
    'Same network as the phone. For bench use.',
    twoChannels: true,
  ),

  /// Serial line over USB (§2.3). Both channels are multiplexed on one byte
  /// stream (§4.4). Optional in the protocol; not implemented by the
  /// reference device.
  usb(
    'USB',
    'Serial cable, for bench diagnostics.',
    twoChannels: false,
  );

  const HeraTransport(
    this.label,
    this.description, {
    required this.twoChannels,
  });

  final String label;
  final String description;

  /// True if control and audio use separate sockets. Otherwise the
  /// multiplexing rules of §4.4 apply.
  final bool twoChannels;

  /// True if the underlying link is protected (WPA2 or cable). Diagnostic
  /// only (§11.1).
  bool get isProtected => this != station;

  /// Advertised BLE local name (§3.1).
  static const String deviceName = 'HERA';
}
