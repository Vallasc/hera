# hera_audio

Audio primitives used by `hera_client`: PCM16 conversion, G.711, sample rate
conversion and WAV encoding.

Pure Dart: no Flutter dependency, no I/O and no global state.

## Contents

| | |
|---|---|
| `AudioFormat` | encoding and sample rate; parses both the string (`pcm16`) and the object (`{"type": "audio/pcm", "rate": 16000}`) conventions used by Realtime-style session APIs |
| `AudioDecoder` | wire bytes to float samples at a target rate; keeps state across packets |
| `AudioEncoder` | float samples to wire bytes in a given format |
| `G711` | A-law and µ-law, following the CCITT reference implementation |
| `Resampler` | streaming polyphase sample rate converter for arbitrary rational ratios |
| `pcm16BytesOf`, `toPcm16` | conversions between normalised floats and 16-bit integers |
| `wavBytes` | mono 16-bit PCM WAV encoding |

`AudioDecoder` keeps the odd trailing byte of a PCM16 packet whose length is
odd and combines it with the next packet. Packet sizes are chosen by the
sender, and dropping that byte would misalign every following sample.

## Tests

```sh
dart test
```

Among other things, the tests check that the resampler output does not
depend on how the input is split into packets, and that G.711 keeps a
signal-to-noise ratio above 25 dB on a speech-level tone.
