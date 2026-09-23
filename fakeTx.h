/*
 * fakeTx.h -- hardware-free stand-in for the ft8 transmitter (testing only).
 *
 * Part of rtlsdr-ft8d. See testability.md for the design and phasing.
 *
 * Phase 1: a socket-compatible fake transmitter that mimics the real ft8
 * daemon's socket behaviour without any Raspberry Pi GPCLK/DMA hardware. It is
 * spawned as a thread from the receiver when --fake-tx is given, listens on the
 * same UNIX socket the real ft8 uses, parses transmit requests the same way,
 * applies the Option 3 frequency rule (band base -> choose audio slot; a
 * frequency inside the band -> use verbatim), replies with SEND_ACK + the
 * chosen frequency (FREQ <absHz>) over the socket, and logs each accepted
 * transmission. No IQ generation yet (that is Phase 2).
 *
 * Multithreaded by design, to mirror a real concurrent server: one listener
 * thread owns accept(), and each accepted connection is served by its own
 * handler thread.
 *
 * GPLv3.
 */

#pragma once

#include <pthread.h>
#include <stdbool.h>

/* Start the fake transmitter listener thread. `dialHz` is the receiver's dial
   frequency, used to convert an absolute transmit frequency into the audio
   offset the RX baseband generator needs (audio = absFreq - dial). Returns 0 on
   success. Call once at receiver start-up when --fake-tx is enabled. */
int fakeTxStart(unsigned int dialHz);

/* Request the listener (and any in-flight handler threads) to stop, then join.
   Idempotent; unlinks the socket. Call at receiver shutdown. */
void fakeTxStop(void);

/* Pending-transmission hand-off (Phase 2/3): when the fake transmitter accepts a
   SEND_F8_REQ it deposits (a) the receiver's OWN transmission and (b), in QSO
   mode, a synthetic PEER reply addressed back to the receiver. The receiver's
   per-slot generator pops both and renders them into the decode buffer, so the
   RX both hears its own transmission and receives a reply that drives its QSO
   state machine.

   fakeTxPopOwn() / fakeTxPopPeer() each return true and fill msg/audioHz if that
   transmission is pending, clearing it (one-shot per slot). Thread-safe.
   `msgCap` is the size of the caller's msg buffer. */
bool fakeTxPopOwn(char *msg, int msgCap, float *audioHz);
bool fakeTxPopPeer(char *msg, int msgCap, float *audioHz);
