/*
 * mktestiq -- generate a hardware-free FT8 test vector.
 *
 * Writes a 3200 sps complex-float ".iq" file (the same format rtlsdr_ft8d reads
 * with -r) containing several standard-callsign FT8 signals summed together at
 * different audio frequencies spread across the ~3 kHz passband. Decoding the
 * file with `rtlsdr_ft8d -r <file> -x` exercises the whole STFT / waterfall /
 * candidate-search / decode / frequency-reporting path with NO RTL hardware and
 * NO down-conversion (the file is already at baseband).
 *
 * Purpose: verify the receiver locates each signal at the correct audio
 * frequency in the band (reported RF should be dial + audio).
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

/* Must match the receiver's baseband parameters */
#define SIGNAL_LENGHT 15
#define SIGNAL_SAMPLE_RATE 3200
#define NSAMP (SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE)

static float iBuf[NSAMP];
static float qBuf[NSAMP];

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

/* Add one FT8 signal at the given audio frequency (Hz) into the buffers. */
static int addSignal(const char *message, double audioFreq, double amp) {
    ftx_message_t msg;
    if (ftx_message_encode(&msg, NULL, message) != FTX_MESSAGE_RC_OK) {
        fprintf(stderr, "encode failed for: %s\n", message);
        return -1;
    }
    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    const double df = 3200.0 / 512.0;  // tone spacing 6.25 Hz
    const double dt = 1.0 / 3200.0;
    double phi = 0.0;
    for (int i = 0; i < FT8_NN; i++) {
        double dphi = 2.0 * M_PI * dt * (audioFreq + ((double)tones[i] - 3.5) * df);
        for (int j = 0; j < 512; j++) {
            int idx = 512 * i + j;
            if (idx >= NSAMP)
                break;
            iBuf[idx] += (float)(amp * cos(phi));
            qBuf[idx] += (float)(amp * sin(phi));
            phi += dphi;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *outName = (argc > 1) ? argv[1] : "test_band.iq";
    srand(12345);  // fixed seed -> reproducible test vector

    /* Several standard callsigns at audio frequencies spread across the band.
       Standard calls decode to full text (no hash table needed). */
    struct {
        const char *msg;
        double audio;
    } sigs[] = {
        {"CQ K1JT FN20", 400.0},
        {"CQ W1AW FN31", 800.0},
        {"CQ G0RDI IO91", 1200.0},
        {"CQ DL1ABC JO62", 1500.0},
    };
    const int n = (int)(sizeof(sigs) / sizeof(sigs[0]));

    /* Clear + add a light noise floor so it looks like a real capture */
    for (int i = 0; i < NSAMP; i++) {
        iBuf[i] = wgn(0.02f);
        qBuf[i] = wgn(0.02f);
    }

    for (int s = 0; s < n; s++) {
        if (addSignal(sigs[s].msg, sigs[s].audio, 0.7) < 0)
            return 1;
        printf("added: %-18s @ %.0f Hz audio\n", sigs[s].msg, sigs[s].audio);
    }

    /* Normalize to -3 dB (peak 0.5), like the receiver does after capture */
    float maxSig = 1e-24f;
    for (int i = 0; i < NSAMP; i++) {
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
        return 1;
    }
    for (int i = 0; i < NSAMP; i++) {
        float I = iBuf[i] * scale;
        float Q = -(qBuf[i] * scale);
        fwrite(&I, sizeof(float), 1, fd);
        fwrite(&Q, sizeof(float), 1, fd);
    }
    fclose(fd);

    printf("wrote %s (%d samples, %d signals)\n", outName, NSAMP, n);
    printf("decode with: ./rtlsdr_ft8d -r %s -x   (expected RF = dial + audio)\n", outName);
    return 0;
}
