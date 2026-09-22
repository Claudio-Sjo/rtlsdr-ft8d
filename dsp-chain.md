# DSP Chain: derivation and tooling

This document explains how the rtlsdr-ft8d receiver DSP chain is dimensioned,
how every derived constant is computed, how the CIC compensation filter was
designed and re-verified, and exactly what tools are needed to re-derive any of
it when making future adjustments (e.g. a different bandwidth or sample rate).

For measured *results* (bandwidth, CPU, capacity, sensitivity) see
`rx-characterization.md`. For the widening design history see `wideband_plan.md`.

---

## 1. Fixed protocol facts (do not change)

FT8 is defined by the WSJT-X protocol; these are constants of the mode:

| Quantity                 | Value               | Symbol in code               |
| ------------------------ | ------------------- | ---------------------------- |
| Tone spacing             | 6.25 Hz             | `K_FSK_DEV`                  |
| Symbol period            | 0.16 s (= 1 / 6.25) | `FT8_SYMBOL_PERIOD` (ft8_lib)|
| Symbols per transmission | 79                  | `FT8_NN` (ft8_lib)           |
| Slot period              | 15 s                | `SIGNAL_LENGHT`              |

The base FFT bin is always `1 / symbol_period = 6.25 Hz`. Everything else scales
around this. Because `block_size = sample_rate * symbol_period`, the sample rate
must be chosen so this is an integer.

---

## 2. The signal path (rtlsdr_ft8d.cpp `rtlsdr_callback`)

```
RTL 2.4 Msps 8-bit IQ
   |
   v  fs/4 "economic" mixer  (no trig; permute/negate every 4 samples)
      shifts the wanted band to baseband, keeps the upper sideband.
      => RTL is tuned to  realfreq + FS4_RATE  (dial in the fs/4 frame)
   |
   v  CIC decimator  N=2 stages, M=1, ratio R = DOWNSAMPLING
      integrators run at 2.4 Msps; decimate by R; two comb stages
   |
   v  Compensation FIR  (57 taps, zCoef)  at the decimated rate
      flattens the CIC sinc^N passband droop
   |
   v  scale by 1 / (32768 * DOWNSAMPLING)   -> float baseband
   |
   v  stored in iSamples/qSamples at SIGNAL_SAMPLE_RATE
```

Downstream (`ft8_subsystem`): STFT over the 15 s buffer -> waterfall magnitude
(`mag_power`) -> ft8_lib `ftx_find_candidates` / `decode` -> reported
`freq_hz = dial + audio`.

---

## 3. How the sample rate sets everything (rtlsdr_ft8d.h)

The **only two free choices** are the RTL input rate (fixed at 2.4 Msps) and the
decimated output rate `SIGNAL_SAMPLE_RATE`. Everything else is derived:

```
DOWNSAMPLING  = SAMPLING_RATE / SIGNAL_SAMPLE_RATE      ; CIC ratio R
NUM_BIN       = SIGNAL_SAMPLE_RATE / (2 * 6.25)         ; bins across half-band
BLOCK_SIZE    = SIGNAL_SAMPLE_RATE / 6.25               ; samples per FT8 symbol
SUB_BLOCK_SIZE= BLOCK_SIZE / K_TIME_OSR
NFFT          = BLOCK_SIZE * K_FREQ_OSR
NUM_BLOCKS    = ((SIGNAL_LENGHT*SIGNAL_SAMPLE_RATE) - NFFT + SUB_BLOCK_SIZE) / BLOCK_SIZE
MAG_ARRAY     = NUM_BLOCKS * K_FREQ_OSR * K_TIME_OSR * NUM_BIN
```

with `K_FREQ_OSR = K_TIME_OSR = 2` (oversampling factors).

Concrete values for the two supported rates:

| Constant                   | Narrow (3200) | Wide default (6400) |
| -------------------------- | ------------: | ------------------: |
| DOWNSAMPLING (R)           |           750 |                 375 |
| NUM_BIN                    |           256 |                 512 |
| BLOCK_SIZE                 |           512 |                1024 |
| NFFT                       |          1024 |                2048 |
| NUM_BLOCKS                 |            92 |                  92 |
| MAG_ARRAY                  |         94208 |              188416 |
| Bin ceiling (NUM_BIN*6.25) |       1600 Hz |             3200 Hz |

**Hard rule for a new rate:** `SAMPLING_RATE / SIGNAL_SAMPLE_RATE` must be an
integer (integer CIC ratio) AND `SIGNAL_SAMPLE_RATE * 0.16` must be an integer
(integer samples/symbol). Both hold for 3200 and 6400 (R = 750 / 375,
samples/symbol = 512 / 1024). Example bad choice: 4800 sps gives R = 500 (ok)
but is otherwise fine too - just check both conditions before adding a rate.

The usable audio band is then `RX_AUDIO_MIN .. RX_AUDIO_MAX`, kept below the bin
ceiling and below the FIR design edge (see below). It is the single source of
truth for the monitor config, the TX audio slots and the UI readout.

### Build selection

Wide (6400) is the default. Narrow is a compile-time fallback:

```
make               # wide default (6400 sps, 200..2900 Hz)
make narrowband    # legacy narrow (3200 sps, 200..1500 Hz), -DNARROWBAND
```

The switch is `#ifdef NARROWBAND` in rtlsdr_ft8d.h. Because the sizes are
compile-time `#define`s backing statically- and heap-allocated buffers, the rate
is a build-time choice, not a runtime flag. (A runtime flag would require making
all the sizes runtime variables with dynamic allocation - see wideband_plan.md
"Strategy B".)

---

## 4. The CIC compensation FIR: how it was designed

### 4.1 What it is

An N=2 CIC decimator has a `sinc^N` passband droop that attenuates higher
in-band frequencies. The 57-tap `zCoef` FIR (in `rtlsdr_callback`) is the
inverse of that droop over the passband, tapering to zero in the stopband, so
the cascade CIC x FIR is flat across the usable band.

### 4.2 The design algorithm (WestCoastDSP)

Source of the original coefficients:
`https://github.com/WestCoastDSP/CIC_Octave_Matlab` (`cic.m`). The core design
is an inverse-droop target fed to `fir2` (frequency-sampling FIR design):

```octave
% Parameters: R (CIC ratio), N (stages), M (differential delay=1),
%             L (filter order, even -> L+1 taps), Fo (normalized cutoff, 0<Fo<=0.5/M)
p  = 2e3;  s = 0.25/p;
fp = 0:s:Fo;              % passband frequency samples (normalized, 1 = Nyquist)
fs = (Fo+s):s:1;          % stopband
f  = [fp fs];  f(end) = 1;
Mp = ones(1,length(fp));
Mp(2:end) = abs( M*R*sin(pi*fp(2:end)/R) ./ sin(pi*M*fp(2:end)) ).^N;  % inverse CIC droop
Mf = [Mp zeros(1,length(fs))];
h  = fir2(L, f, Mf);      % L+1 taps, linear phase
h  = h / max(h);          % floating-point, unit peak
```

Project parameters: **R=750, N=2, M=1, L=56, Fo=0.92** -> 57 taps. In the code
the taps are stored scaled so the centre tap = 0.5 (peak 0.5 convention).

### 4.3 Why the same taps work for both 3200 and 6400 sps

The compensator is designed in **normalized** frequency (fraction of output
Nyquist), so its shape covers 0..(Fo x Nyquist): 0..1472 Hz at 3200 sps and
0..2944 Hz at 6400 sps automatically. Moreover, for large R the droop term
`M*R*sin(pi*f/R)/sin(pi*M*f)` is nearly R-independent (`sin(pi*f/R) ~ pi*f/R`),
so the R=375 taps are essentially identical to the R=750 taps.

**Verified numerically:** regenerating with R=375 vs R=750 gives
`max|h375 - h750| = 1.5e-6`. Hence `zCoef` is reused unchanged for the wide
default. (Reproducing the in-tree R=750 taps from the algorithm above also
matched to ~7 significant figures, which validated the whole method.)

### 4.4 When you WOULD need to regenerate the FIR

- Changing the number of CIC stages N or the differential delay M.
- A drastically smaller R (small ratios make the droop R-dependent).
- Wanting a different passband edge Fo or more/steeper taps (change L).

If you regenerate, re-verify the passband is flat by MEASUREMENT (Section 6),
not by hand - analytic cascade-flatness modelling proved unreliable during this
work and should not be trusted for sign-off.

### 4.5 CIC gain scaling

The output is divided by `32768.0 * DOWNSAMPLING`. The `DOWNSAMPLING` term tracks
the CIC integrator gain, so it follows R automatically; no manual change needed
when switching rate.

---

## 5. Tools needed for future adjustments

| Task                             | Tool                                                        | Notes                                                     |
| -------------------------------- | ----------------------------------------------------------- | --------------------------------------------------------- |
| Regenerate CIC compensation FIR  | GNU Octave + `signal` package                               | `fir2` lives in the signal pkg. Alternatively MATLAB.     |
| (Octave signal pkg build)        | `liboctave-dev` (for `mkoctfile`) + `control` pkg (dep)     | `pkg install -forge control; pkg install -forge signal`   |
| Quick numeric checks / plots     | Python 3 + numpy + scipy                                    | scipy `firls`/`firwin2` can cross-check `fir2`.           |
| Build                            | clang/clang++ (or gcc), FFTW3 (`-lfftw3f`), ncurses, libusb, librtlsdr | Makefile picks `-Dx86` or the Pi `-DRPI*`/`-mcpu`. |
| Hardware-free verification       | `mktestiq` (in-tree) + `rtlsdr_ft8d -r file -x`             | No RTL/radio needed.                                      |
| Decode CPU timing                | `FT8D_BENCH=N` env var                                      | Re-runs the decode N times, prints mean wall/CPU ms.      |

### Installing the Octave signal package (Debian/Ubuntu/WSL)

```
sudo apt-get install -y liboctave-dev
octave --no-gui --eval "pkg install -forge control; pkg install -forge signal; pkg load signal"
```

### Regenerating the FIR for new parameters (example R=375)

Put the algorithm from Section 4.2 in a `.m` file, then:

```
octave --no-gui gen_comp.m 375 2 1 56 0.92     # R N M L Fo
```

Emit the taps as a C initializer and paste into `zCoef` in rtlsdr_ft8d.cpp
(keep `FIR_TAPS` = taps - 1). Then re-verify by measurement.

---

## 6. How to verify after any DSP change (measurement, not theory)

All hardware-free, using the in-tree `mktestiq` generator (see
`rx-characterization.md` Section 8 for the full option list):

1. **Frequency mapping / edge sweep** - single tone across frequency:
   ```
   for f in <range>; do
     ./mktestiq -s $f -r <RATE> t.iq
     ./rtlsdr_ft8d -r t.iq -x -f 20m -c N0CALL -l AA00 | grep K1JT
   done
   ```
   Confirms reported RF = dial + audio and finds the usable edge (should stop at
   the NUM_BIN*6.25 bin ceiling).

2. **Passband flatness** - the sweep's SNR should stay roughly constant across
   the band; a sagging or peaking SNR vs frequency means the FIR/CIC cascade is
   not flat (redesign the FIR).

3. **Parallel-decode capacity** - `mktestiq -n N -r <RATE>`; count decodes vs N.

4. **Sensitivity floor** - `mktestiq -s F -A <sig> -N <noise> -S <seed> -r <RATE>`
   over many seeds; find the noise level where decode probability falls.

5. **CPU cost** - `FT8D_BENCH=300 ./rtlsdr_ft8d -r t.iq -x ...`; compare mean
   decode ms. On the Pi, confirm it fits inside the 15 s slot.

6. **Regression** - the default vector must still decode:
   `./mktestiq t.iq && ./rtlsdr_ft8d -r t.iq -x -f 20m -c N0CALL -l AA00`
   and the self-test must pass: `./rtlsdr_ft8d -t -x`.

---

## 7. Change checklist (adding a new bandwidth/rate)

1. Verify `SAMPLING_RATE / newRate` is an integer AND `newRate * 0.16` is an
   integer.
2. Add the rate under the `#ifdef` in rtlsdr_ft8d.h (or add a new `-D` switch +
   Makefile target). All derived sizes recompute automatically.
3. Set `RX_AUDIO_MAX` below the new bin ceiling (`NUM_BIN * 6.25`) and below the
   FIR edge (`Fo * Nyquist`).
4. If N/M/Fo/L or R changed enough to matter, regenerate `zCoef` (Section 4-5).
5. Check heap buffers: `mag_power`/`mag_db` are already heap-allocated; confirm
   any fixed scratch (e.g. `noiseFloorPower`'s `samples[8192]`) still suffices.
6. Run the full verification suite (Section 6), including the Pi CPU budget.
7. Bump `RTLSDR_FT8D_VERSION` and update CHANGELOG + rx-characterization.md.
