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
3. reports the actual frequency back over the socket (`FREQ <absHz>`);
4. hands the parsed message (+ chosen audio offset) to the receiver's per-slot
   IQ generator, which renders it into the decode buffer at the real slot
   boundary (real-time preserved);
5. for a CQ, on the NEXT slot also renders a plausible reply to that CQ, so a
   full QSO exchange can be driven.

The receiver decodes the rendered slot and its QSO state machine reacts --
producing the next TX request. A closed, self-driving loop with real slot timing.

NOTE (revised after code review): an earlier draft wrote/read a shared `.iq`
file between transmitter and receiver. That is unnecessary -- the receiver
already synthesizes IQ in-process via `genFT8Signal` in its per-slot rxtest path.
The fake instead hands off the message in memory (see "Detailed work plan"
below). No file, no file-handoff race.

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
- **IQ hand-off is in-memory, not a file.** The receiver already renders IQ into
  its decode buffer per slot (`genFT8Signal` in `fillRxTestBuffer`); the fake
  deposits the parsed message into a mutex-guarded in-process structure that the
  slot loop consumes. (An earlier `.iq`-file design was dropped -- see the
  "Detailed work plan" and "Superseded risks" below.)

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

**Phase 2 -- render own transmission into the RX slot -- DONE**
Implemented: `fakeTx` deposits the accepted transmission (message text + audio
offset = absFreq - dial) into a mutex-guarded `fakePending` structure, retrieved
by `fakeTxPopPending()` (one-shot per slot). The receiver, under `--fake-tx`,
drives the synthetic per-slot source (no real RTL) and renders the pending
transmission via `genFT8Signal()` in `fillFakeTxBuffer()`, so the RX decodes and
displays its own transmission at the frequency the fake chose. `--fake-tx` joins
`--rx-test` as a synthetic source across the reporter/RTL/main-loop guards.
Verified end-to-end on x86: a live `./rtlsdr_ft8d --fake-tx -f 20m -c SA0PRF -l
JO99` plus an injected `FT8Tx 14074000 CQ SA0PRF JO99` -> the fake chose an audio
slot, reported it back, and the RX decoded its own CQ the next slot.

Bugfix found during Phase 2: `genFT8Signal()` hard-coded the 3200 sps / 512
samples-per-symbol constants, so the self-test and rx-test generators produced
wrong-rate signals on the wide (6400 sps) default build -- the self-test was
FAILING. Fixed to derive from `SIGNAL_SAMPLE_RATE` / `K_FSK_DEV`; self-test now
passes on both narrow and wide.

**Phase 3 -- auto-reply QSO loop -- DONE**
The fake gained a fixed station identity (`F1ABC` / `JN99`) and a responder
(`fakePeerReply`) that answers the receiver's QSO: CQ -> answer with grid;
signal report -> R-report; RR73 -> 73; 73 -> done. Two pending slots (own echo +
peer reply); the peer reply is rendered one slot LATER than the RX's own
transmission, so it lands in the opposite slot like a real alternating QSO.
Verified live (driven via tmux with AUTOCQ + AUTOREPLY + AUTOQSO enabled): the
receiver runs a complete exchange end to end -- `CQ SA0PRF JO99` -> peer
`SA0PRF F1ABC JN99` -> `F1ABC SA0PRF +17` -> peer `SA0PRF F1ABC R-10` ->
`F1ABC SA0PRF RR73` -> peer `SA0PRF F1ABC 73` -> new CQ -- repeating.

Bug found and fixed via this harness (in the RX, `qsoHandler.cpp` addQso update
branch): on receiving a signal report while already in `replySig`, the state
machine always re-set `replySig` (the advance-to-`replyRR73` logic was commented
out), so a QSO initiator would resend its report forever and never complete.
Fixed: `if (qsoState == replySig) qsoState = replyRR73; else qsoState =
replySig;`. This is a pre-existing state-machine bug, unrelated to the fake
transmitter, that the closed-loop harness surfaced.

Minor open item (logging policy, not correctness): when the receiver is the
initiator and ends by sending RR73 then receiving the peer's `73`, it resets
without writing an ADI record; the QSO itself completes correctly on the air.

All three phases build clean (x86 `-Wall -Wextra`; `fakeTx.cpp` and `ft8.cpp`
ARM syntax-checked) and the decoder self-test passes on the wide default.

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

---

## Detailed work plan (revised after code review)

Code review of the receiver (rtlsdr_ft8d.cpp) changed the architecture in one
important way: **no `.iq` file is needed.** The receiver already synthesizes IQ
directly into its decode buffer and already runs a per-slot synthetic path.

Grounding facts (verified in code):
- `fillRxTestBuffer(idx)` (rtlsdr_ft8d.cpp ~285) already renders FT8 messages
  straight into `rx_state.iSamples[idx]/qSamples[idx]` via `genFT8Signal(iS, qS,
  message, audioHz, amp, noise)` -- the encoder+modulator the fake needs already
  lives in the RX. No dependency on mktestiq or on any file.
- The main slot loop (rtlsdr_ft8d.cpp ~2039-2069) already has an `rx_options.
  rxtest` branch that, once per real 15 s slot, calls `fillRxTestBuffer`, flips
  the buffer, and signals the decoder -- honouring real slot timing. This is the
  exact injection point.
- `genFT8Signal` sums into the buffer, so several messages (own TX + a synthetic
  peer reply) can coexist in one slot.

**Revised architecture:** the fake transmitter is a single async **socket-server
thread** (mirrors the other RX threads: decoder/CQHandler/TXHandler/KBDHandler).
The IQ generation stays on the main slot-loop thread where it already is. The two
communicate through a small mutex-guarded, in-memory structure -- NOT a file.
This removes the earlier file-handoff race and file-phase risks entirely.

### Data flow (per slot)

```
qsoHandler queueTx -> TXHandler --socket--> fakeTx thread
     (FT8Tx <freq> <dest> <src> <extra>)         |
                                                  | parse (like ft8),
                                                  | apply Option 3 (band base ->
                                                  |   choose audio; else verbatim),
                                                  | send FREQ <abs> reply back,
                                                  | log the "transmission",
                                                  | deposit {message,audioHz} into
                                                  |   a mutex-guarded pendingTx slot
                                                  v
main slot loop (rxtest path) -> fillRxTestBuffer():
      render own pendingTx message(s) via genFT8Signal at their audio offset,
      (Phase 3) also render a synthetic peer reply,
      flip buffer + signal decoder  -> decode -> UI/QSO/log
```

### Tasks by phase

**Phase 1 -- socket-compatible fake transmitter thread (no IQ yet) -- DONE**
Implemented in `fakeTx.cpp`/`fakeTx.h`, wired into the receiver behind
`--fake-tx`. A listener thread binds/listens on `SOCKNAME` and spawns a detached
handler thread per connection (multithreaded, mirrors a real concurrent server).
Each handler reads the `FT8Msg`, and for `SEND_F8_REQ` parses
`"FT8Tx <freq|band> <dest> <src> <msg...>"` (band table like `ft8`), applies
Option 3 (`fakeIsBandBase` + `FAKETX_AUDIO_MIN/MAX` 300..2800), replies
`SEND_ACK` then `CHANGE_RTX_STATE "FREQ <absHz>"`, and logs to `faketx.log`.
`--fake-tx` also forces `noreport`. Verified end-to-end on x86: CQ on a band base
-> fake chooses an audio slot; a frequency inside the band -> transmitted
verbatim; a band name -> resolved then chosen. Builds clean (x86 `-Wall
-Wextra`; `fakeTx.cpp` ARM syntax-check clean).

**Phase 2 -- render own transmission into the RX slot**
- [ ] Shared `pendingTx` structure (message text + audio Hz + valid flag),
      mutex-guarded; fakeTx thread fills it, slot loop consumes it.
- [ ] Extend `fillRxTestBuffer` (or a sibling used when `--fake-tx`) to render
      the pending own-TX message via `genFT8Signal` at its audio offset, instead
      of / in addition to the A1TEST filler.
- [ ] Confirm the RX decodes and displays/logs its own "transmitted" message at
      the frequency the fake chose (closes the self-transmit hear-back check on
      x86).
- Risk to handle: slot-phase agreement is now trivial (single process, shared
  slot clock) -- the fake just deposits into `pendingTx`; the slot loop reads it
  at the fill point. Guard with the mutex; stale/duplicate deposits must be
  cleared once consumed.

**Phase 3 -- auto-reply QSO loop**
- [ ] Give the fake a small station identity (callsign/grid) and minimal QSO
      logic: when the RX transmits a CQ, on the NEXT slot render a synthetic
      reply (`<theircall> <fakecall> <grid>`), then progress through the
      exchange (reply -> RR73 -> 73) reacting to what the RX sends.
- [ ] Drive the RX `qsoHandler` state machine end-to-end and confirm a complete
      logged QSO (ADI entry) with correct frequencies.
- Risk to handle: reply realism -- messages must encode (valid callsign/grid)
  and land at an audio slot inside the passband.

### Superseded risks

The earlier "file handoff race" and "file phase agreement" risks are DROPPED:
there is no `.iq` file. IQ is generated in-process on the slot-loop thread from a
mutex-guarded in-memory hand-off. Remaining risks: thread lifecycle/guarding,
reply realism (Phase 3), sample-rate coupling (genFT8Signal already uses the
RX's SIGNAL_SAMPLE_RATE, so wide/narrow both work for free).

Status: PLANNED, not implemented.
