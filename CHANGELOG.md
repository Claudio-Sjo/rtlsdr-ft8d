## CHANGELOG

### 0.8.5

- **Corrected FT8 frequency convention (USB, `RF = dial + audio`).** Per
  ft8spec.md, FT8 is upper sideband: the published band frequency is the USB
  dial and an individual signal may sit anywhere in WSJT-X's ~0..3000 Hz audio
  passband (`f_RF = f_dial + f_audio`; e.g. 20 m dial 14.074.000 + 1500 Hz audio
  = 14.075.500). Both ends now use this single convention: the receiver reports
  decoded signals as `dial + audio`, and CQ transmissions pick a random audio
  slot within the usable passband (300..2700 Hz), like a normal WSJT-X station.
  QSO replies transmit on the peer's reported (absolute) frequency. Earlier code
  variously added or subtracted a fixed 1500 Hz and, at one point, confined
  transmit to a too-narrow band; frequencies shown in the UI, logged to ADIF and
  sent to PSKReporter now match the true on-air frequency.
- **Transmit frequency authority moved to the receiver.** Previously the `ft8`
  transmitter silently altered the requested frequency: it added a fixed
  `FT8_TXOFS` (1250 Hz) offset and, with `-o`, a random +/-1000 Hz, so the
  actual on-air frequency was decided inside `ft8` and was unknown to the
  receiver -- the UI only showed the requested/centre value. Now `rtlsdr_ft8d`
  computes the exact absolute transmit frequency and passes it over the socket,
  and `ft8` transmits it verbatim (no added offset, no randomization for
  socket-driven transmissions). QSO replies answer on the peer's frequency
  (`currentQSO.freq`); CQ transmissions use dial + 1500 Hz +/- a random spread
  chosen on the receiver side (fixing an earlier bug where replies used the
  dial frequency without the 1500 Hz audio offset). The frequency shown in the
  UI, written to ADIF and sent to PSKReporter is therefore the true
  transmitted frequency. The frequency randomizer thus lives in the receiver.
- **`calibrate` program: align the transmitter to the RTL-SDR TCXO.** New tool
  that measures the Pi crystal's ppm error using the receiver as the frequency
  reference: it commands `ft8` to emit an uncorrected CW test tone, receives it
  through the (1 ppm TCXO) RTL-SDR, finds the carrier, and computes the ppm
  error. Because the ft8 "test tone" is delta-sigma dithered (it alternates
  between two divider frequencies with randomised timing so only its long-term
  average equals the target), the measurement uses the power-weighted spectral
  centroid over the carrier region -- not the single strongest FFT bin -- to
  recover the true average frequency. The result is averaged over several
  captures and written to a calibration file (`~/.config/rtlsdr-ft8d/txcal`).
- **`ft8` reads the stored calibration.** Frequency-correction precedence is
  now: explicit `-p ppm` > stored calibration file > 0 ppm (with a warning that
  the transmitter is uncalibrated). This replaces NTP self-cal as the default
  while `--self-cal` remains available. Self-cal was dropped as the default
  because `ntp_adjtime()` reports the kernel's software timekeeping correction,
  which is unrelated to the PLLD RF error and wandered run-to-run despite a
  stable oscillator; the calibration file gives a fixed, measured, repeatable
  correction tied to the receiver's TCXO instead.

### 0.8.4

- **Unified version numbering.** The transmitter (`ft8`) previously carried its
  own independent version string ("v 0.4"); it now shares a single
  `RTLSDR_FT8D_VERSION` macro (in `rtlsdr_ft8d.h`) with the receiver, so both
  binaries report the same project version and future bumps happen in one place.
- **Fixed reception frequency reported ~200 Hz too high.** The decoded-frequency
  formula added `mon.min_bin` to the candidate bin index, but `ft8_subsystem()`
  fills its waterfall starting at FFT bin 0 (not at `min_bin`, as stock ft8_lib's
  `monitor_process()` does). This double-counted `min_bin / symbol_period` and
  shifted every reported frequency high by ~200 Hz. The term is now dropped so the
  reported audio frequency matches the true signal. Verified with an internal
  known-frequency calibration (offset removed to the Hz) and an RF loopback against
  an exactly-known transmit frequency (round-trip error reduced from ~259 Hz to a
  ~56 Hz residual, i.e. a few ppm of combined oscillator/tuning error, not a bug).
- **Fixed spurious "Cannot open device" and self-termination during reception.**
  The RX stall watchdog re-opened the RTL device with `rtlsdr_open()` while the
  previous `rtlsdr_read_async()` thread still held the USB claim, so the reopen
  always failed and the program quit. Worse, the watchdog false-tripped within the
  first slot because it compared the callback counter only against its value one
  loop iteration earlier. The watchdog now treats the RX as stalled only after no
  USB callback for `RTL_STALL_TIMEOUT` (30 s, 2 FT8 slots), and recovery goes
  through a new `restartRtlDevice()` that cancels the async read, joins the RX
  thread and closes the handle before re-opening (also fixing an `rxThread` leak).
  Up to `RTL_MAX_RESTART` clean restarts are attempted before giving up.

### 0.8.3

- **Real SNR estimate.** The reported SNR was previously the Costas sync
  correlation score minus a constant (it clustered around 14-15 dB and was
  not a true SNR). It is now estimated in dB referenced to a 2500 Hz noise
  bandwidth (WSJT-X convention): signal power is measured at the known Costas
  sync tones, and noise from a robust slot-wide median floor (immune to
  signals scaling together). The scattered `-20` display/report offsets were
  removed accordingly, so the value shown in the UI, logged to ADIF and sent
  to PSKReporter is the same physically meaningful dB figure.
- **Implemented the callsign hash table.** Previously stubbed, so nonstandard
  and compound callsigns (transmitted by FT8 as 22/12/10-bit hashes) showed as
  `<...>`. They are now stored and resolved to text. Hash-resolved calls are
  stripped of their angle brackets before display, QSO matching and logging.
- **Synthetic receiver test mode (`--rx-test`).** Generates a realistic FT8
  slot every 15 s containing a varying 4-5 signals (mixed CQ and directed
  messages, A1TEST-family calls, spread across the audio passband and a range
  of amplitudes/SNRs), driving the full decode -> UI -> QSO -> logging pipeline
  with no RTL-SDR hardware. Reporting is forced off in this mode so synthetic
  spots can never reach the live PSKReporter database.
- **UI reworked to non-overlapping windows.** The ncurses layout used
  overlapping `subwin()` panels that shared cell memory and corrupted the
  display during long runs; each panel is now an independent `newwin()` with an
  inset content area, eliminating the bleed.
- **All ncurses access moved to a single thread.** ncurses is not
  thread-safe; keyboard input (`wgetch`) previously ran in a separate thread
  concurrently with rendering, corrupting the display over time. Keyboard
  polling is now handled on the UI thread, so only one thread touches curses.

### 0.8.2

- **Reduced terminal refresh traffic (SSH responsiveness).** The ncurses UI
  no longer repaints the static window borders and titles on every update
  (previously done up to five times per second). Draw helpers now stage
  output with `wnoutrefresh()` and the main UI loop issues a single
  coalesced `doupdate()` only when something actually changed. The clock is
  refreshed about once per second instead of ~5 times. Over a remote (SSH)
  session this eliminates the visible lag caused by continuously streaming
  line-drawing escape sequences; behaviour and appearance are unchanged
  locally.

### 0.8.1

- **x86 (PC) build support.** The Makefile now detects x86 hosts
  (`uname -m` = x86_64/i386/i686) and builds with a new `-Dx86` define. On
  x86 only the receiver (`rtlsdr_ft8d`) is built: the FT8 transmitter (`ft8`)
  and its socket-client helpers (`client`, `sk150lm_beacon`) are Raspberry
  Pi-only, as they drive the BCM PLLD/GPCLK0 clock via DMA. The `install`
  target is likewise architecture-aware on x86 (no `ft8` binary or
  `ft8tx.service`), and `clean` now removes all known artifacts regardless
  of the detected architecture.
- The startup splash reports the build architecture correctly on x86
  ("x86 version") now that `-Dx86` is authoritative.

### 0.8.0

- **RTL-SDR v4 support with autodetection.** The RTL generation is now
  detected from the tuner type at startup (`rtlsdr_get_tuner_type`): an
  R828D is treated as a v4, anything else (R820T/R820T2) as a v3. HF bands
  automatically pick the correct reception path -- Q-branch direct sampling
  on the v3, or the internal upconverter (direct sampling off) on the v4 --
  so HF now works on both generations without manual tweaking. VHF/UHF is
  unchanged. Autodetection can be overridden with the new `--rtl3` / `--rtl4`
  options, and an explicit `-d` still takes precedence.
- **Startup splash window.** A centered window (green border, yellow text)
  is shown for 5 seconds at launch, reporting the detected RTL-SDR device
  (or that none was found), the build architecture (x86 or ARM) and the
  software version.
- Reduced the "device not found" fallback delay from 3 s to 1 s (the splash
  already reports the condition).
- Concurrency, memory-safety and logic fixes across the RX/UI/QSO code
  (thread-safe queues, NULL-pointer and buffer-overflow guards,
  reentrant time functions, QSO peer-dedup fix, corrected self-test).
  See `findings.md` for the full audit.
- Documentation: corrected the transmitter description (Raspberry Pi PLLD /
  GPCLK0 on GPIO4 via DMA, not an external Si5351/I2C part).

### 0.1, (2021/12/05), not yet released

