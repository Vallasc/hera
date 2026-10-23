import 'dart:typed_data';

import 'package:flutter/foundation.dart' show visibleForTesting;

import 'package:cryptography/cryptography.dart';
import 'package:cryptography/dart.dart';

import 'protocol.dart';

/// Encrypted profile of `PROTOCOL.md` §11.
///
/// Ephemeral X25519 per control connection, HKDF-SHA256 expanded into four
/// keys, ChaCha20-Poly1305 on every control record and audio datagram, and a
/// 64-entry anti-replay window on the audio channel.
///
/// The token is never transmitted; it keys the HMAC that authenticates each
/// `HELLO` (§11.4).
///
/// Sealing and opening are synchronous, since they run for every packet (up
/// to 200 per second per direction with 5 ms blocks). Key generation,
/// derivation and the `HELLO` tag run once per session and are asynchronous.
abstract final class HeraSec {
  static const int pubkeyLength = 32;
  static const int tagLength = 16;
  static const int counterLength = 8;

  /// Control record overhead: 2 bytes of length, 8 of counter, 16 of tag
  /// (§11.2).
  static const int recordOverhead = 2 + counterLength + tagLength;

  /// Datagram overhead: 4 bytes of `session_id`, 8 of counter, 16 of tag.
  static const int datagramOverhead =
      Hera.sessionIdSize + counterLength + tagLength;

  /// A record carries at most two maximum-size frames (§11.2).
  static const int maxPlain = 2 * Hera.maxFrame;
  static const int maxRecord = maxPlain + recordOverhead;

  /// HKDF info labels (§11.3).
  static const String labelCtrlC2d = 'hera/v4/ctrl/c2d';
  static const String labelCtrlD2c = 'hera/v4/ctrl/d2c';
  static const String labelAudioC2d = 'hera/v4/audio/c2d';
  static const String labelAudioD2c = 'hera/v4/audio/d2c';

  /// Prefix of the authenticated `HELLO` fields (§11.4).
  static const String labelHello = 'hera/v4/hello';

  /// ChaCha20-Poly1305 (IETF variant, 96-bit nonce), as required by §11.3.
  static final DartChacha20 _aead = DartChacha20.poly1305Aead();
}

/// An ephemeral X25519 key pair.
class HeraKeyPair {
  HeraKeyPair._(this._data, this.publicKey);

  final SimpleKeyPairData _data;

  /// The 32-byte public key carried in `HELLO` (§5.3).
  final Uint8List publicKey;

  /// Generates a new key pair. A new pair is required for every session and
  /// every resume (§11.3).
  static Future<HeraKeyPair> generate() async {
    final pair = await DartX25519().newKeyPair();
    final data = await pair.extract();
    final pub = await pair.extractPublicKey();
    return HeraKeyPair._(data, Uint8List.fromList(pub.bytes));
  }

  /// Builds a key pair from a known private key. Intended for tests against
  /// the §14.1 vectors.
  static Future<HeraKeyPair> fromPrivateKey(Uint8List privateKey) async {
    final pair = await DartX25519().newKeyPairFromSeed(privateKey);
    final data = await pair.extract();
    final pub = await pair.extractPublicKey();
    return HeraKeyPair._(data, Uint8List.fromList(pub.bytes));
  }
}

/// Sending direction of a channel: a key and a monotonically increasing
/// counter.
class HeraSealer {
  HeraSealer(this._key);

  final Uint8List _key;
  int _counter = 0;

  int get counter => _counter;

  /// The key, for tests against the §14.1 vectors only.
  @visibleForTesting
  Uint8List get debugKey => _key;
}

/// Receiving direction of a channel, with the anti-replay state.
///
/// `_highest` is the highest counter accepted and `_window` a bitmap of the 64
/// counters ending at `_highest`.
class HeraOpener {
  HeraOpener(this._key);

  final Uint8List _key;
  int _highest = 0;
  int _window = 0;
  bool _primed = false;

  bool get primed => _primed;
  int get highest => _highest;

  /// The key, for tests against the §14.1 vectors only.
  @visibleForTesting
  Uint8List get debugKey => _key;
}

/// The four keys of a session (§11.3).
class HeraSecSession {
  HeraSecSession._({
    required this.controlOut,
    required this.controlIn,
    required this.audioOut,
    required this.audioIn,
  });

  final HeraSealer controlOut;
  final HeraOpener controlIn;
  final HeraSealer audioOut;
  final HeraOpener audioIn;

  /// Derives the four session keys (§11.3).
  ///
  /// The salt is `pub_client || pub_device` regardless of which side is
  /// deriving.
  static Future<HeraSecSession> derive({
    required HeraKeyPair own,
    required Uint8List peerPublicKey,
    bool weAreDevice = false,
  }) async {
    if (peerPublicKey.length != HeraSec.pubkeyLength) {
      throw const HeraProtocolException('public key has the wrong length');
    }

    final shared = DartX25519().sharedSecretSync(
      keyPairData: own._data,
      remotePublicKey: SimplePublicKey(peerPublicKey, type: KeyPairType.x25519),
    );
    final sharedBytes = await shared.extractBytes();
    // Reject low-order peer keys, which yield an all-zero shared secret.
    if (sharedBytes.every((byte) => byte == 0)) {
      throw const HeraProtocolException('unusable public key');
    }

    final clientPub = weAreDevice ? peerPublicKey : own.publicKey;
    final devicePub = weAreDevice ? own.publicKey : peerPublicKey;
    const half = HeraSec.pubkeyLength;
    final salt = Uint8List(2 * half)
      ..setRange(0, half, clientPub)
      ..setRange(half, 2 * half, devicePub);

    Future<Uint8List> expand(String label) async {
      final kdf = Hkdf(hmac: Hmac.sha256(), outputLength: 32);
      final key = await kdf.deriveKey(
        secretKey: SecretKey(sharedBytes),
        nonce: salt,
        info: label.codeUnits,
      );
      return Uint8List.fromList(await key.extractBytes());
    }

    // One key and one counter per channel and direction (§11.2).
    final outCtrl = weAreDevice ? HeraSec.labelCtrlD2c : HeraSec.labelCtrlC2d;
    final inCtrl = weAreDevice ? HeraSec.labelCtrlC2d : HeraSec.labelCtrlD2c;
    final outAudio =
        weAreDevice ? HeraSec.labelAudioD2c : HeraSec.labelAudioC2d;
    final inAudio = weAreDevice ? HeraSec.labelAudioC2d : HeraSec.labelAudioD2c;

    return HeraSecSession._(
      controlOut: HeraSealer(await expand(outCtrl)),
      controlIn: HeraOpener(await expand(inCtrl)),
      audioOut: HeraSealer(await expand(outAudio)),
      audioIn: HeraOpener(await expand(inAudio)),
    );
  }
}

// ------------------------------------------------- control records (§11.2)

/// Seals [plain] into a control record.
///
/// Synchronous, so that records are written in counter order.
Uint8List sealHeraRecord(HeraSealer tx, Uint8List plain) {
  if (plain.isEmpty || plain.length > HeraSec.maxPlain) {
    throw HeraProtocolException(
      'record of ${plain.length} bytes, maximum ${HeraSec.maxPlain}',
    );
  }
  final counter = tx._counter++;
  final header = Uint8List(2 + HeraSec.counterLength);
  final recLen = HeraSec.counterLength + plain.length + HeraSec.tagLength;
  header[0] = recLen & 0xFF;
  header[1] = (recLen >> 8) & 0xFF;
  _putUint64(header, 2, counter);

  final box = HeraSec._aead.encryptSync(
    plain,
    secretKey: SecretKeyData(tx._key),
    nonce: _nonceOf(counter),
    aad: header,
  );

  final out = Uint8List(header.length + plain.length + HeraSec.tagLength)
    ..setRange(0, header.length, header)
    ..setRange(header.length, header.length + plain.length, box.cipherText)
    ..setRange(header.length + plain.length,
        header.length + plain.length + HeraSec.tagLength, box.mac.bytes);
  return out;
}

/// Total length of the record starting at [offset], or 0 if the declared
/// length is out of range. At least two bytes must be readable.
int heraRecordLength(Uint8List buffer, int offset) {
  final declared = buffer[offset] | (buffer[offset + 1] << 8);
  if (declared < HeraSec.counterLength + HeraSec.tagLength ||
      declared > HeraSec.maxPlain + HeraSec.counterLength + HeraSec.tagLength) {
    return 0;
  }
  return 2 + declared;
}

/// Opens a complete record.
///
/// Throws [HeraProtocolException] if the record is malformed, the counter does
/// not increase or authentication fails. The caller must close the control
/// channel (§11.2).
Uint8List openHeraRecord(HeraOpener rx, Uint8List record) {
  if (record.length < HeraSec.recordOverhead ||
      heraRecordLength(record, 0) != record.length) {
    throw const HeraProtocolException('malformed record');
  }
  final counter = _getUint64(record, 2);
  if (rx._primed && counter <= rx._highest) {
    throw const HeraProtocolException('record counter did not increase');
  }

  final header = Uint8List.sublistView(record, 0, 2 + HeraSec.counterLength);
  final body = Uint8List.sublistView(record, 2 + HeraSec.counterLength);
  final plain = _open(rx._key, counter, header, body);

  rx._primed = true;
  rx._highest = counter;
  return plain;
}

// --------------------------------------------------- audio datagrams (§11.2)

/// Seals one frame into a datagram.
Uint8List sealHeraDatagram(HeraSealer tx, int sessionId, Uint8List frame) {
  if (frame.isEmpty || frame.length > Hera.maxFrame) {
    throw HeraProtocolException('invalid audio frame size: ${frame.length}');
  }
  final counter = tx._counter++;
  final header = Uint8List(Hera.sessionIdSize + HeraSec.counterLength);
  _putUint32(header, 0, sessionId);
  _putUint64(header, Hera.sessionIdSize, counter);

  final box = HeraSec._aead.encryptSync(
    frame,
    secretKey: SecretKeyData(tx._key),
    nonce: _nonceOf(counter),
    aad: header,
  );

  final out = Uint8List(header.length + frame.length + HeraSec.tagLength)
    ..setRange(0, header.length, header)
    ..setRange(header.length, header.length + frame.length, box.cipherText);
  out.setRange(header.length + frame.length, out.length, box.mac.bytes);
  return out;
}

/// Opens a datagram. Returns null if the session id does not match, the
/// counter is a replay or authentication fails; the datagram is then
/// discarded (§4.2).
Uint8List? openHeraDatagram(HeraOpener rx, int sessionId, Uint8List datagram) {
  if (datagram.length < HeraSec.datagramOverhead + 1) return null;
  if (_getUint32(datagram, 0) != sessionId) return null;

  final counter = _getUint64(datagram, Hera.sessionIdSize);
  // Check the window before decrypting, to reject replays cheaply.
  if (rx._primed && counter <= rx._highest) {
    final behind = rx._highest - counter;
    if (behind >= 64) return null;
    if ((rx._window >> behind) & 1 != 0) return null;
  }

  final split = Hera.sessionIdSize + HeraSec.counterLength;
  final header = Uint8List.sublistView(datagram, 0, split);
  final body = Uint8List.sublistView(datagram, split);
  final Uint8List plain;
  try {
    plain = _open(rx._key, counter, header, body);
  } on HeraProtocolException {
    return null;
  }

  // Update the window only after successful authentication.
  _accept(rx, counter);
  return plain;
}

/// Computes the `HELLO` tag (§11.4): HMAC-SHA256 keyed with the token,
/// truncated to 16 bytes.
Future<Uint8List> heraHelloTag({
  required Uint8List token,
  required int role,
  required int formats,
  required int durations,
  required int flags,
  required int sessionId,
  required Uint8List publicKey,
}) async {
  final transcript = BytesBuilder(copy: false)
    ..add(HeraSec.labelHello.codeUnits)
    ..addByte(role)
    ..addByte(formats)
    ..addByte(durations)
    ..addByte(flags);
  final sid = Uint8List(4);
  _putUint32(sid, 0, sessionId);
  transcript
    ..add(sid)
    ..add(publicKey);

  final mac = await Hmac.sha256().calculateMac(
    transcript.takeBytes(),
    secretKey: SecretKey(token),
  );
  return Uint8List.fromList(mac.bytes.sublist(0, HeraSec.tagLength));
}

/// Constant-time comparison of two tags (§11.4).
bool heraTagEqual(Uint8List a, Uint8List b) {
  if (a.length != b.length) return false;
  var diff = 0;
  for (var i = 0; i < a.length; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

// ----------------------------------------------------------------- internal

/// nonce = 00 00 00 00 || counter (u64 LE).
Uint8List _nonceOf(int counter) {
  final nonce = Uint8List(12);
  _putUint64(nonce, 4, counter);
  return nonce;
}

Uint8List _open(
  Uint8List key,
  int counter,
  Uint8List header,
  Uint8List body,
) {
  if (body.length < HeraSec.tagLength) {
    throw const HeraProtocolException('ciphertext shorter than the tag');
  }
  final cut = body.length - HeraSec.tagLength;
  final box = SecretBox(
    Uint8List.sublistView(body, 0, cut),
    nonce: _nonceOf(counter),
    mac: Mac(Uint8List.sublistView(body, cut)),
  );
  try {
    return Uint8List.fromList(HeraSec._aead.decryptSync(
      box,
      secretKey: SecretKeyData(key),
      aad: header,
    ));
  } on SecretBoxAuthenticationError {
    throw const HeraProtocolException('authentication failed');
  }
}

/// Sliding anti-replay window (§11.2), as in RFC 4303 §3.4.3.
void _accept(HeraOpener rx, int counter) {
  if (!rx._primed) {
    rx._primed = true;
    rx._highest = counter;
    rx._window = 1;
    return;
  }
  if (counter > rx._highest) {
    final advance = counter - rx._highest;
    rx._window = advance >= 64 ? 1 : ((rx._window << advance) | 1);
    rx._highest = counter;
    return;
  }
  final behind = rx._highest - counter;
  if (behind < 64) rx._window |= 1 << behind;
}

void _putUint32(Uint8List out, int at, int value) {
  out[at] = value & 0xFF;
  out[at + 1] = (value >> 8) & 0xFF;
  out[at + 2] = (value >> 16) & 0xFF;
  out[at + 3] = (value >> 24) & 0xFF;
}

void _putUint64(Uint8List out, int at, int value) {
  for (var i = 0; i < 8; i++) {
    out[at + i] = (value >> (8 * i)) & 0xFF;
  }
}

int _getUint32(Uint8List data, int at) =>
    data[at] |
    (data[at + 1] << 8) |
    (data[at + 2] << 16) |
    (data[at + 3] << 24);

int _getUint64(Uint8List data, int at) {
  var value = 0;
  for (var i = 0; i < 8; i++) {
    value |= data[at + i] << (8 * i);
  }
  return value;
}
