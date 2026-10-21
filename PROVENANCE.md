# Source provenance

SidebandAudio was extracted on October 21, 2026 from Stephen Walker's Sideband
audio implementation. This repository's code is distributed under the MIT
license in `LICENSE`. The parent application's license does not change.

The Swift interfaces and demo are developed independently in this repository.

The C DSP is an original implementation informed by research into podcast
audio processing. Its silence-shortening design uses fixed sample windows
documented in `Documentation/Engine.md`. It does not contain Overcast binaries,
Apple private audio units, or copied platform codec implementations. Similar
settings or signal paths do not establish perceptual parity with another player.

Apple builds use the system Accelerate framework for FFT and correlation.
The scalar C path uses the standard library and libm. Codec decoding and device
output use public AVFoundation APIs in the optional Apple player.

Demo audio attribution is recorded beside the bundled sample. Sample audio has
its own license; the code license does not override it.
