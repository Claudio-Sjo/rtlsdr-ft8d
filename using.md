# Using rtlsdr-ft8d

Operator and developer guide: how to build, run, drive the GUI, and test the
FT8 transceiver. For the DSP internals see `dsp-chain.md`; for measured receiver
characteristics see `rx-characterization.md`; for the test harness design see
`testability.md`.

The project builds two main programs:
- **`rtlsdr_ft8d`** -- the receiver / QSO controller (ncurses GUI). Built on all
  platforms (x86 and Raspberry Pi).
- **`ft8`** -- the transmitter that drives the Raspberry Pi GPCLK0/DMA. Built
  only on the Pi. On x86 a hardware-free fake stands in (see Testing).

---

## 1. Building

```
make              # default build (wide RX, 6400 sps, ~200..2900 Hz audio)
make narrowband   # legacy narrow RX (3200 sps, ~200..1500 Hz); -DNARROWBAND
make clean        # remove build products
make mktestiq     # build the hardware-free test-vector generator (host tool)
```

The Makefile auto-detects the target (x86 vs Raspberry Pi 1/2/3/4) and sets the
right flags. On x86 only `rtlsdr_ft8d` (+ helpers) is built; on the Pi the `ft8`
transmitter and its helpers are built too.

Install (Pi): `sudo make install` (installs `rtlsdr_ft8d`, `ft8`, `calibrate`
and the `ft8tx.service` systemd unit).

Dependencies: libusb-1.0, librtlsdr, fftw3f, libcurl, ncurses, pthreads.

---

## 2. Running the receiver

```
rtlsdr_ft8d -f <freq|band> -c <callsign> -l <locator> [options]
```

Required:

| Switch | Meaning |
| ------ | ------- |
| `-f`   | Dial frequency in Hz (suffix k/M allowed) OR a band string. With a band string the standard dial for that band is used. Bands: 160m 80m 60m 40m 30m 20m 17m 15m 12m 10m 6m 4m 2m 1m25 70cm 23cm |
| `-c`   | Your callsign (max 12 chars) |
| `-l`   | Your Maidenhead locator (max 6 chars) |

Receiver options:

| Switch | Meaning |
| ------ | ------- |
| `-g N`   | Gain 0-49 (default 29) |
| `-a`     | Auto gain (no argument) |
| `-o Hz`  | Frequency offset (default 0) |
| `-p ppm` | Crystal correction factor in ppm (default 0) |
| `-u Hz`  | Upconverter frequency (e.g. `-u 125M`) |
| `-d N`   | Direct sampling 0/1/2 (v3-only; auto on HF per RTL generation) |
| `--rtl3` | Force RTL-SDR v3 behaviour (R820T2, Q-branch direct sampling on HF) |
| `--rtl4` | Force RTL-SDR v4 behaviour (R828D, internal upconverter on HF) |
| `-n N`   | Max decode iterations (0 = infinite, default) |
| `-i N`   | Device index for multiple receivers (default 0) |

Debugging / testing options:

| Switch | Meaning |
| ------ | ------- |
| `-x`        | Do NOT report spots to web clusters (PSKreporter etc.) |
| `-t`        | Decoder self-test: generate a signal, decode it, print result, exit |
| `--rx-test` | Synthetic RX source (A1TEST... traffic) with NO RTL device |
| `--fake-tx` | Spawn the hardware-free fake ft8 transmitter (testing; no Pi hardware) |
| `-w PREFIX` | Write the received signal to a file and exit |
| `-r FILE`   | Read a `.iq` or `.c2` file, decode, and exit (raw: sample rate float32, 2 channels) |

Other:

| Switch | Meaning |
| ------ | ------- |
| `--help`    | Show the option list |
| `--version` | Show the program version |

Example:

```
rtlsdr_ft8d -f 20m -c A1XYZ -l AB12cd -g 29
```

Frequency convention (USB): the reported RF of a decode is `dial + audio`
(see rx-characterization.md). On transmit, the transmitter chooses the audio
slot for a CQ and reports the actual frequency back (see dsp-chain.md / Option 3).

---

## 3. The GUI (QSO mode)

The receiver runs a full-screen ncurses UI with these panes:

- **Header** -- your call/locator, the current TX frequency, program version and
  mode, and (right) the usable bandwidth, e.g. `BW 200-2900 Hz`.
- **Incoming CQ** -- decoded CQ calls you can answer.
- **Transceiver Status** -- PSK Report / Auto Reply / Self CQ / Auto QSO on/off,
  active slot, RTx (Rx/Tx) state.
- **Ongoing QSO** -- the live exchange: your transmissions in RED, the peer's
  (and free text attributed to the QSO by frequency+slot) in GREEN.
- **FT8 Traffic** -- all decoded messages this slot.

### Keyboard

Typed **commands** (type the text, then Enter):

| Command | Effect |
| ------- | ------ |
| `AUTOCQ ON` / `AUTOCQ OFF` | Enable/disable automatic CQ calling (Self CQ) |
| `AUTOREPLY ON` / `AUTOREPLY OFF` | Enable/disable auto-answering others' CQs |
| `AUTOQSO ON` / `AUTOQSO OFF` | Enable/disable carrying a QSO to completion |
| `PSK ON` / `PSK OFF` | Enable/disable PSKreporter spotting |
| `SLOT ODD` / `SLOT EVEN` | Choose the slot you transmit in |
| `QUIT` | Exit the program |

Navigation keys:

| Key | Effect |
| --- | ------ |
| `TAB` | Cycle the active window (CQ -> ... -> TX) |
| Arrow Up / Down | In the CQ window, move the selection through decoded CQs |

Automatic operating: enable `AUTOCQ` (call CQ and work answers), `AUTOREPLY`
(answer other stations' CQs), and/or `AUTOQSO` (complete the grid -> report ->
RR73 -> 73 exchange automatically). The QSO state machine is driven only by the
structured FT8 tokens; free text received on the QSO's frequency and slot is
shown/logged with the QSO but never changes the QSO state.

Logging: completed QSOs are written to an ADIF file under `~/ft8QSOdir/`.

---

## 4. Testing (hardware-free)

All of the following run on a plain PC (x86), no RTL-SDR and no Raspberry Pi.

### 4.1 Decoder self-test

```
rtlsdr_ft8d -t -x
```
Generates an FT8 signal internally, decodes it, prints `Self-test SUCCESS` or
`Self-test FAIL`, and exits. Works for both the wide and narrow builds.

### 4.2 Decode a test vector (`mktestiq` + `-r`)

`mktestiq` writes an interleaved float32 `.iq` file the receiver can read.

```
mktestiq [-w] [-s F] [-n N] [-A amp] [-N amp] [-S seed] [-r RATE] [outfile]
```

| Option | Meaning |
| ------ | ------- |
| (default) | Multi-signal vector at 400/800/1200/1500 Hz |
| `-w`      | Wideband vector spread across ~0..3000 Hz |
| `-n N`    | N distinct signals spread evenly across the band (parallel-decode stress) |
| `-s F`    | A single signal at audio F Hz (edge/frequency sweep) |
| `-A amp`  | Per-signal amplitude for `-s` (default 0.7) |
| `-N amp`  | Noise stddev per I/Q sample (default 0.02); raise to probe the SNR floor |
| `-S seed` | RNG seed (default 12345); vary for noise statistics |
| `-r RATE` | Output sample rate: 3200 (narrow) or 6400 (wide, matches the default build) |

Example (decode a wideband vector through the default wide build):

```
make && make mktestiq
./mktestiq -w -r 6400 test.iq
./rtlsdr_ft8d -r test.iq -x -f 20m -c N0CALL -l AA00
```
The decoded messages print to stdout as `RF_Hz  SNR  cmd  call  loc`, where
`RF_Hz = dial + audio`.

Match the vector rate to the build: `-r 6400` for the default (wide) build,
`-r 3200` for a `make narrowband` build.

### 4.3 Decode CPU benchmark

```
FT8D_BENCH=300 ./rtlsdr_ft8d -r test.iq -x -f 20m -c N0CALL -l AA00
```
Re-runs the decode 300 times and prints the mean wall/CPU ms per slot (a
`BENCH:` line). Useful to compare narrow vs wide cost or to check the Pi budget.

### 4.4 Synthetic RX traffic (`--rx-test`)

```
rtlsdr_ft8d --rx-test -f 20m -c N0CALL -l AA00
```
Injects a synthetic slot of A1TEST-family traffic once per real 15 s slot (no
RTL device), exercising the full decode -> UI -> QSO pipeline with real timing.
Reporting is forced off.

### 4.5 Hardware-free transmitter (`--fake-tx`)

```
rtlsdr_ft8d --fake-tx -f 20m -c SA0PRF -l JO99
```
Spawns a fake `ft8` transmitter thread that speaks the same socket the real
transmitter uses, applies the same frequency logic, and renders what "it
transmitted" back into the receiver so the RX hears itself. Enable AUTOCQ +
AUTOREPLY + AUTOQSO in the GUI and a full self-driven QSO runs with the fake
peer `F1ABC`. Reporting is forced off. Transmissions are logged to `faketx.log`.

Test-only environment variables for `--fake-tx`:

| Env var | Effect |
| ------- | ------ |
| `FAKETX_FREETEXT=1` | The fake peer sends free text (`TNX 73 GL`) on the QSO frequency -- exercises free-text attribution to the QSO |
| `FAKETX_EDGE=1`     | The fake peer cycles through non-standard message forms (RRR, /P call, hashed call, free text, contest exchanges) to probe the parser |

Driving the GUI non-interactively (for scripted tests) is easiest with tmux:

```
tmux new-session -d -s ft8 -x 220 -y 60 bash
tmux send-keys -t ft8 "./rtlsdr_ft8d --fake-tx -f 20m -c SA0PRF -l JO99" Enter
sleep 5
tmux send-keys -t ft8 "AUTOCQ ON" Enter
tmux send-keys -t ft8 "AUTOREPLY ON" Enter
tmux send-keys -t ft8 "AUTOQSO ON" Enter
# observe:
tmux capture-pane -t ft8 -p          # current screen
cat faketx.log                        # what the fake "transmitted"
tmux send-keys -t ft8 "QUIT" Enter    # stop
```

---

## 5. The transmitter (`ft8`, Raspberry Pi only)

The receiver drives the transmitter over a UNIX socket (`/tmp/ft8S`); operators
normally never call `ft8` directly. For a manual standalone test on the Pi:

```
ft8 <freq|band> <message tokens...>
```
It transmits on GPIO4 (GPCLK0) via DMA. See dsp-chain.md and the Option 3 notes
for how the transmit frequency is chosen and reported back to the receiver.

---

## 6. Quick reference

```
# Build + self-test (any PC)
make && ./rtlsdr_ft8d -t -x

# Decode a synthesized wideband vector
make mktestiq && ./mktestiq -w -r 6400 t.iq
./rtlsdr_ft8d -r t.iq -x -f 20m -c N0CALL -l AA00

# Full hardware-free self-QSO (tmux, see 4.5)
./rtlsdr_ft8d --fake-tx -f 20m -c SA0PRF -l JO99   + AUTOCQ/AUTOREPLY/AUTOQSO ON

# Real operation on the Pi
rtlsdr_ft8d -f 20m -c A1XYZ -l AB12cd
```
