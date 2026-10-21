# Demo speech sample

`MoonChallenge.wav` contains John F. Kennedy's statement about landing a person
on the Moon, from his Special Message to Congress on May 25, 1961.

- Source: [Wikimedia Commons: Discurso de Kennedy](https://commons.wikimedia.org/wiki/File:Discurso_de_Kennedy.ogg)
- Original upload: December 23, 2004, by Campani.
- Downloaded original (before the later denoising edit):
  https://upload.wikimedia.org/wikipedia/commons/archive/8/8d/20200619003957%21Discurso_de_Kennedy.ogg
- Rights: the source identifies the recording as a United States government
  work in the public domain in the United States (PD-USGov-NASA).
- Preparation: decoded to mono, 44.1 kHz, signed 16-bit PCM WAV with FFmpeg.
  One second of digital silence was added before and after the speech to make
  the silence-shortening comparison repeatable despite noise in the archival
  recording. No loudness processing or silence removal was applied.

The recording is included locally. The demo makes no network requests.
The sample's public-domain status is separate from the repository's MIT code license.
