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
peer reply); slot placement is driven by the fake transmitter's real-time
timing (see "Transmission-time realism" below), so the peer reply lands in the
opposite slot like a real alternating QSO.
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



---

## Plan: harden the QSO responder / message parser

Investigation (post Phase 3) found the auto-QSO logic handles only the common
standard-QSO subset of WSJT-X message formats, and -- worse -- misclassifies
anything it does not recognise instead of ignoring it. ft8_lib decodes the full
message-type space to text; the limitation is in the thin QSO-classification
layer, not the decoder.

### Two parsing layers

1. Decode splitter -- `decode()` in rtlsdr_ft8d.cpp (~1470): splits a decoded
   message into `dest src third-token`; feeds the QSO machine only if
   `dest == mycall` (or it is a `CQ`).
2. Classifier -- `parseMsg()` in qsoHandler.cpp (~551): classifies the THIRD
   token into cqMsg/locMsg/sigMsg/RR73Msg/s73Msg. Rules today:
   - digit / `+` / `-`, or 4-char `R+`/`R-`  -> sigMsg (`73` -> s73Msg)
   - exactly `RR73`                          -> RR73Msg
   - 4-char alphanumeric                     -> locMsg (assumed grid)
   - EVERYTHING ELSE                         -> locMsg  (catch-all default)

### Formats NOT handled correctly (measured/derived)

- Free-text messages (`TNX 73`, `GL DX`, `5W ANT`): no dest/src structure; the
  splitter mis-tokenises them and the third token defaults to locMsg.
- `RRR` (older roger, still emitted; the encoder supports it): parseMsg checks
  only `RR73`, so `RRR` falls through to the locMsg default -> misread as grid.
- Compound `RR73;`-style (DXpedition/contesting, e.g.
  `K1ABC RR73; CQ W9XYZ EN37`): the `;` compound is not parsed.
- Non-standard / compound calls (`PJ4/KA1ABC`, `<WA9XYZ>`): a `/P` or `/R`
  suffix on our own call, or a hashed peer, can defeat the `dest == mycall`
  match so the QSO never starts.
- Contest exchanges (EU VHF, ARRL FD/RTTY, WWROF): serial/section/R-report in
  positions the 3-token model does not expect. Decode to text fine; the QSO
  machine cannot interpret them. (Auto-QSO in a contest is likely out of scope,
  but they must at least be ignored safely.)
- THE CORE HAZARD: the locMsg catch-all means unknown content is not rejected --
  it is silently treated as a locator, which can push the state machine the
  wrong way rather than being safely ignored.

### Proposed work (phased, use the fake_ft8 harness to measure then fix)

- [ ] Step 1 -- Measure. Extend the fake responder (fakeTx.cpp) with an
      "edge-case" mode that emits: free text, `RRR`, a `/P` suffixed call, a
      hashed `<call>`, and one contest exchange. Run the loop and record how the
      RX state machine reacts (baseline evidence, no fix yet).
- [ ] Step 2 -- Add an explicit "unknown -> ignore" outcome. Introduce a
      `noneMsg`/`unknownMsg` value in `peermsg_t` (qsoHandler.h) and make
      `parseMsg` return it for anything it does not positively recognise, instead
      of defaulting to locMsg. `addQso` then ignores unknowns (no state change),
      which is the safe behaviour.
- [ ] Step 3 -- Recognise the missing standard tokens. Add `RRR` (treat like
      RR73 for our purposes, or as an ack that advances to reply73/RR73 per the
      state table), and tighten the grid test (validate a real 4-char Maidenhead
      locator: letter letter digit digit) so non-grids are not taken as grids.
- [ ] Step 4 -- Robust tokenising / call matching. Handle `dest == mycall` when
      our call carries a `/P` or `/R` suffix, and skip/flag free-text and
      compound (`;`) forms up front so they never reach the report/grid logic.
- [ ] Step 5 -- Decide contest-format policy: at minimum ignore them safely;
      optionally log them for the operator. Do NOT attempt auto-QSO in contest
      formats unless explicitly wanted.
- [ ] Step 6 -- Re-run the harness (Step 1 emissions) to confirm each edge case
      is now either handled or safely ignored, and that the standard QSO still
      completes. Update this document and CHANGELOG.

Note: Steps 2-5 modify the RX QSO logic (qsoHandler.cpp / .h), which is product
behaviour independent of the test harness; the harness (Step 1/6) is how we get
evidence before and after. Keep the fake edge-case emissions behind the existing
--fake-tx test path so production is unaffected.

Status: PLANNED, not implemented.



---

## Step 1 results -- measured responder behaviour (baseline, no fixes)

Added an edge-case emission mode to the fake responder: with `FAKETX_EDGE=1`
set (behind `--fake-tx`), `fakeEdgeMessage()` replies to the receiver's CQ /
transmissions with a cycling series of non-standard WSJT-X forms, each addressed
to the RX callsign, so the RX parser / QSO state machine can be observed.

Method: driven via tmux with AUTOCQ + AUTOREPLY + AUTOQSO enabled; evidence taken
from the RX's OWN transmissions (`faketx.log`) and the FT8 Traffic window pane.
(The `LOG()` debug goes to stderr, which ncurses swallows -- the RX's resulting
transmissions are the reliable indicator of its internal state.)

Emitted form -> how it decoded -> how the RX reacted:

| Emitted (peer -> RX) | RX decoded it as | RX reaction | Verdict |
| -------------------- | ---------------- | ----------- | ------- |
| `SA0PRF F1ABC RRR` | `SA0PRF F1ABC RRR` | replied `+17` (report) | MISCLASSIFIED: `RRR` hit the locMsg catch-all; treated like a locator/report instead of a roger. QSO did not close. |
| `SA0PRF F1ABC/P JN99` | `SA0PRF F1ABC/P JN99` (/P kept) | kept resending `+17` | Peer call carries `/P`; exchange did not progress cleanly. |
| `SA0PRF <F1ABC> RR73` | (hashed/bracketed) | kept resending `+17` | Hashed peer call not matched to the ongoing QSO peer; no advance. |
| `TNX 73 GL` (free text) | not routed to QSO machine | ignored | SAFE by accident: no `dest` match, so it never reached the QSO logic. |
| `SA0PRF F1ABC R 579 MA` (ARRL RTTY) | `SA0PRF F1ABC R+00` (!) | -- | DECODE-LEVEL MANGLING: ft8_lib rendered the contest exchange as a bogus `R+00` report. |
| `SA0PRF F1ABC 559 0013` (serial) | `SA0PRF F1ABC R` (!) | -- | DECODE-LEVEL MANGLING: contest serial form rendered as a stray `R`. |

Key takeaways (confirmed with evidence):
1. The `locMsg` catch-all is a real hazard: `RRR` (and anything unrecognised) is
   silently taken as a locator, pushing the state machine the wrong way rather
   than being ignored. This is the highest-value thing to fix (planned Step 2).
2. `RRR` is a legitimate roger the parser does not know (only `RR73`). Planned
   Step 3.
3. `/P` suffixed and `<hashed>` peer calls break the "same peer" match so an
   in-progress QSO does not advance. Planned Step 4.
4. Free text happens to be safe today only because it lacks a `dest` match --
   not by design. Worth an explicit ignore path.
5. Contest / RTTY forms are MANGLED by the decoder itself into bogus standard
   messages -- a subtlety beyond the QSO layer. At minimum the QSO machine must
   not act on such garbage (Step 2 fail-safe covers this); fully distinguishing
   contest formats would require decode-type awareness (out of scope for now,
   note for Step 5).

Net: the RX did not cleanly handle ANY of the non-standard forms; the standard
QSO (measured in Phase 3) remains the only fully-supported exchange. This
baseline is what Steps 2-5 must improve, re-measured with the same harness.

Status: Step 1 DONE (measurement). Steps 2-6 PLANNED.



---

## Free-text attribution to a QSO (by frequency + slot) -- DONE

Requirement: attach a peer's free-text (and other not-addressed-to-us) messages
to the in-progress QSO for DISPLAY/LOGGING ONLY; the QSO state machine stays
driven purely by structured FT8 tokens.

Confirmed premise: within a QSO both stations hold one frequency (WSJT-X default
"Hold Tx Freq" behaviour; drift is a few Hz on HF). Tolerance chosen: +/-50 Hz
(the FT8 signal footprint). It also requires the correct SLOT (ODD/EVEN) -- a
station on the same frequency in the wrong slot is not our peer.

Implementation:
- qsoHandler: `qsoInProgress()`, `getActiveQsoFreq()`, `getActiveQsoPeer()`,
  `getActiveQsoPeerSlot()` (peer slot from `currentQSO.ft8slot`).
- decode(): for a message that is neither CQ nor addressed to us, if a QSO is in
  progress and `|absFreq - qsoFreq| <= 50` AND `thisSlot == peerSlot`, push it to
  the DISPLAY queue (`qso_queue`) only -- never the state-machine queue
  (`qsoh_queue`) -- so it can never change state. Uses the pristine decoded text.
- fakeTx: peer now holds ONE frequency per QSO (chosen once, reset on a fresh
  CQ), which also made the QSO more realistic. Test hooks: `FAKETX_FREETEXT`
  (peer sends `TNX 73 GL` on its frequency) and `FAKETX_EDGE` (edge-case series).

Verified via tmux (AUTOCQ+AUTOREPLY+AUTOQSO): the peer's `TNX 73 GL` decoded in
the Traffic window AND was attributed to the QSO in the Ongoing-QSO window
(matched by freq+slot), while the RX's own transmissions stayed token-driven
(kept sending its report) -- i.e. free text did NOT alter QSO state. Exactly the
intended display/log-only behaviour.

### ft8_lib free-text ENCODER added (in-tree hack)

Found that the vendored ft8_lib had NO free-text encoder (`ftx_message_encode`
tried only std + nonstd), so the harness could not synthesize free text. Since
`libft8/` is our frozen in-tree copy, added `ftx_message_encode_free()` in
`libft8/ft8/message.c` (inverse of `ftx_message_decode_free`: pack up to 13
chars as a 71-bit base-42 value, i3=0/n3=0), and wired it as the fallback in
`ftx_message_encode`. Round-trip verified: `TNX 73 GL`, `GL DX 73`, `73 GL`
encode+decode MATCH; standard messages unaffected.

### Repo bug fixed: libft8/ft8/ was not tracked by git

While committing, found the `.gitignore` rule `ft8` (meant for the root `ft8`
transmitter binary) ALSO matched the `libft8/ft8/` directory, so the ENTIRE FT8
codec (constants/crc/decode/encode/ldpc/message/text .c/.h) was excluded from
git -- a fresh clone would not build. Fixed by anchoring the binary-ignore
patterns to the repo root (`/ft8`, `/client`, `/rtlsdr_ft8d`, `/mktestiq`,
`/sk150lm_beacon`, `/calibrate`). The 15 `libft8/ft8/` source files are now
tracked (`.o` still ignored via `*.o`).

Status: DONE (feature + free-text encoder + gitignore fix), verified on x86.



---

## Transmission-time realism (fake-tx 12.6 s occupancy) -- DONE

Earlier the fake transmitter deposited its rendered signal instantly and slot
placement came only from a one-slot delay in the RX render (`fillFakeTxBuffer`).
That reproduced the slot ALTERNATION but not the real intra-slot timing (the
12.6 s transmission occupancy and the ~2.4 s decode gap). The fake now models
the real transmitter's timing so the simulation matches reality.

New per-connection handler sequence (`fakeTx.cpp`, `--fake-tx` only):
1. Send `SEND_ACK` immediately (as real `ft8`).
2. Compute the own-echo audio and the peer reply.
3. `fakeWaitSlotBoundary()` -- align to the next 15 s FT8 slot boundary (like the
   real `ft8` `sleep(15 - sec)` / `wait_every_15_sec`).
4. Send `FREQ <absHz>` (RTXstate=true) and deposit the receiver's OWN
   transmission -> rendered in this (the TX) slot.
5. `fakeInterruptibleSleep(FT8_TXTIME)` -- hold ~12.6 s, the transmission
   occupancy.
6. Deposit the synthetic PEER reply (now ~12.6 s into the slot) -> picked up by
   the next slot's fill and rendered in the OPPOSITE slot. Send RTXstate=false
   ("End of transmission").

Both waits poll `fakeTxStopFlag` in 50 ms steps so shutdown stays prompt; the
handler is a detached per-connection thread, so the listener keeps accepting.
The RX-side one-slot `peerHeld` delay in `fillFakeTxBuffer` was REMOVED -- the
handler's real-time deposits now provide slot placement (keeping it would push
the peer an extra slot).

Side benefit: because the receiver's `TXHandler` performs its three blocking
reads across these waits, `txStatusFlag`/`setTransmitting()` now track the true
transmission window -- the GUI `RTx` indicator shows `Tx` for the real ~12.6 s,
not instantaneously.

Verified live (tmux, AUTOCQ + AUTOREPLY + AUTOQSO): a full exchange completes at
the realistic one-message-per-slot cadence, e.g. (timestamps 15 s apart, slots
alternating): RX `CQ SA0PRF JO99` (ODD) -> peer `SA0PRF F1ABC JN99` (EVEN) ->
RX `F1ABC SA0PRF +17` (ODD) -> peer `SA0PRF F1ABC R-10` (EVEN) ->
RX `F1ABC SA0PRF RR73` (ODD) -> peer `SA0PRF F1ABC 73` (EVEN) -> new CQ. The peer
holds one frequency per QSO. This matches the operator's model: CQ on ODD (12.6 s
TX + 2.4 s decode gap), reply on EVEN, next reply on the following ODD.

(Unchanged pre-existing item: when the RX is the CQ initiator and ends by
sending RR73 then receiving the peer's `73`, the `s73Msg` path resets without
`logToAdi`, so that role produces no ADIF record; the QSO itself completes
correctly. Logging policy, not a timing issue.)
