# rtlsdr-ft8d — Source Audit Findings

Audit of the project's own source (`rtlsdr_ft8d.cpp/.h`, `ft8_ncurses.cpp`,
`qsoHandler.cpp`, `pskreporter.cpp`, `ft8.cpp`) excluding the vendored
`ft8_lib` submodule and `mailbox.c`.

## Target / platform notes

- Primary target is **ARM (Raspberry Pi)** — see the `Makefile` ARM detection
  (`-DRPI1` for armv6 `arm1176jzf-s`, else `-DRPI23`), and the transmitter
  (`ft8.cpp`) which drives the on-SoC PLLD clock out on **GPIO4 / GPCLK0** via
  DMA. There is no external synthesizer in the current RF-generation path.
- A **PC option is a future enhancement**: on a PC (no BCM GPIO/PLLD), the
  transmitter would instead use an **Si5351 accessed over USB via a CH551**
  microcontroller. Not implemented yet. Fixes below must not assume x86 vs ARM;
  keep the RX/decoder/UI/QSO code portable, and keep the BCM-specific TX code
  (`ft8.cpp`) cleanly separable so a future Si5351/CH551 backend can be added
  behind the same `FT8Tx ...` socket command interface.

---

## Critical

### C1. Data races on all shared queues (unlocked `.size()`/`front()`/iterate)
`std::vector` is not thread-safe. Producers `push_back` under a mutex, but
consumers test `.size()` and sometimes read `front()` **outside** the lock. A
concurrent `push_back` can reallocate the backing store while another thread
reads it → UB (torn reads, use-after-free, crashes).

- `pskUploader` — `rtlsdr_ft8d.cpp`: `if (dec_results_queue.size() > 0)` and
  `while (dec_results_queue.size() > MAX_REPORTS_PER_PACKET)` tested before
  taking `lock`; `postSpots` pushes under `lock`.
- `CQHandler` — `ft8_ncurses.cpp`: `log_queue.size()`, `cq_queue.size()`,
  `qso_queue.size()`, `kbd_queue.size()` all tested unlocked.
- `QSOHandler` — `qsoHandler.cpp`: `while (tick_queue.size() == 0)` and
  `while (qsoh_queue.size())` tested unlocked.

Also a **lock-ordering / mutex-mapping** muddle: `printSpots` takes `msglock`
then `CQlock`; `CQHandler` takes only `CQlock`. Comments claim `msglock`
"protects the decodes structure" but the mapping is inconsistent.

Fix: lock-guarded pop helper (lock, check size, move to local, unlock, return
bool). Never touch a shared vector unlocked.

### C2. NULL `reporter` deref when PSK reporting toggled on at runtime
`reporter` (`PskReporter*`) is only `new`'d when `!rx_options.noreport` at
startup. But `pskThread` always runs and `postSpots` queues whenever `noreport`
is false. Starting with `-x` (noreport) and later issuing the `PSK ON` keyboard
command (`enableReporting()` clears `noreport`) makes `postSpots` queue records
and `pskUploader` call `reporter->addReceiveRecord()` / `reporter->send()` on a
**NULL** pointer → segfault.

Fix: always construct `reporter`; gate only the *sending* on the reporting
flag, and null-check before use.

---

## High

### H1. `strtok` NULL deref on malformed / short over-the-air messages
`decode()` in `rtlsdr_ft8d.cpp`: after matching `"CQ"`, code does
`strPtr = strtok(NULL, " ")` then `strlen(strPtr)` / `snprintf(...,"%.12s",strPtr)`
without checking for NULL. A "CQ" with no following token → `strlen(NULL)`
segfault. Same hazard in the log branch (`src = strtok(NULL," ")` then
`snprintf("%s", src)`). Attacker-influenceable (anyone can transmit FT8). Code
comment already admits "This needs to be handled".

Also `strtok` is non-reentrant and is used on two buffers (`text`, `msgToLog`)
in the same function; fragile.

### H2. `atofs` out-of-bounds on empty argument
`atofs()` does `len = strlen(s); last = s[len-1]; s[len-1] = '\0';`. If `s` is
empty (`len == 0`), `s[-1]` is read and written — OOB on the byte before the
argument. Reachable via `-o ""`, `-u ""`, etc.

Fix: `if (len == 0) return 0.0;` up front.

### H3. `decodeRecordedFile` OOB on short filename
`strcmp(&filename[strlen(filename) - 3], ".iq")`: a filename shorter than 3
chars (`-r a`) makes the pointer underflow → OOB read.

Fix: check `strlen(filename) >= 3` first.

---

## Medium

### M1. `KBDHandler` `editString` buffer overflow
`ft8_ncurses.cpp`, default key case:
```
editString[ix] = key;
editString[ix + 1] = 0;
```
`editString` is `char[MAXTXSTRING]` (40), `ix = strlen(editString)`, no bound
check. Typing >39 chars without ENTER/DEL writes past the buffer → stack
corruption.

Fix: `if (ix < MAXTXSTRING - 1)`.

### M2. `checkPeer` indexing bug + `addPeer` off-by-one
`qsoHandler.cpp`:
```
for (uint32_t i = 0; i < peersIdx; i++)
    if (peers[peersIdx] == theHash)   // BUG: peers[peersIdx], should be peers[i]
        return true;
```
Duplicate-peer detection never works (always reads the current write slot, which
is outside the populated range) → worked QSOs are not suppressed.
`addPeer` wraps with `if (peersIdx > MAXQSOPEERS) peersIdx = 0;` allowing a write
to `peers[MAXQSOPEERS]` (index 512 in a 512-element array) once → OOB write.

Fix: index `peers[i]`; wrap on `>= MAXQSOPEERS`.

### M3. Non-reentrant `gmtime`/`localtime` shared across threads
`gmtime`/`localtime` return a pointer to one static `struct tm`. Used
concurrently by the decoder thread (`rx_state.gtm = gmtime(...)`), the ncurses
thread (`printLog`/`printCQ`/`printClock`/`printQSORemote`), the QSO thread
(`logToAdi`/`logQSO`), and `saveSample`. Races garble timestamps and can tear
`struct tm`. `rx_state.gtm` stores the pointer to that shared static — value can
change under the reader.

Fix: `gmtime_r`/`localtime_r` with caller-owned `struct tm`.

### M4. `decoderSelfTest` inverted pass condition
```
if (strcmp(dec_results[0].call, "K1JT") && strcmp(dec_results[0].loc, "FN20"))
    return 0;   // fail
else
    return 1;   // success
```
`strcmp` returns 0 on match, so this returns success (1) if *either* field
matches, and fail only if *both* differ. CI self-test can pass on a partial /
wrong decode.

Fix: `if (!strcmp(call,"K1JT") && !strcmp(loc,"FN20")) return 1; else return 0;`.

### M5. Waterfall block-count truncation (header `DCHECK`)
`rtlsdr_ft8d.h`: `NUM_BLOCKS = 92` "vs 92.25", `MAG_ARRAY = 94208 vs 94464`.
`ft8_subsystem` fills and passes 92 blocks; the last ~quarter symbol of the slot
is not searched. Slight sensitivity loss at the slot tail, not a crash. Verify
against `ft8_lib` monitor sizing.

### M6. `main()` `getopt_long` `case 0` fall-through
`case 0:` (long opts) has no `break` before `case 'f':`. `--version` exits so is
safe today, but any future non-exiting long option falls through into the
frequency parser with possibly-NULL `optarg` → `strcasecmp(NULL,...)` crash.

---

## Lower severity / correctness / cosmetic

### L1. `stderr = stream;` reassignment
`main()` assigns the `stderr` global to a `FILE*` from `fopen("/tmp/ft8.log")`.
Assigning `stderr` is not portable (UB per C standard). Use
`freopen("/tmp/ft8.log", "w", stderr)`.

### L2. Missing `fopen` NULL checks in ADI/QSO logging
`createADIheader`, `logToAdi`, `logQSO` (`qsoHandler.cpp`) never check `fopen`.
If `~/ft8QSOdir` is absent (README requires a manual `mkdir`), `fprintf(NULL,...)`
→ segfault.

### L3. `usage()` text bugs
- Stray `"Use:\rtlsdr_ft8d"` — `\r` should be `\n\t`.
- `-w` and `-r` help lines are swapped (`-w` says "Read", `-r` says "Write").

### L4. `pskUploader` design
Enters drain path only on a leading unlocked size check, then `sleep(60)` before
locking; `erase(begin())` is O(n) per pop (O(n^2) drain). Batches at best once a
minute. Prefer a condition variable and pop-from-front helper. (Functional, not
a crash — folded into C1's helper where practical.)

### L5. `decoder` thread priority
`SCHED_RR`, `sched_priority = 90` despite a "low priority" comment; requires root
and is unusually high. Cosmetic/behavioral, left as-is unless requested.

### L6. Large stack buffers
`readRawIQfile`/`writeRawIQfile`: `float filebuffer[2*15*3200]` = 384 KB on the
stack. OK on default main-thread stack; safer on the heap. Left as-is.

### L7. `webClusterSpots` dead code
Calls `curl_global_init` inside the per-result loop and posts to hardcoded
`mycluster.localhost`. Unused placeholder.

### L8. Version-string duplication / stale CHANGELOG
`rtlsdr_ft8d_version = "0.7.0"` and `pskreporter_app_version = "..._v0.7.0"` are
hand-synced; `CHANGELOG.md` still says 0.1.

---

## Fix order (this pass)

1. C1 thread-safe queue access (helper) — most likely field-crash cause.
2. C2 NULL `reporter` guard.
3. H1 `strtok` NULL guards; H2 `atofs` empty; H3 short-filename guard.
4. M1 `editString` bound.
5. M2 `checkPeer`/`addPeer`.
6. M3 `gmtime_r`/`localtime_r`.
7. M4 self-test condition.
8. L1 `freopen`; L2 `fopen` checks; L3 usage text.
9. Build & verify.
