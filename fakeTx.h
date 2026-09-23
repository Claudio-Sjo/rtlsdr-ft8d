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

/* Start the fake transmitter listener thread. Returns 0 on success. Safe to
   call once at receiver start-up when --fake-tx is enabled. */
int fakeTxStart(void);

/* Request the listener (and any in-flight handler threads) to stop, then join.
   Idempotent; unlinks the socket. Call at receiver shutdown. */
void fakeTxStop(void);
