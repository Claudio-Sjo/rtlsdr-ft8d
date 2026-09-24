# TX frequency synthesis investigation (~1200 ppm anomaly)

Code-only analysis of why `calibrate` measured a ~1200 ppm deviation between the
Pi FT8 transmitter and the RTL-SDR reference. A ~1200 ppm (0.12%) error is far
too large to be the Pi crystal (spec ~+/-50 ppm, typically <= 2.5 ppm), so the
number must come from a systematic offset elsewhere, not the oscillator.

## How the TX frequency is synthesized (ft8.cpp)

- `F_PLLD_CLK` is the assumed PLLD clock, chosen at BUILD time by the RPi macro:
  - `RPI4`  -> 750000000.0
  - `RPI23` -> 500000000.0
  - `RPI1`  -> 500000000.0 * (1 - 2.5e-6)
- GPCLK0 uses a 12-bit FRACTIONAL divider. `setupDMATab()` computes
  `div = plld / f_target` truncated to 1/4096, and `txSym()` delta-sigma dithers
  between the two dividers bracketing the target so the long-term average
  frequency equals the target.
- ppm correction is folded into the clock: `F_PLLD_CLK * (1 - ppm/1e6)` -> a
  PROPORTIONAL shift of the synthesized frequency.
- `self_cal` is OFF by default (NTP `freq` wander used to move the TX every run;
  now a fixed `-p` ppm is used, deterministic).

## What each stage can contribute (quantified)

| Source | Magnitude | Explains 1200 ppm? |
| ------ | --------- | ------------------ |
| Crystal error | <= ~2.5 ppm | No (500x too small) |
| 12-bit divider quantization @ 14 MHz (D~35.7) | ~7 ppm | No |
| 12-bit divider quantization @ 144 MHz (D~3.47) | ~70 ppm | No |
| delta-sigma dither (avg unbiased; PWM clock only sets time ratio) | ~0 bias | No |
| Wrong RPi build macro: RPI23 (500 MHz) binary on a Pi4 (750 MHz PLLD) | ~500000 ppm (1.5x) | No (way too big) |
| ppm applied with wrong sign/value | proportional | Possibly, but see below |

So none of the *synthesis* mechanisms alone produce ~1200 ppm. That points to
the MEASUREMENT or a fixed-offset artifact.

## The calibrate measurement geometry (calibrate.cpp)

- RTL sample rate 2.4 Msps, FFT 2^20 -> 2.289 Hz/bin.
- RTL tuned 100 kHz BELOW the tone: `centerFreq = testFreq - 100000`.
- Tone expected at +100 kHz in the FFT. `measuredFreq = centerFreq + offsetHz`,
  `error = measuredFreq - testFreq = offsetHz - 100000`. Self-consistent: a tone
  truly at `testFreq` gives `offset = +100000`, error 0.
- Peak search scans the whole spectrum (skipping +/-2 kHz DC guard), then takes a
  power-weighted centroid over +/-3 kHz around the peak.

Key point: the geometry is arithmetically correct, so a 1200 ppm result means
the strongest carrier really landed ~1200 ppm from where commanded. What 1200
ppm is in Hz:

| Test freq | 1200 ppm error |
| --------- | -------------- |
| 14.074 MHz | ~16.9 kHz |
| 50.313 MHz | ~60.4 kHz |
| 144.174 MHz | ~173.0 kHz |
| 432.065 MHz | ~518.5 kHz |

Note a suggestive coincidence: mishandling the fixed 100 kHz tune offset would
LOOK like a frequency-dependent ppm (100000/f):

| Test freq | apparent ppm if the 100 kHz offset were mishandled |
| --------- | --- |
| 14.074 MHz | ~7105 ppm |
| 50.313 MHz | ~1988 ppm |
| 144.174 MHz | ~694 ppm |

i.e. a FIXED Hz offset around ~100-170 kHz reads as ~700-2000 ppm depending on
band. This is the classic signature of a fixed offset misreported as ppm.

## Most likely causes (ranked)

1. **Wrong RPi build macro for the actual board.** `F_PLLD_CLK` is compile-time.
   A binary built `-DRPI23` (500 MHz) running on a Pi4 (PLLD 750 MHz), or vice
   versa, mis-synthesizes by a large proportional factor. A full 500/750
   mismatch is 1.5x (way beyond 1200 ppm), but if the *actual* PLLD on the board
   differs from the assumed constant by a smaller amount (firmware/`force_turbo`,
   a different PLLD base, or a Pi variant), a ~0.12% proportional error is
   plausible. THIS IS THE FIRST THING TO CHECK.
2. **A spur / delta-sigma image being picked as the peak.** `txSym` spreads spurs
   by randomising the iteration period; if a spur near +/-tens-of-kHz is stronger
   than the main carrier in that capture (or the +/-3 kHz centroid straddles an
   asymmetric spur cluster), the centroid is pulled off. This would be somewhat
   RUN-TO-RUN VARIABLE, not perfectly repeatable.
3. **RTL front-end offset not zeroed.** `calibrate` never calls
   `rtlsdr_set_freq_correction(dev, 0)`, so a stored dongle ppm is folded in --
   but that is tens of ppm at most, not 1200. Minor contributor only.
4. **A fixed IF/image/alias artifact** from the R820T/R828 tuner landing within
   the search band and being taken as the carrier.

## The decisive test (run on the Pi)

Measure at TWO widely separated frequencies, e.g. 50.313 MHz (6 m) and
144.174 MHz (2 m), with a dry run so nothing is written:

```
calibrate -f 50313000  -d
calibrate -f 144174000 -d
```

Interpretation:
- **Same ppm at both** -> proportional error -> the PLLD constant is wrong
  (wrong RPi macro or actual PLLD != assumed). Fix `F_PLLD_CLK` / the build
  target. A genuine crystal can't be 1200 ppm, so a constant 1200 ppm = wrong
  clock constant.
- **Same Hz at both (ppm differs)** -> a fixed offset (tune-offset handling,
  IF/image, DC) -> a measurement/geometry bug, not a TX error. Expressing it as
  ppm is meaningless.
- **Neither constant / jumps between runs** -> a spur is being selected -> a
  measurement robustness problem in the centroid/peak logic.

Also worth capturing while testing:
- Confirm which `-DRPI*` the running `ft8` was built with vs the actual board.
- Run `calibrate` twice at the same freq to see if the value is repeatable
  (repeatable -> deterministic synthesis/constant error; variable -> spur/noise).
- Optionally widen/narrow the centroid window and re-measure; a value that moves
  with the window is a spur artifact.

## Bottom line

The crystal is almost certainly fine (~2.5 ppm). A ~1200 ppm reading is a
systematic artifact -- most probably a wrong/mismatched PLLD clock constant
(compile-time `F_PLLD_CLK` vs the board's real PLLD) and/or the calibrate
measurement locking onto a spur or a fixed offset that is then misreported as
ppm. The two-frequency dry-run test above tells us which, definitively.

Status: analysis only, no code changed. Awaiting on-Pi measurement.
