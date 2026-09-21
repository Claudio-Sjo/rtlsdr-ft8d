/*
 * mktestiq -- generate a hardware-free FT8 test vector.
 *
 * Writes a complex-float ".iq" file (the same format rtlsdr_ft8d reads with -r)
 * containing one or more standard-callsign FT8 signals summed together at
 * chosen audio frequencies. Decoding the file with `rtlsdr_ft8d -r <file> -x`
 * exercises the whole STFT / waterfall / candidate-search / decode /
 * frequency-reporting path with NO RTL hardware and NO down-conversion (the
 * file is already at baseband).
 *
 * Purpose: verify the receiver locates each signal at the correct audio
 * frequency in the band (reported RF should be dial + audio), and -- for the
 * wideband work (see wideband_plan.md, Phase 0) -- MEASURE the usable passband
 * edge by sweeping a single tone across frequency and observing where decoding
 * stops.
 *
 * Modes:
 *   (default)      Multi-signal vector across the currently-usable band
 *                  (400/800/1200/1500 Hz). Proven baseline.
 *   -w             Wideband vector: signals spread across the full 0..3000 Hz
 *                  WSJT-X audio window (for testing a widened receiver).
 *   -s F           Sweep: emit ONE signal at audio frequency F Hz. Intended to
 *                  be called repeatedly from a shell loop to find the edge.
 *   -r RATE        Output sample rate (default 3200). Use 6400 for wideband
 *                  test vectors. NSAMP scales with the rate.
 *   last arg       Output file name (default test_band.iq).
 *
 * IQ / sideband convention (must match rtlsdr_ft8d's writeRawIQfile so the
 * reader reconstructs the intended sideband):
 *   a signal at +audio is e^{+j*2*pi*audio*t} = (cos, sin);
 *   the file stores I = cos, Q = -sin;
 *   rtlsdr_ft8d's readRawIQfile negates Q back to +sin, so the decoder sees
 *   +audio -- i.e. the signal appears at `audio` Hz, reported as dial + audio.
 *
 * GPLv3, part of rtlsdr-ft8d.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

extern "C" {
#include <ft8/constants.h>
#include <ft8/encode.h>
#include <ft8/message.h>
}

/* Duration of one FT8 slot (seconds). The sample rate is a runtime option so
   the same tool can emit both the current 3200 sps vectors and 6400 sps
   wideband vectors (see wideband_plan.md). */
#define SIGNAL_LENGHT 15

static int gSampleRate = 3200;  /* output sample rate, -r overrides */
static int gNsamp = SIGNAL_LENGHT * 3200;

static float *iBuf = NULL;
static float *qBuf = NULL;

/* Box-Muller white Gaussian noise */
static float wgn(float amp) {
    static double s;
    static int have = 0;
    static double g2;
    if (have) {
        have = 0;
        return (float)(g2 * amp);
    }
    double u1, u2, v1, v2;
    do {
        u1 = rand() / (double)RAND_MAX;
        u2 = rand() / (double)RAND_MAX;
        v1 = 2 * u1 - 1;
        v2 = 2 * u2 - 1;
        s = v1 * v1 + v2 * v2;
    } while (s >= 1 || s == 0);
    double f = sqrt(-2 * log(s) / s);
    g2 = v2 * f;
    have = 1;
    return (float)(v1 * f * amp);
}

/* Add one FT8 signal at the given audio frequency (Hz) into the buffers.
   samplesPerSymbol = sample_rate * FT8 symbol period (0.16 s). */
static int addSignal(const char *message, double audioFreq, double amp) {
    ftx_message_t msg;
    if (ftx_message_encode(&msg, NULL, message) != FTX_MESSAGE_RC_OK) {
        fprintf(stderr, "encode failed for: %s\n", message);
        return -1;
    }
    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    const double df = 6.25;  /* FT8 tone spacing (Hz), fixed by the protocol */
    const double dt = 1.0 / (double)gSampleRate;
    const int samplesPerSymbol = (int)(gSampleRate * 0.16 + 0.5);  /* 512 @3200, 1024 @6400 */
    double phi = 0.0;
    for (int i = 0; i < FT8_NN; i++) {
        double dphi = 2.0 * M_PI * dt * (audioFreq + ((double)tones[i] - 3.5) * df);
        for (int j = 0; j < samplesPerSymbol; j++) {
            int idx = samplesPerSymbol * i + j;
            if (idx >= gNsamp)
                break;
            iBuf[idx] += (float)(amp * cos(phi));
            qBuf[idx] += (float)(amp * sin(phi));
            phi += dphi;
        }
    }
    return 0;
}

static int writeIq(const char *outName, int nSignals) {
    /* Normalize to -3 dB (peak 0.5), like the receiver does after capture */
    float maxSig = 1e-24f;
    for (int i = 0; i < gNsamp; i++) {
        float ai = fabsf(iBuf[i]), aq = fabsf(qBuf[i]);
        if (ai > maxSig) maxSig = ai;
        if (aq > maxSig) maxSig = aq;
    }
    float scale = 0.5f / maxSig;

    /* Write interleaved float32, storing Q negated to match writeRawIQfile so
       readRawIQfile reconstructs +audio (see header comment). */
    FILE *fd = fopen(outName, "wb");
    if (!fd) {
        fprintf(stderr, "cannot open %s\n", outName);
        return -1;
    }
    for (int i = 0; i < gNsamp; i++) {
        float I = iBuf[i] * scale;
        float Q = -(qBuf[i] * scale);
        fwrite(&I, sizeof(float), 1, fd);
        fwrite(&Q, sizeof(float), 1, fd);
    }
    fclose(fd);

    printf("wrote %s (%d samples @ %d sps, %d signal%s)\n",
           outName, gNsamp, gSampleRate, nSignals, nSignals == 1 ? "" : "s");
    printf("decode with: ./rtlsdr_ft8d -r %s -x   (expected RF = dial + audio)\n", outName);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [-w] [-s F] [-r RATE] [outfile]\n"
            "  (default)  multi-signal band vector 400/800/1200/1500 Hz\n"
            "  -w         wideband vector spread across 0..3000 Hz\n"
            "  -s F       single signal at audio F Hz (for edge sweeps)\n"
            "  -r RATE    output sample rate (default 3200; 6400 for wideband)\n"
            "  outfile    output file name (default test_band.iq)\n",
            prog);
}

int main(int argc, char **argv) {
    const char *outName = "test_band.iq";
    int wideband = 0;
    double sweepFreq = -1.0;  /* >=0 selects single-signal sweep mode */

    int argi = 1;
    for (; argi < argc; argi++) {
        if (!strcmp(argv[argi], "-w")) {
            wideband = 1;
        } else if (!strcmp(argv[argi], "-s")) {
            if (argi + 1 >= argc) { usage(argv[0]); return 1; }
            sweepFreq = atof(argv[++argi]);
        } else if (!strcmp(argv[argi], "-r")) {
            if (argi + 1 >= argc) { usage(argv[0]); return 1; }
            gSampleRate = atoi(argv[++argi]);
        } else if (argv[argi][0] == '-') {
            usage(argv[0]);
            return 1;
        } else {
            outName = argv[argi];
        }
    }
    if (gSampleRate <= 0) { usage(argv[0]); return 1; }

    gNsamp = SIGNAL_LENGHT * gSampleRate;
    iBuf = (float *)calloc((size_t)gNsamp, sizeof(float));
    qBuf = (float *)calloc((size_t)gNsamp, sizeof(float));
    if (!iBuf || !qBuf) {
        fprintf(stderr, "out of memory (%d samples)\n", gNsamp);
        return 1;
    }

    srand(12345);  /* fixed seed -> reproducible test vector */

    /* Light noise floor so it looks like a real capture */
    for (int i = 0; i < gNsamp; i++) {
        iBuf[i] = wgn(0.02f);
        qBuf[i] = wgn(0.02f);
    }

    int nSignals = 0;

    if (sweepFreq >= 0.0) {
        /* Sweep mode: a single tone-signal at the requested audio frequency.
           A standard callsign so it decodes to full text (no hash table). */
        char msg[32];
        snprintf(msg, sizeof(msg), "CQ K1JT FN20");
        if (addSignal(msg, sweepFreq, 0.7) < 0) return 1;
        nSignals = 1;
        printf("sweep: %-14s @ %.1f Hz audio\n", msg, sweepFreq);
    } else if (wideband) {
        /* Wideband vector across the full WSJT-X 0..3000 Hz audio window.
           Kept a little inside the edges. Standard calls -> full text. */
        struct { const char *msg; double audio; } sigs[] = {
            {"CQ K1JT FN20", 300.0},
            {"CQ W1AW FN31", 800.0},
            {"CQ G0RDI IO91", 1300.0},
            {"CQ DL1ABC JO62", 1800.0},
            {"CQ EA4XYZ IN80", 2300.0},
            {"CQ VK3ABC QF22", 2800.0},
        };
        const int n = (int)(sizeof(sigs) / sizeof(sigs[0]));
        for (int s = 0; s < n; s++) {
            if (addSignal(sigs[s].msg, sigs[s].audio, 0.7) < 0) return 1;
            printf("added: %-18s @ %.0f Hz audio\n", sigs[s].msg, sigs[s].audio);
        }
        nSignals = n;
    } else {
        /* Default: proven baseline vector in the currently-usable band. */
        struct { const char *msg; double audio; } sigs[] = {
            {"CQ K1JT FN20", 400.0},
            {"CQ W1AW FN31", 800.0},
            {"CQ G0RDI IO91", 1200.0},
            {"CQ DL1ABC JO62", 1500.0},
        };
        const int n = (int)(sizeof(sigs) / sizeof(sigs[0]));
        for (int s = 0; s < n; s++) {
            if (addSignal(sigs[s].msg, sigs[s].audio, 0.7) < 0) return 1;
            printf("added: %-18s @ %.0f Hz audio\n", sigs[s].msg, sigs[s].audio);
        }
        nSignals = n;
    }

    int rc = writeIq(outName, nSignals);
    free(iBuf);
    free(qBuf);
    return rc < 0 ? 1 : 0;
}
