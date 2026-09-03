# Raspberry Pi 4 Support (64-bit OS)

This document describes the changes required to run rtlsdr-ft8d on a Raspberry Pi 4 with a 64-bit (aarch64) operating system.

## Issues and Fixes

### 1. RPi4 Detection in Makefile

On 64-bit Raspberry Pi OS, `/proc/cpuinfo` no longer shows `Cortex-A72`. Instead it reports `CPU part : 0xd08`. The Makefile auto-detection was falling through to `-DRPI23`.

**Fix:** Added detection by CPU part number `0xd08` as a fallback for 64-bit OS.

### 2. Compilation Error in mainFT8

The `#ifdef` version check in `mainFT8()` only handled `RPI1` and `RPI23`, triggering `#error` when compiled with `-DRPI4`.

**Fix:** Added `#elif defined(RPI4)` case.

### 3. Unix Domain Socket (sockaddr vs sockaddr_un)

The socket code used `struct sockaddr` (16 bytes) instead of `struct sockaddr_un` (110 bytes). On RPi4 with newer kernels, `bind()` and `connect()` fail or behave incorrectly with the undersized address structure.

**Fix:** Replaced `struct sockaddr` with properly initialized `struct sockaddr_un` in all socket code (server `ft8.cpp`, client `ft8_ncurses.cpp`, `client.c`, `sk150lm_beacon.c`). Added `#include <sys/un.h>`.

### 4. Argument Parsing from Socket (wordexp)

The daemon used `wordexp()` with `WRDE_DOOFFS` flag but never initialized `we_offs`, causing corrupted argument arrays passed to `mainFT8()`. Additionally, the global `optind` was not reset between calls, breaking `getopt_long` on subsequent requests.

**Fix:** Removed `WRDE_DOOFFS` flag, reset `optind = 1` before each `mainFT8`/`mainWSPR` call, added `wordfree()` to prevent memory leaks.

### 5. PWM Clock Divider

The PWM clock divider was hardcoded to 2, giving 250 MHz from PLLD on RPi2/3 (500/2). On RPi4, PLLD runs at 750 MHz, so divider 2 gives 375 MHz — making the DMA pacing 1.5× too fast.

**Fix:** Set PWM clock divider to 3 on RPi4 (750/3 = 250 MHz) to match the expected DMA pacing rate.

### 6. DMA Busy-Wait on 64-bit OS (Critical)

The DMA `CONBLK_AD` register is 32-bit, but on aarch64 the bus addresses were compared as 64-bit `long int` values. The comparison never matched, so the busy-wait loops fell through immediately — causing FT8 transmissions to complete in ~2 seconds instead of ~12.6 seconds.

**Fix:** Cast both sides of the comparison to `unsigned int` (32-bit) so the register value correctly matches the bus address on both 32-bit and 64-bit systems.

### 7. Deprecated curl API (Warnings)

The `curl_formadd` / `CURLFORM_*` / `CURLOPT_HTTPPOST` API is deprecated since libcurl 7.56.0.

**Fix:** Replaced with the modern `curl_mime` API (`curl_mime_init`, `curl_mime_addpart`, `curl_mime_name`, `curl_mime_data`, `CURLOPT_MIMEPOST`, `curl_mime_free`).

## Build

After cloning and setting up the submodule, simply run:

```bash
make clean
make
```

The Makefile will auto-detect RPi4 on both 32-bit and 64-bit OS. Verify you see `-DRPI4` in the compiler flags during build.

## Testing

1. Start the daemon:
   ```bash
   sudo systemctl restart ft8tx.service
   ```

2. Send a test transmission:
   ```bash
   ./client
   ```

3. Verify in the journal that TX duration is approximately 12.6 seconds:
   ```bash
   journalctl -u ft8tx.service -n 20 --no-pager
   ```
