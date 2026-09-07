## CHANGELOG

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

