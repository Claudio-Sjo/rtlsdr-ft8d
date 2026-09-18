# libft8 — vendored copy of ft8_lib

This directory is a **frozen, in-tree copy** of the subset of Karlis Goba's
`ft8_lib` that `rtlsdr-ft8d` actually compiles and includes. It replaces the
previous git submodule so the project builds without a submodule checkout and
without depending on the (now-unreachable) `git://` submodule URL.

## Upstream source

- Project: **ft8_lib** by Kārlis Goba (YL3JG)
- Repository: https://github.com/kgoba/ft8_lib
- Vendored from commit: **50ee0c06361388a992c80a1af9c1189652b72e51** (release **2.0**)
- License: **MIT** (see `LICENSE` in this directory; Copyright (c) 2018 Kārlis Goba)

The MIT license and the original file contents (including their copyright
headers) are preserved unmodified.

## Why a frozen copy instead of a submodule

`rtlsdr-ft8d` builds directly on `ft8_lib`'s exact API and behaviour — the
waterfall memory layout, `ftx_find_candidates` / `ftx_decode_candidate`, the
callsign-hash interface, `kFT8_Costas_pattern`, and the SNR/frequency handling
in the receiver all assume this specific version. Pinning a known-good copy in
the tree makes the build self-contained and reproducible, and prevents an
upstream change from silently breaking the decoder or the frequency/SNR
assumptions. Upstream fixes can still be brought in deliberately by re-copying
the relevant files and re-testing.

## Files included (only what the project uses)

Only the sources that are compiled or included by rtlsdr-ft8d are vendored;
the upstream demos, tests, tools and unused modules are intentionally omitted.

```
ft8/
  constants.c/.h   FT8 constants, Costas array, Gray code, tone tables
  crc.c/.h         CRC-14
  decode.c/.h      candidate search (Costas sync) + LDPC + CRC decode
  encode.c/.h      message -> FSK tone sequence
  ldpc.c/.h        LDPC decoder
  message.c/.h     pack/unpack of FT8 messages (incl. callsign hashing)
  text.c/.h        text/callsign helpers
  debug.h          logging macros (header-only)
common/
  monitor.c/.h     STFT waterfall build helper (uses KISS FFT)
  common.h         shared decode-result struct
  audio.h          audio I/O declarations (header only; .c not compiled)
  wave.h           WAV I/O declarations (header only; .c not compiled)
fft/
  kiss_fft.c/.h        KISS FFT
  kiss_fftr.c/.h       KISS real FFT
  _kiss_fft_guts.h     KISS FFT internal header
LICENSE            upstream MIT license
```

`audio.h` and `wave.h` are included by some source files but their `.c`
implementations are not compiled/linked (the code that would need them is behind
inactive `#ifdef`s), so only the headers are vendored.

## Updating

To pull a newer upstream version, re-copy the files above from the desired
`ft8_lib` commit, update the commit hash noted here, then rebuild and run the
decoder self-test (`rtlsdr_ft8d -t`). Review upstream API/behaviour changes
against the receiver's waterfall/decode/SNR/frequency code before adopting.
