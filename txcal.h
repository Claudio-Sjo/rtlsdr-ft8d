/*
 * txcal.h -- shared helpers for the transmitter frequency calibration.
 *
 * The FT8 transmitter (ft8) generates RF from the Raspberry Pi's PLLD, which is
 * derived from the BCM SoC crystal. That crystal has an unknown ppm error and
 * is a different oscillator from the receiver's 1 ppm TCXO. The `calibrate`
 * program measures that error by transmitting a known CW tone and receiving it
 * through the (TCXO-accurate) RTL-SDR, then stores the resulting ppm here.
 * ft8 reads it at startup so its transmit frequency is aligned to the RTL-SDR
 * TCXO reference and is repeatable run-to-run.
 *
 * Cal file: $HOME/.config/rtlsdr-ft8d/txcal  (falls back to ./txcal.conf)
 * Format:   a single decimal number, the crystal ppm correction.
 */
#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Fill `out` with the cal file path. Returns out. */
static inline const char *txcal_path(char *out, size_t outsz) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, outsz, "%s/.config/rtlsdr-ft8d/txcal", home);
    } else {
        snprintf(out, outsz, "txcal.conf");
    }
    return out;
}

/*
 * Read the stored TX calibration ppm.
 * Returns true and sets *ppm on success; false if no cal file / unreadable.
 */
static inline bool txcal_read(double *ppm) {
    char path[512];
    txcal_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f == NULL)
        return false;
    double v;
    int n = fscanf(f, "%lf", &v);
    fclose(f);
    if (n != 1)
        return false;
    *ppm = v;
    return true;
}

/*
 * Write the TX calibration ppm, creating ~/.config/rtlsdr-ft8d/ if needed.
 * Returns true on success.
 */
static inline bool txcal_write(double ppm) {
    const char *home = getenv("HOME");
    if (home && home[0]) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.config", home);
        mkdir(dir, 0755);
        snprintf(dir, sizeof(dir), "%s/.config/rtlsdr-ft8d", home);
        mkdir(dir, 0755);
    }
    char path[512];
    txcal_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (f == NULL)
        return false;
    fprintf(f, "%.4f\n", ppm);
    fclose(f);
    return true;
}
