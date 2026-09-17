/*
 * calibrate -- align the FT8 transmitter frequency to the RTL-SDR TCXO.
 *
 * The Raspberry Pi's FT8 transmitter (the `ft8` program) synthesises RF from
 * the BCM SoC crystal, which has an unknown ppm error. The RTL-SDR receiver
 * has a 1 ppm TCXO and is therefore the best frequency reference available.
 *
 * This program measures the crystal error by:
 *   1. Opening the RTL-SDR and tuning near a chosen test frequency.
 *   2. Launching `ft8 -t <freq> -f` so the Pi emits an UNCORRECTED CW carrier.
 *   3. Capturing IQ and locating the carrier with an FFT.
 *   4. Computing ppm = (f_measured - f_commanded) / f_commanded * 1e6.
 *   5. Averaging several measurements and writing the result to the TX cal
 *      file, which `ft8` reads at startup (as a fixed -p value).
 *
 * Physical setup: the RTL-SDR must be able to hear the Pi's transmitter during
 * calibration -- a loopback via a strong attenuator, or simple RF leakage with
 * the antennas close. Keep the level well below RTL overload.
 *
 * Copyright (C) 2024  rtlsdr-ft8d contributors
 * GPLv3 (see the other source files).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <getopt.h>

#include <rtl-sdr.h>
#include <fftw3.h>

#include <txcal.h>

/* Capture parameters */
#define CAL_SAMPLE_RATE 2400000u   /* RTL sample rate (as in the receiver)   */
#define CAL_NFFT        (1 << 20)  /* 1048576-point FFT -> ~2.29 Hz/bin      */
#define CAL_MEASUREMENTS 8         /* averaged measurements                  */

static rtlsdr_dev_t *dev = NULL;
static volatile bool abort_flag = false;

static void sigHandler(int signum) {
    (void)signum;
    abort_flag = true;
    if (dev)
        rtlsdr_cancel_async(dev);
}

static void usage(void) {
    printf(
        "calibrate -- align the FT8 transmitter to the RTL-SDR TCXO\n\n"
        "Use: calibrate -f <freq> [options]\n"
        "\t-f test frequency in Hz (e.g. 144174000). Required.\n"
        "\t   Choose a clear frequency where you may legally transmit, or use a\n"
        "\t   dummy load / attenuated loopback.\n"
        "\t-g gain [0-49] (default: 29)\n"
        "\t-i RTL device index (default: 0)\n"
        "\t-e path to the ft8 transmitter binary (default: ./ft8)\n"
        "\t-n number of measurements to average (default: 8)\n"
        "\t-d dry run: measure and print ppm but do not write the cal file\n"
        "\t--help\n\n"
        "The RTL-SDR must be able to hear the Pi transmitter (attenuated\n"
        "loopback or close antennas). The measured ppm is written to the TX\n"
        "calibration file and used automatically by ft8.\n");
}

/* Launch "ft8 -t <freq> -f" as a child process. Returns the child pid, or -1. */
static pid_t startTxTone(const char *ft8Path, double freqHz) {
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        /* Child: exec the transmitter in free-running (uncorrected) tone mode */
        char freqArg[32];
        snprintf(freqArg, sizeof(freqArg), "%.0f", freqHz);
        /* Silence the child's stdout so it doesn't clutter the display */
        freopen("/tmp/calibrate_ft8.log", "w", stdout);
        freopen("/tmp/calibrate_ft8.log", "a", stderr);
        execlp(ft8Path, ft8Path, "-t", freqArg, "-f", (char *)NULL);
        /* If execlp returns, it failed */
        perror("execlp ft8");
        _exit(127);
    }
    return pid;  /* parent */
}

/*
 * Capture one block of IQ, run an FFT, and return the offset (Hz) of the
 * transmitted carrier relative to the tuned centre frequency.
 *
 * IMPORTANT: the ft8 "test tone" is NOT a clean CW carrier. txSym() generates
 * the tone with a delta-sigma scheme -- it rapidly alternates between two
 * discrete divider frequencies (f0/f1) so that the LONG-TERM AVERAGE equals the
 * target frequency, and it deliberately randomises the iteration period to
 * spread the resulting spurs. Instantaneously this is a frequency-modulated
 * signal with sidebands, so the single strongest FFT bin is not a reliable
 * estimate of the true frequency.
 *
 * The quantity we actually want -- and the quantity the delta-sigma modulator
 * targets -- is the AVERAGE frequency, which equals the power-weighted spectral
 * centroid of the carrier region. So we locate the carrier by its peak, then
 * compute the centroid over a window wide enough to include the f0/f1 hop span
 * and the spread spurs.
 */
static bool measureToneOffset(fftwf_plan plan, fftwf_complex *in, fftwf_complex *out,
                              double *offsetHz) {
    /* Read raw 8-bit IQ from the RTL */
    int bufLen = CAL_NFFT * 2;  /* I,Q bytes */
    uint8_t *buf = (uint8_t *)malloc(bufLen);
    if (buf == NULL)
        return false;

    rtlsdr_reset_buffer(dev);
    int nRead = 0;
    if (rtlsdr_read_sync(dev, buf, bufLen, &nRead) < 0 || nRead != bufLen) {
        free(buf);
        return false;
    }

    /* Convert unsigned 8-bit IQ to complex float, centred at 0 */
    for (int i = 0; i < CAL_NFFT; i++) {
        in[i][0] = ((float)buf[2 * i] - 127.5f) / 127.5f;
        in[i][1] = ((float)buf[2 * i + 1] - 127.5f) / 127.5f;
    }
    free(buf);

    fftwf_execute(plan);

    double binHz = (double)CAL_SAMPLE_RATE / (double)CAL_NFFT;
    int dcGuard = (int)(2000.0 / binHz);  /* ignore +/-2 kHz around DC */

    /* Precompute magnitude-squared once; find the peak bin (carrier region),
       skipping the DC spike. */
    static float *mag2 = NULL;
    if (mag2 == NULL)
        mag2 = (float *)malloc(sizeof(float) * CAL_NFFT);
    if (mag2 == NULL)
        return false;

    float maxMag = -1.0f;
    int maxBin = -1;
    for (int k = 0; k < CAL_NFFT; k++) {
        float re = out[k][0], im = out[k][1];
        float m = re * re + im * im;
        mag2[k] = m;
        int kk = (k < CAL_NFFT / 2) ? k : (k - CAL_NFFT);
        if (kk > -dcGuard && kk < dcGuard)
            continue;  /* skip DC region for peak search */
        if (m > maxMag) {
            maxMag = m;
            maxBin = k;
        }
    }
    if (maxBin < 0)
        return false;

    /*
     * Power-weighted centroid over a window around the peak. The window must be
     * wide enough to span the two delta-sigma divider frequencies and their
     * spread spurs. The f0/f1 spacing is on the order of a tone_spacing
     * (~6.25 Hz) but the randomised spurs spread further, so use +/-3 kHz.
     * A noise floor is subtracted so only carrier energy contributes.
     */
    int win = (int)(3000.0 / binHz);

    /* Estimate a noise floor from bins just outside the window */
    double noiseAcc = 0.0;
    int noiseN = 0;
    for (int off = win + dcGuard; off < win + dcGuard + 2000; off++) {
        int kp = (maxBin + off) % CAL_NFFT;
        int km = (maxBin - off + CAL_NFFT) % CAL_NFFT;
        noiseAcc += mag2[kp] + mag2[km];
        noiseN += 2;
    }
    double noiseFloor = (noiseN > 0) ? (noiseAcc / noiseN) : 0.0;

    double num = 0.0;  /* sum(power * freqBin) */
    double den = 0.0;  /* sum(power)           */
    for (int off = -win; off <= win; off++) {
        int k = (maxBin + off + CAL_NFFT) % CAL_NFFT;
        int kk = (k < CAL_NFFT / 2) ? k : (k - CAL_NFFT);
        if (kk > -dcGuard && kk < dcGuard)
            continue;  /* never let the DC spike bias the centroid */
        double p = (double)mag2[k] - noiseFloor;
        if (p <= 0.0)
            continue;
        num += p * (double)kk;
        den += p;
    }
    if (den <= 0.0)
        return false;

    double centroidBin = num / den;
    *offsetHz = centroidBin * binHz;
    return true;
}

int main(int argc, char **argv) {
    double testFreq = 0.0;
    int gain = 290;   /* tenths of dB */
    int devIndex = 0;
    const char *ft8Path = "./ft8";
    int measurements = CAL_MEASUREMENTS;
    bool dryRun = false;

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt, li = 0;
    while ((opt = getopt_long(argc, argv, "f:g:i:e:n:dh", long_options, &li)) != -1) {
        switch (opt) {
            case 'f': testFreq = atof(optarg); break;
            case 'g': gain = atoi(optarg) * 10; break;
            case 'i': devIndex = atoi(optarg); break;
            case 'e': ft8Path = optarg; break;
            case 'n': measurements = atoi(optarg); break;
            case 'd': dryRun = true; break;
            case 'h':
            default: usage(); return (opt == 'h') ? 0 : 1;
        }
    }

    if (testFreq < 1e6) {
        fprintf(stderr, "Error: a valid -f test frequency (Hz) is required.\n\n");
        usage();
        return 1;
    }
    if (measurements < 1) measurements = 1;

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    /* Open and configure the RTL-SDR */
    if (rtlsdr_get_device_count() == 0) {
        fprintf(stderr, "Error: no RTL-SDR device found.\n");
        return 1;
    }
    if (rtlsdr_open(&dev, devIndex) < 0) {
        fprintf(stderr, "Error: cannot open RTL-SDR device %d.\n", devIndex);
        return 1;
    }
    rtlsdr_set_sample_rate(dev, CAL_SAMPLE_RATE);
    rtlsdr_set_tuner_gain_mode(dev, 1);
    rtlsdr_set_tuner_gain(dev, gain);

    /* Tune 100 kHz BELOW the tone so the carrier lands away from the DC spike */
    double tuneOffset = 100000.0;
    uint32_t centerFreq = (uint32_t)(testFreq - tuneOffset);
    rtlsdr_set_center_freq(dev, centerFreq);
    rtlsdr_reset_buffer(dev);

    printf("calibrate: RTL-SDR tuned to %.6f MHz, expecting tone at %.6f MHz\n",
           centerFreq / 1e6, testFreq / 1e6);

    /* FFTW setup */
    fftwf_complex *in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * CAL_NFFT);
    fftwf_complex *out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * CAL_NFFT);
    fftwf_plan plan = fftwf_plan_dft_1d(CAL_NFFT, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

    /* Start the transmitter (uncorrected CW tone) */
    printf("calibrate: starting transmitter (%s -t %.0f -f)...\n", ft8Path, testFreq);
    pid_t txPid = startTxTone(ft8Path, testFreq);
    if (txPid < 0) {
        fprintf(stderr, "Error: cannot start the ft8 transmitter.\n");
        fftwf_destroy_plan(plan);
        fftwf_free(in); fftwf_free(out);
        rtlsdr_close(dev);
        return 1;
    }

    /* Give the transmitter a moment to bring up the carrier */
    sleep(3);

    /* Take several measurements and average the ppm */
    double sumPpm = 0.0;
    int good = 0;
    for (int m = 0; m < measurements && !abort_flag; m++) {
        double offsetHz;
        if (!measureToneOffset(plan, in, out, &offsetHz)) {
            fprintf(stderr, "  measurement %d: capture/FFT failed\n", m + 1);
            continue;
        }
        /* The tone should appear at +tuneOffset relative to the tuned centre.
           Any deviation from that is the transmitter's frequency error. */
        double measuredFreq = centerFreq + offsetHz;
        double errorHz = measuredFreq - testFreq;
        double ppm = errorHz / testFreq * 1e6;
        printf("  measurement %d: tone at %.3f Hz offset -> %.6f MHz, "
               "error %+.1f Hz, %+.3f ppm\n",
               m + 1, offsetHz, measuredFreq / 1e6, errorHz, ppm);
        sumPpm += ppm;
        good++;
    }

    /* Stop the transmitter */
    kill(txPid, SIGTERM);
    int wstatus;
    waitpid(txPid, &wstatus, 0);

    /* Cleanup DSP/radio */
    fftwf_destroy_plan(plan);
    fftwf_free(in);
    fftwf_free(out);
    rtlsdr_close(dev);

    if (good == 0) {
        fprintf(stderr, "Error: no valid measurements. Is the tone reaching the RTL-SDR?\n");
        return 1;
    }

    double avgPpm = sumPpm / good;
    printf("\ncalibrate: averaged %d measurement(s) -> %.3f ppm\n", good, avgPpm);

    if (dryRun) {
        printf("calibrate: dry run, cal file NOT written.\n");
        return 0;
    }

    if (txcal_write(avgPpm)) {
        char path[512];
        txcal_path(path, sizeof(path));
        printf("calibrate: wrote %.3f ppm to %s\n", avgPpm, path);
        printf("ft8 will now transmit aligned to the RTL-SDR TCXO reference.\n");
    } else {
        fprintf(stderr, "Error: could not write the cal file.\n");
        return 1;
    }
    return 0;
}
