# Plan: Selectable Receiver Bandwidth (up to WSJT-X ~3000 Hz)

## Goal

Add the ability to widen the receiver's usable audio passband from the current
200-1500 Hz up to WSJT-X's full ~3000 Hz, so this receiver can decode FT8
signals placed anywhere in the standard 0-3000 Hz USB audio window (not just the
lower ~1300 Hz).

Current state (v0.8.7): `mon_cfg.f_max` is capped at 1500 Hz because the whole
DSP chain produces a 3200 sps complex baseband stream (Nyquist 1600 Hz) and the
FT8 waterfall has `NUM_BIN = 256` bins x 6.25 Hz = 1600 Hz ceiling. The RTL is
tuned to `realfreq + FS4_RATE` (dial at audio 0, USB convention
`f_RF = f_dial + f_audio`).

---

## Key finding: the FT8 decoder scales cleanly

The ft8_lib decoder (`libft8/`) needs **no changes**. It derives all of its
geometry from `sample_rate x symbol_period`, and the FT8 symbol period (0.16 s)
is fixed by the protocol. The base FFT bin is always
`1 / symbol_period = 6.25 Hz` (the FT8 tone spacing). Raising the sample rate
simply produces more bins and extends the Nyquist ceiling upward, keeping the
6.25 Hz base bin intact.

### Target sample rate: 6400 sps

To represent ~3000 Hz we need Nyquist > 3000, so **6400 sps** (Nyquist 3200 Hz).

- RTL rate `2,400,000 / 6400 = 375` -> integer decimation ratio. So the RTL
  input rate (2.4 Msps), the fs/4 mixer, and the tuning offset
  (`realfreq + FS4_RATE`, FS4_RATE = SAMPLING_RATE/4) all stay valid. Only the
  decimation ratio R changes 750 -> 375.
- `block_size = 6400 x 0.16 = 1024` samples/symbol (integer, fine).
- `NUM_BIN` doubles 256 -> 512, extending the ceiling 1600 -> 3200 Hz.

Even at 6400 sps the *usable flat* region will still be inside Nyquist (RTL
front-end + CIC droop), so the new FIR should be designed for perhaps
~2800-2900 Hz, and the real edge must be **measured** with the mktestiq sweep,
not assumed.

---

## The two pieces of work

### Piece 1 - The last low-pass (CIC compensation FIR)

**RESOLVED (Phase 1): no coefficient change is needed for 6400 sps.**

The original `zCoef` was reproduced exactly from the authoritative
WestCoastDSP/CIC_Octave_Matlab algorithm (`cic.m`): an inverse-CIC-droop
passband target `abs(M*R*sin(pi*f/R)/sin(pi*M*f))^N` fed to `fir2(L, f, Mf)`,
normalized to unity peak. Parameters R=750, N=2, M=1, L=56 (-> 57 taps),
Fo=0.92 regenerate the in-tree coefficients to ~7 significant figures. This
validated the method.

Regenerating for **R=375** (the 6400 sps ratio) gives coefficients essentially
identical to the R=750 set: `max|h375 - h750| = 1.5e-6`. The reason is that the
compensation filter is designed in *normalized* frequency (fraction of output
Nyquist), and for large decimation ratios the CIC droop over the normalized
passband is nearly R-independent (`sin(pi*f/R) ~ pi*f/R`). So the SAME taps that
compensate 0..1472 Hz at 3200 sps also compensate 0..2944 Hz at 6400 sps
(Fo=0.92 x 3200 Hz Nyquist = 2944 Hz).

**Consequence:** the existing `zCoef` can be reused unchanged at 6400 sps. No FIR
table, no runtime coefficient synthesis. The only filter-related change is the
CIC gain scale (`32768.0 * DOWNSAMPLING`), which already tracks DOWNSAMPLING.

Generator (validated), for reference/reproducibility (needs Octave signal pkg):
```octave
p=2e3; s=0.25/p; fp=[0:s:Fo]; fs=(Fo+s):s:1; f=[fp fs];
Mp=ones(1,length(fp));
Mp(2:end)=abs(M*R*sin(pi*fp(2:end)/R)./sin(pi*M*fp(2:end))).^N;
Mf=[Mp zeros(1,length(fs))]; f(end)=1;
h=fir2(L,f,Mf); h=h/max(h);   % R=750,N=2,M=1,L=56,Fo=0.92 -> in-tree zCoef
```

NOTE: analytic cascade-flatness modeling of FIR x CIC was unreliable in review;
the actual passband flatness at 6400 sps MUST be confirmed by the Phase 3
`mktestiq -w -r 6400` sweep, not by hand calculation.

### Piece 2 - Bin adaptation (the bulk of the effort)

Every sizing constant is a **compile-time `#define`** that statically sizes
global/stack buffers. A runtime parameter cannot just flip a value; the buffers
must become dynamically allocated.

| Item                            | Location             | Current             | Change for 6400 sps                        |
| ------------------------------- | -------------------- | ------------------- | ------------------------------------------ |
| `SAMPLING_RATE`                 | rtlsdr_ft8d.h        | 2400000             | keep (2.4M / 6400 = 375)                   |
| `DOWNSAMPLING` / R              | rtlsdr_ft8d.h        | 750                 | 375 (runtime var)                          |
| `SIGNAL_SAMPLE_RATE`            | rtlsdr_ft8d.h        | 3200                | 6400 (runtime var)                         |
| CIC gain scale                  | rtlsdr_ft8d.cpp ~245 | `32768 * 750`       | track R                                    |
| `zCoef` (FIR)                   | rtlsdr_ft8d.cpp ~143 | R=750 const array   | reuse unchanged (R=375 taps identical)     |
| `NUM_BIN`                       | rtlsdr_ft8d.h        | 256                 | 512 (runtime)                              |
| `BLOCK_SIZE`/`SUB`/`NFFT`       | rtlsdr_ft8d.h        | 512/256/1024        | 1024/512/2048 (runtime)                    |
| `NUM_BLOCKS`/`MAG_ARRAY`        | rtlsdr_ft8d.h        | 92/94208            | ~92/188416 (runtime)                       |
| `iSamples`/`qSamples`           | rtlsdr_ft8d.h ~117   | static `[2][96000]` | heap alloc                                 |
| `mag_power`                     | rtlsdr_ft8d.cpp ~2183| ~92 KB on stack     | heap (mandatory)                           |
| `mag_db`                        | rtlsdr_ft8d.cpp ~2192| `[NFFT]` stack      | heap                                       |
| FFTW buffers / `hann`           | rtlsdr_ft8d.cpp ~450 | `NFFT`              | already runtime alloc (OK)                 |
| `mon_cfg.f_max`/`.sample_rate`  | rtlsdr_ft8d.cpp ~2247| 1500/3200           | runtime, keep `max_bin <= NUM_BIN`         |
| ft8_lib decode/monitor          | libft8/              | general             | no change                                  |
| `noiseFloorPower` scratch       | rtlsdr_ft8d.cpp ~1130| `static float[8192]`| verify still big enough                    |

---

## Two implementation strategies

### Strategy A - Compile-time `WIDEBAND` build variant (LOW effort, ~hours)

A build option that redefines `SIGNAL_SAMPLE_RATE` to 6400 and `DOWNSAMPLING`
to 375. Every other constant recomputes from the macros automatically. The
compensation FIR (`zCoef`) is REUSED UNCHANGED (Phase 1 showed R=375 taps equal
R=750). No dynamic allocation, no stack-overflow risk beyond checking the larger
`mag_power`/`mag_db`. Cost: two build variants instead of one runtime flag.

### Strategy B - Runtime `--bandwidth` / `--wide` flag (MODERATE effort, ~1-2 days)

A true runtime flag: convert the ~7 sizing macros to runtime variables,
heap-allocate the 4 buffer groups (`iSamples`, `qSamples`, `mag_power`,
`mag_db`), add the FIR coefficient table, plumb the flag through
`receiver_options`, and re-verify. Cleaner UX; this is where the real effort and
regression risk lives (dynamic-buffer conversion).

**Recommendation:** Start with Strategy A (safe, fast, proves the DSP works
end-to-end at 6400 sps and validates the new FIR). Promote to Strategy B later
if a runtime flag is desired, reusing the proven wideband constants and FIR.

---

## Phased steps

- [x] **Phase 0 - Baseline & measurement harness.** DONE. `mktestiq` extended
      with `-s F` sweep, `-w` wideband vector, `-r RATE` (heap buffers). Baseline
      measured: hard edge at 1600 Hz (bin ceiling), 1550 Hz decodes at +19 dB.
      See "Phase 0 results" below. No regression in default/self-test vectors.
- [x] **Phase 1 - CIC compensation FIR.** DONE / RESOLVED. Reproduced the
      original R=750 taps exactly from the WestCoastDSP `fir2` algorithm
      (method validated), then found R=375 taps are identical to R=750
      (max diff 1.5e-6) because the filter is designed in normalized frequency.
      **The existing `zCoef` is reused unchanged at 6400 sps** - no new table.
      See "Piece 1" above.
- [x] **Phase 2 - Strategy A (compile-time WIDEBAND).** DONE, and WIDE IS NOW
      THE DEFAULT build. `SIGNAL_SAMPLE_RATE` 6400, `DOWNSAMPLING` 375,
      `RX_AUDIO_MAX` 2900; `zCoef` reused unchanged; `mag_power`/`mag_db` moved
      to heap. Legacy narrow chain available via `make narrowband`
      (-DNARROWBAND). Both build clean (-Wall -Wextra, x86). Results consolidated
      in rx-characterization.md.
- [x] **Phase 3 - Verify.** DONE (x86). Wideband build decodes all 6 wideband
      test signals (incl. 1800/2300/2800 Hz, impossible before) at correct
      dial+audio. Edge sweep: decodes cleanly through 3100 Hz (stops only at the
      3200 Hz Nyquist/bin ceiling); the RX_AUDIO_MAX=2900 cap is conservative.
      CPU comparison (decode work per 15 s slot, identical 4-signal content):
      narrow 26.4 ms vs wideband 31.1 ms = **1.18x** (only +18% despite 2x
      sample rate/NFFT/bins, because LDPC+Costas cost is content-driven, not
      FFT-size-driven). STILL TODO: benchmark on the actual RPi 2/3 to confirm
      the wideband decode fits the 15 s slot budget; update CHANGELOG + version.
- [ ] **Phase 4 (optional) - Strategy B (runtime flag).** Convert sizing macros
      to runtime variables, heap-allocate the buffer groups, plumb a
      `--bandwidth`/`--wide` flag through `receiver_options`, keep the narrow
      mode as default for compatibility, re-verify.

---

## Phase 3c results (decode sensitivity / SNR floor, x86)

`mktestiq` gained `-A amp` (signal amplitude), `-N amp` (WGN stddev) and
`-S seed` (RNG seed) so the decode SNR floor can be probed statistically.
Single signal (CQ K1JT FN20), signal amp 0.05, noise raised, 20 seeds/level:

| noise stddev | decode success | reported SNR |
| ------------ | -------------- | ------------ |
| <= 0.32      | 20/20 (100%)   | -24 dB       |
| 0.34         | 19/20 (95%)    | -24 dB       |
| 0.36         | 15/20 (75%)    | -24 dB       |
| 0.40         | 4/20 (20%)     | -24 dB       |
| 0.45         | 0/20           | -            |

Findings:
- **Decode floor ~ -24 dB reported SNR.** 100% reliable to -24 dB, then a
  probabilistic S-curve cliff (typical of LDPC/FEC near threshold), zero below.
- Reported SNR pins at -24 dB through the failing region: that is also the
  estimator's floor, so success PROBABILITY (not the reported number)
  characterizes the true threshold.
- Consistent with WSJT-X's published FT8 ~ -21 dB (50%) threshold in the
  2500 Hz reference bandwidth; the synthetic AWGN test (no fading) is a bit more
  favorable than real HF, and the estimator calibration accounts for the rest.
- **Sensitivity is uniform across the band:** wideband floor at 2500 Hz (high
  end) equals mid-band 1000 Hz (both 20/20 through noise 0.38) - the flat
  compensated passband gives no weak-signal penalty near the upper edge.
  Wideband held marginally better at the floor (2x noise samples averaged per
  bin over the 15 s slot).

## Phase 3b results (parallel-decode stress test, x86)

`mktestiq -n N` added: generates N distinct-callsign signals spread evenly
across the usable band (edge follows -r rate). Equal amplitude, so per-signal
SNR falls as N rises (shared power budget). Decoded-count comparison:

| N     | narrow decoded (spacing) | wideband decoded (spacing) |
| ----- | ------------------------ | -------------------------- |
| 5..25 | all                      | all                        |
| 28    | 28 (44 Hz)               | -                          |
| 30    | 29 (41 Hz)               | 30 (90 Hz)                 |
| 40    | 26 (31 Hz)               | 40 (67 Hz)                 |
| 45    | -                        | 45 (59 Hz)                 |
| 50    | 2 (24 Hz, jammed)        | 50 (53 Hz)                 |
| 55/60 | -                        | 50 (cap)                   |

Findings:
- Controlling variable is per-signal spacing vs the ~50 Hz FT8 signal width.
- **Narrow** (200..1500, ~1250 Hz usable): ~28-29 parallel signals before
  overlap degrades it; collapses when packed below ~50 Hz spacing.
- **Wideband** (200..2900, ~2600 Hz usable): 45+ cleanly; saturates at 50 only
  because of `K_MAX_MESSAGES=50` (report array), not the DSP.
- Wideband ~doubles parallel-decode capacity, in proportion to the ~2x wider
  spectrum. Decodes verified genuine (correct callsigns/grids, freq = dial+audio
  within bin resolution; SNR ~-8 dB at N=40 as expected for equal-power stacking).
- If >50 simultaneous decodes are ever wanted, raise K_MAX_MESSAGES (and check
  K_MAX_CANDIDATES=120) - a separate, small change.

## Phase 0 results (measured baseline, v0.8.7, narrow build)

`mktestiq` extended with `-s F` (single-tone sweep), `-w` (wideband 0..3000 Hz
vector) and `-r RATE` (output sample rate). Buffers are now heap-allocated so
the tool can emit 6400 sps vectors too. Default and self-test vectors unchanged
(no regression: default vector still decodes 400/800/1200/1500 -> dial+audio).

Single-tone sweep (CQ K1JT FN20, dial 20 m = 14074000), current 3200 sps chain:

| audio Hz   | decoded | reported RF | SNR |
| ---------- | ------- | ----------- | --- |
| 1300       | YES     | 14075278    | +18 |
| 1350       | YES     | 14075328    | +17 |
| 1400       | YES     | 14075378    | +18 |
| 1450       | YES     | 14075428    | +17 |
| 1472       | YES     | 14075450    | +18 |
| 1500       | YES     | 14075478    | +18 |
| 1550       | YES     | 14075528    | +19 |
| 1600       | no      | -           | -   |
| 1650..2000 | no      | -           | -   |

**Findings:**
- Hard cutoff at exactly **1600 Hz** = `NUM_BIN(256) x 6.25 Hz`. This is the FFT
  bin ceiling, NOT the FIR roll-off: 1550 Hz still decodes strongly (+19 dB).
- Confirms the plan's premise: **raising NUM_BIN (via sample rate) is what
  unlocks the band.** The FIR redesign is about keeping the passband flat up to
  the new edge, not about moving this hard wall.
- The current `f_max = 1500` cap is slightly conservative vs the measured 1550
  edge; that margin is fine and intentional (stays clear of the wall).
- Reported RF tracks dial + audio exactly across the whole range -> frequency
  mapping (v0.8.7) is correct.

Re-run this sweep after Phase 2 to measure the widened edge (expect signals to
keep decoding well past 1600 Hz toward ~2800-2900 Hz at 6400 sps).

Sweep command used:
```
for f in 1300 1350 1400 1450 1472 1500 1550 1600 1650 1700 1800 1900 2000; do
  ./mktestiq -s $f /tmp/sweep.iq
  ./rtlsdr_ft8d -r /tmp/sweep.iq -x -f 20m -c N0CALL -l AA00 | grep K1JT
done
```

---

## Constraints / notes

- Benchmark harness: `decodeRecordedFile` honors `FT8D_BENCH=N` to re-run
  `ft8_subsystem` N times and print mean wall/CPU ms (stdout, "BENCH:" line).
  Env-gated, once per file decode, negligible overhead when unset. Kept for the
  pending on-Pi measurement. Example:
  `FT8D_BENCH=300 ./rtlsdr_ft8d -r vec.iq -x -f 20m -c N0CALL -l AA00`

- TX audio window must track the RX passband (`qsoHandler.cpp` TX_AUDIO_MIN/MAX,
  `ft8_ncurses.cpp` default display freq) so we only transmit where we can also
  receive. Currently 300-1400 Hz for the 200-1500 passband; widen in step.
- FS4_RATE and the RTL tuning offset stay valid (SAMPLING_RATE unchanged).
- ARM/RPi is the primary target: watch the extra CPU (2x FFT size, 2x FIR work
  per second) and memory (buffers ~2x) on the Pi. Benchmark on the Pi before
  declaring done.
- Do not fabricate the wideband usable edge; measure it (Phase 0/3 harness).
- All git operations are performed by the user.
