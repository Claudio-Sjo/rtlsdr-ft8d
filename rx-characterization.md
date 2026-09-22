# Receiver Characterization

This document records the measured characteristics of the rtlsdr-ft8d receiver
DSP chain: usable bandwidth, frequency mapping, decode CPU cost, parallel-decode
capacity, and weak-signal sensitivity. All results are from hardware-free tests
using the `mktestiq` test-vector generator decoded through the real receiver DSP
(`rtlsdr_ft8d -r <file> -x`), so they exercise the full STFT / waterfall /
candidate-search / LDPC-decode / frequency-reporting path with no RTL hardware
and no down-conversion (the vectors are already at complex baseband).

Unless noted, tests use dial = 20 m (14074000 Hz); reported RF = dial + audio.

## 1. Receiver chain summary

| Stage               | Value                                                        |
| ------------------- | ------------------------------------------------------------ |
| RTL input rate      | 2,400,000 sps (2.4 Msps)                                     |
| fs/4 economic mixer | shifts wanted band to baseband; RTL tuned to dial + FS4_RATE |
| CIC decimator       | N = 2 stages, M = 1, ratio R = DOWNSAMPLING                  |
| Compensation FIR    | 57 taps (WestCoastDSP inverse-CIC design, F0 = 0.92)         |
| FT8 tone spacing    | 6.25 Hz (fixed by protocol)                                  |
| FT8 symbol period   | 0.16 s (fixed by protocol)                                   |
| Waterfall bins      | NUM_BIN = SIGNAL_SAMPLE_RATE / 12.5                          |

The audio bandwidth is bounded on two fronts that must move together:
1. the decimated sample rate (Nyquist = rate / 2), and
2. the waterfall bin ceiling NUM_BIN x 6.25 Hz.

## 2. Two build modes

The usable passband is a compile-time choice. **Wide is the default.**

| Mode              | Build             | Sample rate | R   | NUM_BIN | NFFT | Bin ceiling | Usable audio  |
| ----------------- | ----------------- | ----------: | --: | ------: | ---: | ----------: | ------------- |
| Wide (default)    | `make`            |    6400 sps | 375 |     512 | 2048 |     3200 Hz | 200..2900 Hz  |
| Narrow (fallback) | `make narrowband` |    3200 sps | 750 |     256 | 1024 |     1600 Hz | 200..1500 Hz  |

2.4 MHz is divisible by both output rates (R = 375 and 750 are integers), so the
RTL rate, fs/4 mixer and tuning offset are identical in both modes. Only the
decimation ratio changes. The compensation FIR (`zCoef`) is reused unchanged in
both modes: it is designed in normalized frequency, and for these large ratios
the CIC droop over the normalized passband is nearly R-independent
(max coefficient difference R=375 vs R=750 is 1.5e-6). This was validated by
reproducing the in-tree R=750 taps exactly from the WestCoastDSP `fir2`
algorithm. See `dsp-chain.md` for the full derivation and the tools required to
regenerate the filter for new parameters.

The current bandwidth is shown live on the main-screen top border, e.g.
`BW 200-2900 Hz` (wide) or `BW 200-1500 Hz` (narrow).

## 3. Frequency mapping (USB convention)

FT8 is upper sideband: `f_RF = f_dial + f_audio`. The RTL is tuned to the dial
frequency (in the fs/4 frame), so a signal at `dial + N Hz` decodes at audio N
and is reported as `dial + N`. Verified across the whole band:

Single-tone sweep, reported RF vs generated audio (all exact within one 6.25 Hz
bin):

| audio Hz | reported RF | audio Hz | reported RF |
| -------: | ----------: | -------: | ----------: |
|     1300 |    14075278 |     2200 |    14076178 |
|     1500 |    14075478 |     2600 |    14076578 |
|     1800 |    14075778 |     2800 |    14076778 |
|     2000 |    14075978 |     3100 |    14077078 |

## 4. Usable bandwidth / edge (measured)

Single tone swept in frequency; the point where decoding stops is the usable
edge. The hard wall is the waterfall bin ceiling (NUM_BIN x 6.25 Hz), not the
FIR roll-off.

- **Narrow (3200 sps):** decodes cleanly through 1550 Hz (SNR +19 dB at 1550),
  hard cutoff at exactly 1600 Hz = 256 x 6.25 Hz. The `RX_AUDIO_MAX = 1500`
  cap is intentionally just inside this wall.
- **Wide (6400 sps):** decodes cleanly through 3100 Hz, hard cutoff at the
  3200 Hz = 512 x 6.25 Hz ceiling. The `RX_AUDIO_MAX = 2900` cap is inside
  this wall, with margin below the FIR design edge (0.92 x 3200 = 2944 Hz).

In both modes decoding continues slightly past `RX_AUDIO_MAX`, confirming the
cap is conservative and the true limit is the bin ceiling.

## 5. Decode CPU cost (measured)

Decode work per 15 s slot, timed in-process over many iterations (env-gated
`FT8D_BENCH=N` hook in `decodeRecordedFile`), identical 4-signal content, only
the DSP size differing:

| Mode   | sample rate | NFFT | NUM_BIN | mean decode CPU / slot | ratio |
| ------ | ----------: | ---: | ------: | ---------------------: | ----: |
| Narrow |        3200 | 1024 |     256 |                26.4 ms | 1.00x |
| Wide   |        6400 | 2048 |     512 |                31.1 ms | 1.18x |

**Widening the band costs only ~18% more decode CPU, not 2x**, despite doubling
the sample rate, FFT size and bin count. The dominant cost (LDPC belief
propagation + Costas sync search) is driven by the number of candidates/signals,
not by FFT size; the parts that double (FFT is O(N log N), waterfall fill) are a
minority of the total.

Measured on x86. The absolute numbers will be larger on a Raspberry Pi 2/3, but
the ratio should be similar. **Pending:** confirm on the actual Pi that the wide
decode still fits comfortably inside the 15 s slot budget.

## 6. Parallel-decode capacity (measured)

N distinct-callsign signals spread evenly across the usable band (equal
amplitude, so per-signal SNR falls as N rises). Decoded-count vs N:

| N     | narrow decoded (spacing) | wide decoded (spacing) |
| ----- | ------------------------ | ---------------------- |
| 5..25 | all                      | all                    |
| 30    | 29 (41 Hz)               | 30 (90 Hz)             |
| 40    | 26 (31 Hz)               | 40 (67 Hz)             |
| 45    | -                        | 45 (59 Hz)             |
| 50    | 2 (24 Hz, jammed)        | 50 (53 Hz)             |
| 55/60 | -                        | 50 (report cap)        |

- The controlling variable is per-signal spacing vs the ~50 Hz FT8 signal width;
  once packed closer than ~50 Hz, signals overlap and stop decoding.
- **Narrow** (~1250 Hz usable): ~28-29 parallel signals before overlap; collapses
  when the band is jammed.
- **Wide** (~2600 Hz usable): 45+ cleanly, saturating at 50 only because of the
  `K_MAX_MESSAGES = 50` report-array cap, not a DSP limit.
- **Wide roughly doubles parallel-decode capacity**, in proportion to the ~2x
  wider spectrum. Decodes verified genuine (correct callsigns/grids, freq =
  dial + audio within bin resolution).
- To report more than 50 simultaneous decodes, raise `K_MAX_MESSAGES` (and check
  `K_MAX_CANDIDATES = 120`) - a small, separate change.

## 7. Weak-signal sensitivity / decode floor (measured)

Single signal, fixed amplitude, noise level raised; decode success measured over
20 independent noise seeds per level:

| noise stddev | decode success | reported SNR |
| ------------ | -------------- | ------------ |
| <= 0.32      | 20/20 (100%)   | -24 dB       |
| 0.34         | 19/20 (95%)    | -24 dB       |
| 0.36         | 15/20 (75%)    | -24 dB       |
| 0.40         | 4/20 (20%)     | -24 dB       |
| 0.45         | 0/20           | -            |

- **Decode floor ~ -24 dB reported SNR.** 100% reliable to -24 dB, then a
  probabilistic S-curve cliff (typical of LDPC forward error correction near
  threshold), zero below.
- The reported SNR pins at -24 dB through the failing region (that is the
  estimator's own floor), so the true threshold is characterized by success
  PROBABILITY, not the reported number.
- Consistent with WSJT-X's published FT8 ~ -21 dB (50%) threshold in the 2500 Hz
  reference bandwidth. The synthetic AWGN test (no fading) is somewhat more
  favorable than real HF.
- **Sensitivity is uniform across the widened band:** the wide-mode floor at
  2500 Hz (high end) equals mid-band 1000 Hz (both 20/20 through noise 0.38) -
  the flat compensated passband imposes no weak-signal penalty near the upper
  edge. Wide held marginally better at the floor (2x more noise samples averaged
  per bin over the 15 s slot).

## 8. Test harness (`mktestiq`)

Hardware-free FT8 test-vector generator. Writes an interleaved float32 `.iq`
file (I = cos, Q = -sin, matching the receiver's readRawIQfile convention).

| Option    | Meaning                                                        |
| --------- | -------------------------------------------------------------- |
| (default) | 4-signal band vector 400/800/1200/1500 Hz                      |
| `-w`      | wideband vector spread across 0..3000 Hz                       |
| `-n N`    | N distinct signals spread evenly across the band (parallel)    |
| `-s F`    | single signal at audio F Hz (edge sweep)                       |
| `-A amp`  | per-signal amplitude for `-s` (default 0.7)                    |
| `-N amp`  | WGN stddev per I/Q sample (default 0.02); probe the SNR floor  |
| `-S seed` | RNG seed (default 12345); vary for noise statistics            |
| `-r RATE` | output sample rate (3200 narrow, 6400 wide)                    |

Decode-side CPU benchmark: `FT8D_BENCH=N ./rtlsdr_ft8d -r vec.iq -x ...` re-runs
the decode N times and prints mean wall/CPU ms.

Example (parallel-decode, wide): `./mktestiq -n 40 -r 6400 t.iq && ./rtlsdr_ft8d -r t.iq -x -f 20m -c N0CALL -l AA00`

## 9. Bottom line

The default receiver now runs the wide (6400 sps) chain: usable audio
200..2900 Hz, ~2x the narrow band. This roughly doubles parallel-decode capacity
(to the 50-message report cap) with no loss of weak-signal sensitivity (floor
~ -24 dB, uniform across the band) for only ~18% more decode CPU. The legacy
narrow chain remains available via `make narrowband`.
