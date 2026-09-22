# Testability

Hardware-free testing infrastructure for rtlsdr-ft8d. Status of each item is
marked. Nothing here is implemented yet unless it says DONE.

Existing harness (DONE, see rx-characterization.md / dsp-chain.md):
- `mktestiq` generates `.iq` test vectors (single tone, multi-signal, dense
  N-signal, SNR sweeps) decoded through the real DSP via `rtlsdr_ft8d -r file -x`.
- `FT8D_BENCH=N` times the decode work per slot.
- `-rx-test` mode: `fillRxTestBuffer()` (rtlsdr_ft8d.cpp) already injects a
  synthetic slot of A1TEST-family traffic once per slot into the unchanged 15 s
  main loop, so the decode -> UI -> QSO pipeline runs with real slot timing and
  no RTL hardware.

The gap: none of the above exercises the RX<->TX **socket** loop, the `ft8`
argument parser, the Option 3 band-base/verbatim frequency logic, the `FREQ`
reply, or the QSO state machine end-to-end -- because the real `ft8` transmitter
only builds/runs on the Raspberry Pi (GPCLK0/DMA). Today that can only be tested
on the Pi.

---

## Plan: `fake_ft8` -- a hardware-free transmitter for closed-loop testing

### Concept

A **socket-compatible stand-in for `ft8`** that never touches the Pi hardware.
It speaks the same UNIX socket (`/tmp/ft8S`), the same `FT8Msg` protocol, and
parses requests the same way `ft8` does. Instead of driving GPCLK/DMA it:

1. parses the `"FT8Tx <freq> <dest> <src> <extra>"` request (same logic as
   `ft8` `parse_commandline`);
2. applies the Option 3 rule (band base -> choose audio slot; in-band -> use
   verbatim) and logs to a file what it "transmitted";
3. waits for the real FT8 slot boundary (real-time preserved);
4. synthesizes the transmitted message into an `.iq` file (reusing the
   `mktestiq` FT8 encoder + generator at the current SIGNAL_SAMPLE_RATE);
5. reports the actual frequency back over the socket (`FREQ <absHz>`);
6. for a CQ, on the NEXT slot also synthesizes a plausible reply to that CQ, so a
   full QSO exchange can be driven.

The receiver reads that `.iq` file as its per-slot input, decodes it, and its
QSO state machine reacts -- producing the next TX request. A closed, self-driving
loop with real slot timing.

### Design decisions (agreed)

- **Real-time is preserved.** The fake waits for real 15 s slot boundaries
  (like `ft8`'s `wait_every_15_sec()`), so slot alignment is exercised for real.
  (An offline turn-by-turn variant was considered and rejected in favour of
  real-time fidelity.)
- **Async thread, not a separate process.** `fake_ft8` runs as a thread spawned
  from the receiver (like the existing `decoder`, `CQHandler`, `TXHandler`,
  `KBDHandler` threads), guarded behind a test flag/build so it never ships in
  the production path. This avoids fork/exec lifecycle management and lets it
  share the process's slot clock and buffers directly.
- **IQ file is written by the (fake) transmitter and read by the receiver;**
  both close it between slots so the next slot's write can overwrite it. The
  handoff MUST be race-safe (see risks) -- design as atomic rename-into-place
  plus a per-slot ready token, not a bare shared file.

### Integration points (from the current code)

- RX per-slot injection already exists: `fillRxTestBuffer()` (rtlsdr_ft8d.cpp
  ~285), invoked once per slot by the unchanged main loop. The fake's generated
  IQ feeds this same injection point instead of the hard-coded A1TEST traffic --
  so no new RX main-loop mode is strictly required for Phase 2; extend this hook.
- Socket server half to copy: `ft8.cpp main()` accept/read/switch (~505-555),
  independent of the DMA code.
- Request parser to reuse/trim: `parse_commandline` freq+message split
  (ft8.cpp ~1209-1260), incl. the band table (now also mirrored as
  `kBandBase[]`).
- Option 3 logic already in `ft8.cpp` transmit loop (`isBandBase`, TX_AUDIO_MIN/
  MAX, `FREQ` reply) -- reuse verbatim.
- FT8 encoder/IQ synthesis to reuse: `mktestiq.cpp` `genFT8Signal`/`addSignal`
  and the `.iq` writer (matches `readRawIQfile`).
- QSO state machine to exercise: `qsoHandler.cpp` (queryCQ/handleTx/state
  transitions) and `TXHandler` (ft8_ncurses.cpp) `FREQ` parsing.

### Phasing (agreed)

- **Phase 1 -- Socket-compatible fake transmitter (standalone value).**
  A `fake_ft8` thread that: binds/accepts on the socket, parses the request like
  `ft8`, applies Option 3, logs the "transmission" to a file, and reports the
  frequency back (`FREQ`). No IQ generation yet. This alone tests the socket
  handshake, the parser, Option 3, the `FREQ` reply and `TXHandler` parsing on
  x86 -- the biggest currently-untestable surface.

- **Phase 2 -- IQ generation + RX per-slot ingest.**
  The fake synthesizes the transmitted message into an `.iq` for the slot; the
  receiver ingests it per slot (extend the `fillRxTestBuffer` hook to read the
  fake's file). Add the race-safe handoff (atomic rename + ready token). This
  lets the RX decode what the fake "sent" and show/log it.

- **Phase 3 -- Auto-reply QSO loop.**
  On a decoded CQ, the fake constructs a valid reply (`<mycall> <peercall>
  <grid>`), encodes it, and emits it on the next slot at a chosen audio slot,
  driving the RX QSO state machine through reply -> RR73 -> 73. Needs the fake
  to carry a small station identity and minimal QSO logic.

Each phase is independently useful and independently testable.

### Risks / hard parts (address explicitly when building)

1. **File handoff race.** Two threads sharing an `.iq` file across slots. Use
   write-to-temp + atomic `rename()` (same filesystem) and a per-slot ready
   token/flag the reader checks; never read a partially written file. "Both
   close the file" is necessary but not sufficient.
2. **Slot phase agreement.** The fake must finish writing the slot's `.iq`
   before the RX begins decoding that slot. Since both live in one process and
   share the slot clock, coordinate via the existing slot boundary + a
   condition variable / ready flag rather than wall-clock guesses.
3. **Thread lifecycle.** Spawn/join the fake thread cleanly on start/exit
   alongside the other RX threads; guard behind a test flag/build
   (e.g. `--fake-tx` or a `-DTESTQSO`-style compile switch) so it is absent from
   production binaries.
4. **Reply realism (Phase 3).** The fake needs a station identity and enough QSO
   logic to answer correctly; otherwise it only echoes. Keep it minimal but
   valid (encode must succeed, callsign/grid must parse).
5. **Sample-rate coupling.** The fake must synthesize IQ at the RX's current
   `SIGNAL_SAMPLE_RATE` (3200 narrow / 6400 wide). Reuse the same rate source so
   wide/narrow builds both work.

### Relationship to the on-Pi verification

This harness closes most of the "Pending on-Pi verification" items in
`wideband_plan.md` (socket round trip, Option 3 behaviour, QSO loop) on x86. It
does NOT replace the on-Pi checks for real GPCLK/DMA RF output and the actual Pi
CPU budget -- those still require the target hardware.

Status: PLANNED, not implemented.
