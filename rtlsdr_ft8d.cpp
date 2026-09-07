/*
 * rtlsrd-ft8d, FT8 daemon for RTL receivers
 * Copyright (C) 2016-2021, Guenael Jouchet (VA2GKA)
 * Copyright (C) 2023 Claudio Porfiri (SA0PRF)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <rtl-sdr.h>
#include <fftw3.h>
#include <sys/socket.h>
#include <netdb.h>
#include <curl/curl.h>

#include <rtlsdr_ft8d.h>

#include <ft8/constants.h>
#include <ft8/ldpc.h>
#include <ft8/crc.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <common/wave.h>
#include <common/monitor.h>
#include <common/audio.h>

#include <pskreporter.hpp>
#include <ft8_ncurses.h>

#include <qsoHandler.h>

#include <tsqueue.h>

/* Defines for debug */
// #define TXWINTEST

/* End defines for debug */

void printSpots(uint32_t n_results);

/* Thread for decoding */
static struct decoder_thread decThread;

/* QSO Thread */
pthread_t qsoThread;

/* PSK reporter thread */
pthread_t pskThread;

/* CQHandler window thread */
pthread_t CQHThread;

/* Keyboard Handler thread */
pthread_t KBHThread;

/* Transmission Handler thread */
pthread_t TXHThread;

/* Thread for RX (blocking function used) & RTL struct */
static pthread_t rxThread;
static rtlsdr_dev_t *rtl_device = NULL;

/* FFTW pointers & stuff */
static fftwf_plan fft_plan;
static fftwf_complex *fft_in, *fft_out;
static FILE *fp_fftw_wisdom_file;
static float *hann;

volatile bool exitPskThread = false;

/* Global declaration for states & options, shared with other external objects */

struct receiver_state rx_state;
struct receiver_options rx_options;
struct decoder_options dec_options;
struct decoder_results dec_results[50];
vector<struct decoder_results> dec_results_queue;
vector<struct decoder_results> cq_queue;
vector<struct plain_message> qso_queue;
vector<struct plain_message> log_queue;

/* QSOHandler Threads */
vector<struct tick_message> tick_queue;
vector<struct plain_message> qsoh_queue;

/* mutex for thread sync */
pthread_mutex_t msglock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t CQlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t QSOlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t LOGlock = PTHREAD_MUTEX_INITIALIZER;

/* QSOHandler mutexes */
pthread_mutex_t Ticklock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t QSOHlock = PTHREAD_MUTEX_INITIALIZER;

/* Could be nice to update this one with the CI */
const char *rtlsdr_ft8d_version = "0.8.3";
char pskreporter_app_version[] = "rtlsdr-ft8d_v0.8.3";

static volatile int callback_counter = 0;
static volatile int callback_cnt_old = 0;

/* Callback for each buffer received */
static void rtlsdr_callback(unsigned char *samples, uint32_t samples_count, void *ctx) {
    int8_t *sigIn = (int8_t *)samples;

    /* CIC buffers/vars */
    static int32_t Ix1 = 0, Ix2 = 0,
                   Qx1 = 0, Qx2 = 0;
    static int32_t Iy1 = 0, It1y = 0, It1z = 0,
                   Qy1 = 0, Qt1y = 0, Qt1z = 0;
    static int32_t Iy2 = 0, It2y = 0, It2z = 0,
                   Qy2 = 0, Qt2y = 0, Qt2z = 0;
    static uint32_t decimationIndex = 0;

    /* FIR compensation filter coefs
       Using : Octave/MATLAB code for generating compensation FIR coefficients
       URL : https://github.com/WestCoastDSP/CIC_Octave_Matlab
     */

    /* Coefs with R=750, M=1, N=2, F0=0.92, L=54 */
    const static float zCoef[FIR_TAPS + 1] = {
        -0.0025719973, 0.0010118403, 0.0009110571, -0.0034940765,
        0.0069713409, -0.0114242790, 0.0167023466, -0.0223683056,
        0.0276808966, -0.0316243672, 0.0329894230, -0.0305042011,
        0.0230074504, -0.0096499429, -0.0098950502, 0.0352349632,
        -0.0650990428, 0.0972406918, -0.1284211497, 0.1544893973,
        -0.1705667465, 0.1713383321, -0.1514501610, 0.1060148823,
        -0.0312560926, -0.0745846391, 0.2096088743, -0.3638689868,
        0.5000000000,
        -0.3638689868, 0.2096088743, -0.0745846391, -0.0312560926,
        0.1060148823, -0.1514501610, 0.1713383321, -0.1705667465,
        0.1544893973, -0.1284211497, 0.0972406918, -0.0650990428,
        0.0352349632, -0.0098950502, -0.0096499429, 0.0230074504,
        -0.0305042011, 0.0329894230, -0.0316243672, 0.0276808966,
        -0.0223683056, 0.0167023466, -0.0114242790, 0.0069713409,
        -0.0034940765, 0.0009110571, 0.0010118403, -0.0025719973};

    /* FIR compensation filter buffers */
    static float firI[FIR_TAPS] = {0.0},
                 firQ[FIR_TAPS] = {0.0};

    /* Economic mixer @ fs/4 (upper band)
       At fs/4, sin and cosin calculations are no longer required.

               0   | pi/2 |  pi  | 3pi/2
             ----------------------------
       sin =   0   |  1   |  0   |  -1  |
       cos =   1   |  0   | -1   |   0  |

       out_I = in_I * cos(x) - in_Q * sin(x)
       out_Q = in_Q * cos(x) + in_I * sin(x)
       (Keep the upper band, IQ inverted on RTL devices)
    */
    int8_t tmp;
    for (uint32_t i = 0; i < samples_count; i += 8) {
        sigIn[i] ^= 0x80;             // Unsigned to signed conversion using
        sigIn[i + 1] ^= 0x80;         //   XOR as a binary mask to flip the first bit
        tmp = (sigIn[i + 3] ^ 0x80);  // CHECK -127 alt. speed
        sigIn[i + 3] = (sigIn[i + 2] ^ 0x80);
        sigIn[i + 2] = -tmp;
        sigIn[i + 4] = -(sigIn[i + 4] ^ 0x80);
        sigIn[i + 5] = -(sigIn[i + 5] ^ 0x80);
        tmp = (sigIn[i + 6] ^ 0x80);
        sigIn[i + 6] = (sigIn[i + 7] ^ 0x80);
        sigIn[i + 7] = -tmp;
    }

    /* CIC decimator (N=2)
       Info: * Understanding CIC Compensation Filters
               https://www.altera.com/en_US/pdfs/literature/an/an455.pdf
             * Understanding cascaded integrator-comb filters
               http://www.embedded.com/design/configurable-systems/4006446/Understanding-cascaded-integrator-comb-filters
    */
    for (int32_t i = 0; i < samples_count / 2; i++) {  // UPDATE: i+=2 & fix below
        /* Integrator stages (N=2) */
        Ix1 += (int32_t)sigIn[i * 2];  // EVAL: option to move sigIn in float here
        Qx1 += (int32_t)sigIn[i * 2 + 1];
        Ix2 += Ix1;
        Qx2 += Qx1;

        /* Decimation stage */
        decimationIndex++;
        if (decimationIndex <= DOWNSAMPLING) {
            continue;
        }
        decimationIndex = 0;

        /* 1st Comb */
        Iy1 = Ix2 - It1z;
        It1z = It1y;
        It1y = Ix2;
        Qy1 = Qx2 - Qt1z;
        Qt1z = Qt1y;
        Qt1y = Qx2;

        /* 2nd Comd */
        Iy2 = Iy1 - It2z;
        It2z = It2y;
        It2y = Iy1;
        Qy2 = Qy1 - Qt2z;
        Qt2z = Qt2y;
        Qt2y = Qy1;

        /* FIR compensation filter */
        float Isum = 0.0,
              Qsum = 0.0;
        for (uint32_t j = 0; j < FIR_TAPS; j++) {
            Isum += firI[j] * zCoef[j];
            Qsum += firQ[j] * zCoef[j];
            if (j < FIR_TAPS - 1) {
                firI[j] = firI[j + 1];
                firQ[j] = firQ[j + 1];
            }
        }
        firI[FIR_TAPS - 1] = (float)Iy2;
        firQ[FIR_TAPS - 1] = (float)Qy2;
        Isum += firI[FIR_TAPS - 1] * zCoef[FIR_TAPS];
        Qsum += firQ[FIR_TAPS - 1] * zCoef[FIR_TAPS];

        /* Save the result in the buffer */
        uint32_t idx = rx_state.bufferIndex;
        if (rx_state.iqIndex[idx] < (SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE)) {
            rx_state.iSamples[idx][rx_state.iqIndex[idx]] = Isum / (32768.0 * DOWNSAMPLING);
            rx_state.qSamples[idx][rx_state.iqIndex[idx]] = Qsum / (32768.0 * DOWNSAMPLING);
            rx_state.iqIndex[idx]++;
        }
    }
    callback_counter++;
}

static void sigint_callback_handler(int signum) {
    fprintf(stderr, "Signal caught %d, exiting!\n", signum);
    rx_state.exit_flag = true;
    if (rtl_device)
        rtlsdr_cancel_async(rtl_device);
}

/* Thread used for this RX blocking function */
static void *rtlsdr_rx(void *arg) {
    rtlsdr_read_async(rtl_device, rtlsdr_callback, NULL, 0, DEFAULT_BUF_LENGTH);
    rtlsdr_cancel_async(rtl_device);
    return NULL;
}

/*
 * Synthetic RX source used by -rx-test (no real RTL-SDR needed).
 *
 * FT8 is slot-based: each 15-second window carries one ~12.6 s transmission,
 * and the decoder processes one window at a time. This helper fills one buffer
 * with a whole window worth of traffic -- a varying 4 to 5 FT8 signals summed
 * together at distinct audio frequencies (as a real band would present them),
 * mixing plain CQ calls and messages directed at our own callsign so the QSO
 * state machine can be exercised too. Callsigns are of the A1TEST family.
 *
 * It is invoked once per slot by the (unchanged) 15 s main loop, in place of
 * the RTL sampler, so the real slot timing and the rest of the decode -> UI
 * -> QSO -> reporting pipeline are exercised exactly as with a real device.
 */
static void fillRxTestBuffer(uint32_t idx) {
    static const char *calls[] = {"A1TEST", "A2TEST", "A3TEST", "A4TEST",
                                  "A5TEST", "A6TEST", "A7TEST", "A8TEST"};
    static const char *grids[] = {"JO65", "IO91", "FN20", "JN58",
                                  "EM12", "JO99", "IO83", "KP20"};
    const int nCalls = (int)(sizeof(calls) / sizeof(calls[0]));

    /* Audio frequencies spread across the passband, one slot per signal */
    const float audioFreqs[] = {500.0f, 900.0f, 1300.0f, 1700.0f, 2100.0f};
    const int nFreqs = (int)(sizeof(audioFreqs) / sizeof(audioFreqs[0]));

    static uint32_t rot = 0; /* rotates the station set between slots */

    float *iS = rx_state.iSamples[idx];
    float *qS = rx_state.qSamples[idx];

    /* Clear the whole window buffer first */
    for (int i = 0; i < SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE; i++) {
        iS[i] = 0.0f;
        qS[i] = 0.0f;
    }

    /* 4 or 5 signals this slot, varying per slot */
    int nSignals = 4 + (rot % 2);  // 4 or 5

    const char *myCall = dec_options.rcall[0] ? dec_options.rcall : "A0TEST";

    for (int s = 0; s < nSignals; s++) {
        const char *call = calls[(rot + s) % nCalls];
        const char *grid = grids[(rot + s) % nCalls];
        float audio = audioFreqs[s % nFreqs];

        char message[32];
        /* Mix CQ calls with messages directed at us (for the QSO machine) */
        switch ((rot + s) % 4) {
            case 0:
                snprintf(message, sizeof(message), "CQ %s %s", call, grid);
                break;
            case 1:
                snprintf(message, sizeof(message), "%s %s %s", myCall, call, grid);
                break;
            case 2:
                snprintf(message, sizeof(message), "%s %s -12", myCall, call);
                break;
            default:
                snprintf(message, sizeof(message), "%s %s RR73", myCall, call);
                break;
        }

        /* Sum this signal into the window at a per-signal amplitude so the
           test exercises a range of SNRs (real bands have varied signal
           strengths). Amplitudes span roughly a 4:1 range. */
        /* Sum this signal into the window at a per-signal amplitude so the
           synthetic traffic spans a realistic range of SNRs (like a real band
           with strong and weak stations). */
        float amp = 0.15f + 0.12f * (float)(s % 5);  // ~0.15 .. 0.63
        genFT8Signal(iS, qS, message, audio, amp, 0.02f);
    }

    rx_state.iqIndex[idx] = SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE;
    rot++;
}

/* Thread used for the decoder */
static void *decoder(void *arg) {
    int32_t n_results = 0;

    while (!rx_state.exit_flag) {
        safe_cond_wait(&decThread.ready_cond, &decThread.ready_mutex);

        LOG(LOG_DEBUG, "Decoder thread -- Got a signal!\n");

        if (rx_state.exit_flag)
            break; /* Abort case, final sig */

        /* Select the previous transmission / other buffer */
        uint32_t prevBuffer = (rx_state.bufferIndex + 1) % 2;

        if (rx_state.iqIndex[prevBuffer] < ((SIGNAL_LENGHT - 3) * SIGNAL_SAMPLE_RATE)) {
            LOG(LOG_DEBUG, "Decoder thread -- Signal too short, skipping!\n");
            continue; /* Partial buffer during the first RX, skip it! */
        } else {
            rx_options.nloop++; /* Decoding this signal, count it! */
        }

        /* Delete any previous samples tail */
        for (int i = rx_state.iqIndex[prevBuffer]; i < SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE; i++) {
            rx_state.iSamples[prevBuffer][i] = 0.0;
            rx_state.qSamples[prevBuffer][i] = 0.0;
        }

        /* Normalize the sample @-3dB */
        float maxSig = 1e-24f;
        for (int i = 0; i < SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE; i++) {
            float absI = fabs(rx_state.iSamples[prevBuffer][i]);
            float absQ = fabs(rx_state.qSamples[prevBuffer][i]);

            if (absI > maxSig)
                maxSig = absI;
            if (absQ > maxSig)
                maxSig = absQ;
        }
        maxSig = 0.5 / maxSig;
        for (int i = 0; i < SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE; i++) {
            rx_state.iSamples[prevBuffer][i] *= maxSig;
            rx_state.qSamples[prevBuffer][i] *= maxSig;
        }

        /* Get the date at the beginning last recording session
           with 1 second margin added, just to be sure to be on 15 alignment
        */
        time_t unixtime;
        time(&unixtime);
        unixtime = unixtime - 15 + 1;
        gmtime_r(&unixtime, &rx_state.gtm);

        /* Search & decode the signal */
        ft8_subsystem(rx_state.iSamples[prevBuffer],
                      rx_state.qSamples[prevBuffer],
                      SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE,
                      dec_results,
                      &n_results);
        LOG(LOG_DEBUG, "Decoder thread -- Decoding completed\n");
        saveSample(rx_state.iSamples[prevBuffer], rx_state.qSamples[prevBuffer]);
        postSpots(n_results);
        printSpots(n_results);
    }
    return NULL;
}

/* Double buffer management */
void initSampleStorage() {
    rx_state.bufferIndex = 0;
    rx_state.iqIndex[0] = 0;
    rx_state.iqIndex[1] = 0;
    rx_state.exit_flag = false;
}

/* Default options for the receiver */
void initrx_options() {
    rx_options.gain = 290;
    rx_options.autogain = 0;
    rx_options.ppm = 0;
    rx_options.shift = 0;
    rx_options.directsampling = 0;
    rx_options.maxloop = 0;
    rx_options.nloop = 0;
    rx_options.device = 0;
    rx_options.selftest = false;
    rx_options.writefile = false;
    rx_options.readfile = false;
    rx_options.noreport = false;  // When debugging no report
    rx_options.qso = true;
    rx_options.isHF = false;
    rx_options.directset = false;
    rx_options.rxtest = false;
    rx_options.rtlgen = rtlAuto;
}

void initFFTW() {
    /* Recover existing FFTW optimisations */
    if ((fp_fftw_wisdom_file = fopen("fftw_wisdom.dat", "r"))) {
        fftwf_import_wisdom_from_file(fp_fftw_wisdom_file);
        fclose(fp_fftw_wisdom_file);
    }

    /* Allocate FFT buffers */
    fft_in = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * NFFT);
    fft_out = (fftwf_complex *)fftwf_malloc(sizeof(fftwf_complex) * NFFT);

    /* FFTW internal plan */
    fft_plan = fftwf_plan_dft_1d(NFFT, fft_in, fft_out, FFTW_FORWARD, PATIENCE);

    /* Calculate Hann function only one time
     * https://en.wikipedia.org/wiki/Hann_function
     */
    hann = (float *)malloc(sizeof(float) * NFFT);
    for (int i = 0; i < NFFT; i++) {
        hann[i] = sinf((M_PI / NFFT) * i);
    }
}

void freeFFTW() {
    fftwf_free(fft_in);
    fftwf_free(fft_out);

    if ((fp_fftw_wisdom_file = fopen("fftw_wisdom.dat", "w"))) {
        fftwf_export_wisdom_to_file(fp_fftw_wisdom_file);
        fclose(fp_fftw_wisdom_file);
    }
    fftwf_destroy_plan(fft_plan);
}

inline uint16_t SwapEndian16(uint16_t val) {
    return (val << 8) | (val >> 8);
}

inline uint32_t SwapEndian32(uint32_t val) {
    return (val << 24) | ((val << 8) & 0x00ff0000) | ((val >> 8) & 0x0000ff00) | (val >> 24);
}

bool exitFlag(void) {
    return (rx_state.exit_flag == true);
}

PskReporter *reporter = NULL;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_REPORTS_PER_PACKET 64

void *pskUploader(void *vargp) {
    while (exitFlag() == false) {
        /* Wait ~60s between batches, checking the exit flag every second */
        for (int i = 0; i < 60 && !exitFlag(); i++)
            sleep(1);

        if (exitFlag())
            break;

        /* If reporting is disabled or the reporter was never constructed,
           drain the queue (under the lock) so it cannot grow unbounded, but
           do not touch the NULL reporter. */
        if (rx_options.noreport || reporter == NULL) {
            struct decoder_results dr;
            while (tsq_pop(dec_results_queue, &lock, &dr)) {
                /* discard */
            }
            continue;
        }

        /* Drain the queue in packets of up to MAX_REPORTS_PER_PACKET.
           Each pop is done atomically under the lock via tsq_pop(). */
        struct decoder_results dr;
        int inPacket = 0;
        bool anyInPacket = false;

        while (tsq_pop(dec_results_queue, &lock, &dr)) {
            reporter->addReceiveRecord(dr.call, dr.freq, dr.snr);
            anyInPacket = true;
            if (++inPacket >= MAX_REPORTS_PER_PACKET) {
                reporter->send();
                inPacket = 0;
                anyInPacket = false;
            }
        }
        if (anyInPacket)
            reporter->send();
    }

    return NULL;
}

void closePskThread(void) {
    exitPskThread = true;
}

/* PSKreporter protocol documentation & links:
 *   https://pskreporter.info/pskdev.html
 *   https://pskreporter.info/cgi-bin/psk-analysis.pl
 *   https://pskreporter.info/pskmap.html
 */
void postSpots(uint32_t n_results) {
    if (rx_options.noreport) {
        LOG(LOG_DEBUG, "Decoder thread -- Skipping the reporting\n");
        return;
    }

    /* Fixed strings for Mode */
    const char txMode[] = "FT8";

    for (uint32_t i = 0; i < n_results; i++) {
        struct decoder_results dr;

        snprintf(dr.call, sizeof(dr.call), "%.12s", dec_results[i].call);
        dr.freq = dec_results[i].freq + dec_options.freq;
        dr.snr = dec_results[i].snr;  // real SNR estimate (dB)
        tsq_push(dec_results_queue, &lock, dr);
    }
}

/* Report on a WebCluster -- Ex. RBN Network */
void webClusterSpots(uint32_t n_results) {
    if (rx_options.noreport) {
        LOG(LOG_DEBUG, "Decoder thread -- Skipping the reporting\n");
        return;
    }

    /* No spot to report, simply skip */
    if (n_results == 0) {
        return;
    }

    CURL *curl;
    CURLcode res;

    char myCall[16];
    char dxCall[12];
    char dxFreq[10];
    char info[100];

    for (uint32_t i = 0; i < n_results; i++) {
        snprintf(myCall, sizeof(myCall), "%s", dec_options.rcall);
        snprintf(dxFreq, sizeof(dxFreq), "%8f", (float)(dec_results[i].freq + dec_options.freq) / 1000.0f);
        snprintf(dxCall, sizeof(dxCall), "%s", dec_results[i].call);
        snprintf(info, sizeof(info), "M2M FT8 [%s - %s]", dec_options.rloc, dec_results[i].loc);

        curl_global_init(CURL_GLOBAL_ALL);
        curl = curl_easy_init();
        if (curl) {
            curl_mime *mime = curl_mime_init(curl);
            curl_mimepart *part;

            part = curl_mime_addpart(mime);
            curl_mime_name(part, "_mycall");
            curl_mime_data(part, myCall, CURL_ZERO_TERMINATED);

            part = curl_mime_addpart(mime);
            curl_mime_name(part, "_dxcall");
            curl_mime_data(part, dxCall, CURL_ZERO_TERMINATED);

            part = curl_mime_addpart(mime);
            curl_mime_name(part, "_freq");
            curl_mime_data(part, dxFreq, CURL_ZERO_TERMINATED);

            part = curl_mime_addpart(mime);
            curl_mime_name(part, "_info");
            curl_mime_data(part, info, CURL_ZERO_TERMINATED);

            curl_easy_setopt(curl, CURLOPT_URL, "http://mycluster.localhost/sends.php");
            curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
            res = curl_easy_perform(curl);

            if (res != CURLE_OK)
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

            curl_mime_free(mime);
            curl_easy_cleanup(curl);
        }
    }
}

void printSpots(uint32_t n_results) {
    /*   if (n_results == 0) {
            mvwprintw(logw, 1, 2, "No spot %04d-%02d-%02d %02d:%02dz\n",
                      rx_state.gtm->tm_year + 1900,
                      rx_state.gtm->tm_mon + 1,
                      rx_state.gtm->tm_mday,
                      rx_state.gtm->tm_hour,
                      rx_state.gtm->tm_min);
            wrefresh(logw);
            return;
        }
    */

    for (uint32_t i = 0; i < n_results; i++) {
        /* Rather than printing the results, we exploit a queue */
        struct decoder_results dr;

        snprintf(dr.call, sizeof(dr.call), "%.12s", dec_results[i].call);
        snprintf(dr.cmd, sizeof(dr.cmd), "%s", dec_results[i].cmd);
        dr.freq = dec_results[i].freq + dec_options.freq;
        dr.snr = dec_results[i].snr;
        dr.tempus = dec_results[i].tempus;
        tsq_push(cq_queue, &CQlock, dr);
    }
}

void saveSample(float *iSamples, float *qSamples) {
    if (rx_options.writefile == true) {
        char filename[32];

        time_t rawtime;
        time(&rawtime);
        struct tm gtmv;
        struct tm *gtm = gmtime_r(&rawtime, &gtmv);

        snprintf(filename, sizeof(filename) - 1, "%.8s_%04d-%02d-%02d_%02d-%02d-%02d.iq",
                 rx_options.filename,
                 gtm->tm_year + 1900,
                 gtm->tm_mon + 1,
                 gtm->tm_mday,
                 gtm->tm_hour,
                 gtm->tm_min,
                 gtm->tm_sec);

        writeRawIQfile(iSamples, qSamples, filename);
    }
}

double atofs(char *s) {
    /* standard suffixes */
    char last;
    uint32_t len;
    double suff = 1.0;
    len = strlen(s);
    if (len == 0)  /* Guard against s[-1] OOB access on empty argument */
        return 0.0;
    last = s[len - 1];
    s[len - 1] = '\0';

    switch (last) {
        case 'g':
        case 'G':
            suff *= 1e3;
        case 'm':
        case 'M':
            suff *= 1e3;
        case 'k':
        case 'K':
            suff *= 1e3;
            suff *= atof(s);
            s[len - 1] = last;
            return suff;
    }
    s[len - 1] = last;
    return atof(s);
}

int32_t parse_u64(char *s, uint64_t *const value) {
    uint_fast8_t base = 10;
    char *s_end;
    uint64_t u64_value;

    if (strlen(s) > 2) {
        if (s[0] == '0') {
            if ((s[1] == 'x') || (s[1] == 'X')) {
                base = 16;
                s += 2;
            } else if ((s[1] == 'b') || (s[1] == 'B')) {
                base = 2;
                s += 2;
            }
        }
    }

    s_end = s;
    u64_value = strtoull(s, &s_end, base);
    if ((s != s_end) && (*s_end == 0)) {
        *value = u64_value;
        return 1;
    } else {
        return 0;
    }
}

int32_t readRawIQfile(float *iSamples, float *qSamples, char *filename) {
    float filebuffer[2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE];
    FILE *fd = fopen(filename, "rb");

    if (fd == NULL) {
        fprintf(stderr, "Cannot open data file...\n");
        return 0;
    }

    /* Read the IQ file */
    int32_t nread = fread(filebuffer, sizeof(float), 2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE, fd);
    int32_t recsize = nread / 2;

    /* Convert the interleaved buffer into 2 buffers */
    for (int32_t i = 0; i < recsize; i++) {
        iSamples[i] = filebuffer[2 * i];
        qSamples[i] = -filebuffer[2 * i + 1];  // neg, convention used by wsprsim
    }

    /* Normalize the sample @-3dB */
    float maxSig = 1e-24f;
    for (int i = 0; i < recsize; i++) {
        float absI = fabs(iSamples[i]);
        float absQ = fabs(qSamples[i]);

        if (absI > maxSig)
            maxSig = absI;
        if (absQ > maxSig)
            maxSig = absQ;
    }
    maxSig = 0.5 / maxSig;
    for (int i = 0; i < recsize; i++) {
        iSamples[i] *= maxSig;
        qSamples[i] *= maxSig;
    }

    return recsize;
}

int32_t writeRawIQfile(float *iSamples, float *qSamples, char *filename) {
    float filebuffer[2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE];

    FILE *fd = fopen(filename, "wb");
    if (fd == NULL) {
        fprintf(stderr, "Cannot open data file...\n");
        return 0;
    }

    for (int32_t i = 0; i < SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE; i++) {
        filebuffer[2 * i] = iSamples[i];
        filebuffer[2 * i + 1] = -qSamples[i];  // neg, convention used by wsprsim
    }

    int32_t nwrite = fwrite(filebuffer, sizeof(float), 2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE, fd);
    if (nwrite != 2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE) {
        fprintf(stderr, "Cannot write all the data!\n");
        return 0;
    }

    fclose(fd);
    return SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE;
}

int32_t readC2file(float *iSamples, float *qSamples, char *filename) {
    float filebuffer[2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE];
    FILE *fd = fopen(filename, "rb");
    int32_t nread;
    double frequency;
    int type;
    char name[15];

    if (fd == NULL) {
        fprintf(stderr, "Cannot open data file...\n");
        return 0;
    }

    /* Read the header */
    nread = fread(name, sizeof(char), 14, fd);
    nread = fread(&type, sizeof(int), 1, fd);
    nread = fread(&frequency, sizeof(double), 1, fd);
    rx_options.dialfreq = frequency;

    /* Read the IQ file */
    nread = fread(filebuffer, sizeof(float), 2 * SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE, fd);
    int32_t recsize = nread / 2;

    /* Convert the interleaved buffer into 2 buffers */
    for (int32_t i = 0; i < recsize; i++) {
        iSamples[i] = filebuffer[2 * i];
        qSamples[i] = -filebuffer[2 * i + 1];  // neg, convention used by wsprsim
    }

    /* Normalize the sample @-3dB */
    float maxSig = 1e-24f;
    for (int i = 0; i < recsize; i++) {
        float absI = fabs(iSamples[i]);
        float absQ = fabs(qSamples[i]);

        if (absI > maxSig)
            maxSig = absI;
        if (absQ > maxSig)
            maxSig = absQ;
    }
    maxSig = 0.5 / maxSig;
    for (int i = 0; i < recsize; i++) {
        iSamples[i] *= maxSig;
        qSamples[i] *= maxSig;
    }

    return recsize;
}

void decodeRecordedFile(char *filename) {
    static float iSamples[SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE] = {0};
    static float qSamples[SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE] = {0};
    static uint32_t samples_len;
    int32_t n_results = 0;

    if (strlen(filename) < 3) {
        fprintf(stderr, "Not a valid filename!! (need .iq or .c2 extension)\n");
        return;
    }

    if (strcmp(&filename[strlen(filename) - 3], ".iq") == 0) {
        samples_len = readRawIQfile(iSamples, qSamples, filename);
    } else if (strcmp(&filename[strlen(filename) - 3], ".c2") == 0) {
        samples_len = readC2file(iSamples, qSamples, filename);
    } else {
        fprintf(stderr, "Not a valid extension!! (only .iq & .c2 files)\n");
        return;
    }

    printf("Number of samples: %d\n", samples_len);

    if (samples_len) {
        /* Search & decode the signal */
        ft8_subsystem(iSamples, qSamples, samples_len, dec_results, &n_results);

        time_t unixtime;
        time(&unixtime);
        unixtime = unixtime - 120 + 1;
        gmtime_r(&unixtime, &rx_state.gtm);

        printSpots(n_results);
    }
}

float whiteGaussianNoise(float factor) {
    static double V1, V2, U1, U2, S, X;
    static int phase = 0;

    if (phase == 0) {
        do {
            U1 = rand() / (double)RAND_MAX;
            U2 = rand() / (double)RAND_MAX;
            V1 = 2 * U1 - 1;
            V2 = 2 * U2 - 1;
            S = V1 * V1 + V2 * V2;
        } while (S >= 1 || S == 0);

        X = V1 * sqrt(-2 * log(S) / S);
    } else {
        X = V2 * sqrt(-2 * log(S) / S);
    }

    phase = 1 - phase;
    return (float)X * factor;
}

/*
 * Generate an FT8 signal for the text `message` and ADD it into the I/Q
 * buffers at the given audio frequency (Hz). Used by the self-test and the
 * synthetic -rx-test source. Because it adds (does not overwrite), several
 * signals can be summed into the same buffer.
 *
 * Returns true on success, false if the message could not be encoded.
 */
/* Callsign hash interface (defined below), used by genFT8Signal to register
   nonstandard callsigns at encode time */
extern ftx_callsign_hash_interface_t hash_if;

bool genFT8Signal(float *iSamples, float *qSamples, const char *message,
                  float audioFreq, float amp, float wgn) {
    /* Pack the text into a binary message. Passing the callsign hash interface
       lets nonstandard/compound calls (e.g. A1TEST) be registered in the hash
       table at encode time, so the decoder can later resolve their hashes to
       text instead of printing "<...>". */
    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode(&msg, &hash_if, message);
    if (rc != FTX_MESSAGE_RC_OK)
        return false;

    /* Encode the binary message as a sequence of FSK tones */
    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    const double df = 3200.0 / 512.0;  // tone spacing (6.25 Hz)
    const double dt = 1.0 / 3200.0;    // sample period
    double phi = 0.0;

    for (int i = 0; i < FT8_NN; i++) {
        double dphi = 2.0 * M_PI * dt * (audioFreq + ((double)tones[i] - 3.5) * df);
        for (int j = 0; j < 512; j++) {
            int index = 512 * i + j;
            if (index >= SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE)
                break;  // Never overflow the buffer
            iSamples[index] += amp * cos(phi) + whiteGaussianNoise(wgn);
            qSamples[index] += amp * sin(phi) + whiteGaussianNoise(wgn);
            phi += dphi;
        }
    }
    return true;
}

int32_t decoderSelfTest() {
    static float iSamples[SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE] = {0};
    static float qSamples[SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE] = {0};
    static uint32_t samples_len = SIGNAL_LENGHT * SIGNAL_SAMPLE_RATE;
    int32_t n_results = 0;

    /* Ref test message
     * Message : "CQ K1JT FN20QI"
     * Packed data: 00 00 00 20 4d fc dc 8a 14 08
     * FSK tones: 3140652000000001005477547106035036373140652547441342116056460065174427143140652
     */
    const char message[] = "CQ K1JT FN20QI";

    /* Build one FT8 signal at 50 Hz audio into the (zeroed) buffers */
    if (!genFT8Signal(iSamples, qSamples, message, 50.0f, 0.5f, 0.02f)) {
        printf("Cannot parse message!\n");
        return -2;
    }

    /* Save the test sample */
    writeRawIQfile(iSamples, qSamples, (char *)"selftest.iq");

    /* Search & decode the signal */
    ft8_subsystem(iSamples, qSamples, samples_len, dec_results, &n_results);

    printSpots(n_results);

    /* Simple consistency check: success only when BOTH callsign and locator
       match the reference decode. strcmp() returns 0 on match, so use !strcmp. */
    if (!strcmp(dec_results[0].call, "K1JT") &&
        !strcmp(dec_results[0].loc, "FN20")) {
        return 1;
    } else {
        return 0;
    }
}

/*
 * Callsign hash table.
 *
 * FT8 sends nonstandard / compound callsigns (and the "to" call in some
 * message types) as a 22/12/10-bit hash rather than as text. The full call is
 * only recoverable if it has been seen before and stored. ft8_lib delegates
 * this storage to us via the ftx_callsign_hash_interface_t:
 *   - save_hash(callsign, n22): store the full call under its 22-bit hash.
 *   - lookup_hash(type, hash, out): given a 22/12/10-bit hash, return the call.
 *
 * The 12- and 10-bit hashes are simply n22 >> 10 and n22 >> 12 (see
 * ft8_lib/ft8/message.c save_callsign()), so storing n22 lets us answer all
 * three query widths. This was previously stubbed out; implementing it lets
 * A1TEST-style nonstandard calls resolve instead of printing "<...>".
 */
#define CALLSIGN_HASH_TABLE_SIZE 256

struct callsign_hash_entry {
    bool used;
    uint32_t n22;
    char callsign[12];  // up to 11 chars + NUL
};

static struct callsign_hash_entry callsign_hashtable[CALLSIGN_HASH_TABLE_SIZE];
static pthread_mutex_t callsign_hash_lock = PTHREAD_MUTEX_INITIALIZER;

void hashtable_init(void) {
    pthread_mutex_lock(&callsign_hash_lock);
    for (int i = 0; i < CALLSIGN_HASH_TABLE_SIZE; i++)
        callsign_hashtable[i].used = false;
    pthread_mutex_unlock(&callsign_hash_lock);
}

void hashtable_cleanup(uint8_t max_age) {
    /* Ageing is not tracked; kept for interface compatibility */
    (void)max_age;
}

void hashtable_add(const char *callsign, uint32_t hash) {
    if (callsign == NULL || callsign[0] == '\0')
        return;

    pthread_mutex_lock(&callsign_hash_lock);

    /* Linear-probe insert keyed on the 22-bit hash; update if already present */
    uint32_t start = hash % CALLSIGN_HASH_TABLE_SIZE;
    for (uint32_t i = 0; i < CALLSIGN_HASH_TABLE_SIZE; i++) {
        uint32_t idx = (start + i) % CALLSIGN_HASH_TABLE_SIZE;
        if (!callsign_hashtable[idx].used) {
            callsign_hashtable[idx].used = true;
            callsign_hashtable[idx].n22 = hash;
            snprintf(callsign_hashtable[idx].callsign,
                     sizeof(callsign_hashtable[idx].callsign), "%s", callsign);
            break;
        }
        if (callsign_hashtable[idx].n22 == hash) {
            /* Refresh the stored text for this hash */
            snprintf(callsign_hashtable[idx].callsign,
                     sizeof(callsign_hashtable[idx].callsign), "%s", callsign);
            break;
        }
    }

    pthread_mutex_unlock(&callsign_hash_lock);
}

bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char *callsign) {
    /* Reduce each stored 22-bit hash to the requested width and compare.
       n12 = n22 >> 10, n10 = n22 >> 12 (see ft8_lib save_callsign()). */
    bool found = false;

    pthread_mutex_lock(&callsign_hash_lock);
    for (int i = 0; i < CALLSIGN_HASH_TABLE_SIZE; i++) {
        if (!callsign_hashtable[i].used)
            continue;

        uint32_t stored;
        switch (hash_type) {
            case FTX_CALLSIGN_HASH_22_BITS:
                stored = callsign_hashtable[i].n22 & 0x3FFFFFu;
                break;
            case FTX_CALLSIGN_HASH_12_BITS:
                stored = (callsign_hashtable[i].n22 >> 10) & 0xFFFu;
                break;
            case FTX_CALLSIGN_HASH_10_BITS:
            default:
                stored = (callsign_hashtable[i].n22 >> 12) & 0x3FFu;
                break;
        }

        if (stored == hash) {
            snprintf(callsign, 12, "%s", callsign_hashtable[i].callsign);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&callsign_hash_lock);

    return found;
}

ftx_callsign_hash_interface_t hash_if = {
    .lookup_hash = hashtable_lookup,
    .save_hash = hashtable_add};

/*
 * Strip the angle brackets that ft8_lib puts around a callsign recovered from
 * a hash lookup (e.g. "<A6TEST>" -> "A6TEST"). Returns a pointer to a cleaned
 * copy in the caller-provided buffer, so callsign fields, QSO comparisons and
 * ADIF logs hold the bare call. Non-bracketed tokens are copied unchanged.
 */
static const char *stripBrackets(const char *token, char *out, size_t outsz) {
    if (token == NULL) {
        if (outsz)
            out[0] = '\0';
        return out;
    }
    const char *start = token;
    size_t len = strlen(token);
    if (len >= 2 && start[0] == '<' && start[len - 1] == '>') {
        start++;
        len -= 2;
    }
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

/*
 * Compute a robust noise-floor power for the whole slot: the median of all
 * waterfall bin powers. Signals occupy only a small fraction of the
 * time/frequency cells, so the median is dominated by noise and is immune to
 * signals scaling up/down (unlike using a candidate's neighbouring tone bins,
 * which get contaminated by other signals). Returned as linear power.
 */
static double noiseFloorPower(const monitor_t *mon) {
    const ftx_waterfall_t *wf = &mon->wf;
    static float samples[8192];
    int n = 0;
    long total = (long)wf->num_blocks * wf->block_stride;
    int step = (int)(total / 8192);
    if (step < 1)
        step = 1;
    for (long i = 0; i < total && n < 8192; i += step) {
        samples[n++] = (float)wf->mag[i] * 0.5f - 120.0f;  // dB
    }
    if (n == 0)
        return 1e-12;

    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (samples[j] < samples[i]) {
                float t = samples[i];
                samples[i] = samples[j];
                samples[j] = t;
            }
    float medianDb = samples[n / 2];
    return pow(10.0, medianDb / 10.0);
}

/*
 * Estimate the SNR (in dB, referenced to a 2500 Hz noise bandwidth, WSJT-X
 * convention) for a decoded candidate.
 *
 * The previous code reported (cand->score - 20), the Costas sync correlation
 * score, which is not an SNR and clusters in a narrow range.
 *
 * Signal power is measured at the KNOWN Costas sync tones (21 symbols in 3
 * groups at symbol indices 0..6, 36..42, 72..78), averaging the power in the
 * true tone bin. Noise power is the robust slot-wide median floor passed in.
 * SNR = 10*log10((Psig - Pnoise)/Pnoise), referenced from the tone-bin
 * bandwidth (6.25/freq_osr Hz) to the 2500 Hz WSJT-X reference.
 */
static float estimateSNR(const monitor_t *mon, const ftx_candidate_t *cand, double noisePow) {
    const ftx_waterfall_t *wf = &mon->wf;
    const int sync_offsets[FT8_NUM_SYNC] = {0, FT8_SYNC_OFFSET, 2 * FT8_SYNC_OFFSET};

    double sigSum = 0.0;
    int nSig = 0;

    for (int g = 0; g < FT8_NUM_SYNC; ++g) {
        for (int s = 0; s < FT8_LENGTH_SYNC; ++s) {
            int sym = sync_offsets[g] + s;
            int block = cand->time_offset + sym;
            if (block < 0 || block >= wf->num_blocks)
                continue;

            uint8_t costasTone = kFT8_Costas_pattern[s];
            int bin = cand->freq_offset + costasTone * wf->freq_osr;
            if (bin < 0 || bin >= wf->num_bins)
                continue;

            int base = block * wf->block_stride +
                       cand->time_sub * (wf->freq_osr * wf->num_bins) +
                       cand->freq_sub * wf->num_bins;

            float db = (float)wf->mag[base + bin] * 0.5f - 120.0f;
            sigSum += pow(10.0, db / 10.0);
            nSig++;
        }
    }

    if (nSig == 0 || noisePow < 1e-15)
        return -24.0f;

    double sigPlusNoise = sigSum / nSig;
    double signal = sigPlusNoise - noisePow;
    if (signal < 1e-12)
        signal = 1e-12;

    double snr_bin = 10.0 * log10(signal / noisePow);

    /* Reference from the tone-bin bandwidth to 2500 Hz (WSJT-X convention). */
    double binBW = (double)K_FSK_DEV / (double)wf->freq_osr;
    double snr = snr_bin - 10.0 * log10(2500.0 / binBW);

    if (snr < -24.0)
        snr = -24.0;
    if (snr > 49.0)
        snr = 49.0;
    return (float)snr;
}

void decode(const monitor_t *mon, struct tm *tm_slot_start, struct decoder_results *decodes, int32_t *n_results) {
    /* Get the slot type */
    struct timeval lTime;
    gettimeofday(&lTime, NULL);

    ft8slot_t thisSlot = ((lTime.tv_sec / FT8_PERIOD) & 0x01) ? odd : even;

    // time_t current_time = time(NULL);
    time_t current_time = lTime.tv_sec;

    const ftx_waterfall_t *wf = &mon->wf;
    // Find top candidates by Costas sync score and localize them in time and frequency
    ftx_candidate_t candidate_list[K_MAX_CANDIDATES];
    int num_candidates = ftx_find_candidates(wf, K_MAX_CANDIDATES, candidate_list, K_MIN_SCORE);

    /* Robust slot-wide noise floor for SNR estimation (computed once) */
    double noisePow = noiseFloorPower(mon);

    // wprintw(trafficW, "Found %d candidates\n", num_candidates);
    // wrefresh(trafficW);

    // Hash table for decoded messages (to check for duplicates)
    int num_decoded = 0;
    ftx_message_t decoded[K_MAX_MESSAGES];
    ftx_message_t *decoded_hashtable[K_MAX_MESSAGES];

    // Initialize hash table pointers
    for (int i = 0; i < K_MAX_MESSAGES; ++i) {
        decoded_hashtable[i] = NULL;
    }

    // Go over candidates and attempt to decode messages
    for (int idx = 0; idx < num_candidates; ++idx) {
        const ftx_candidate_t *cand = &candidate_list[idx];

        float freq_hz = (mon->min_bin + cand->freq_offset + (float)cand->freq_sub / wf->freq_osr) / mon->symbol_period;
        float time_sec = (cand->time_offset + (float)cand->time_sub / wf->time_osr) * mon->symbol_period;

#ifdef WATERFALL_USE_PHASE
        // int resynth_len = 12000 * 16;
        // float resynth_signal[resynth_len];
        // for (int pos = 0; pos < resynth_len; ++pos)
        // {
        //     resynth_signal[pos] = 0;
        // }
        // monitor_resynth(mon, cand, resynth_signal);
        // char resynth_path[80];
        // sprintf(resynth_path, "resynth_%04f_%02.1f.wav", freq_hz, time_sec);
        // save_wav(resynth_signal, resynth_len, 12000, resynth_path);
#endif

        ftx_message_t message;
        ftx_decode_status_t status;
        if (!ftx_decode_candidate(wf, cand, K_LDPC_ITERS, &message, &status)) {
            if (status.ldpc_errors > 0) {
                LOG(LOG_DEBUG, "LDPC decode: %d errors\n", status.ldpc_errors);
                // wprintw(trafficW, "LDPC decode: %d errors\n", status.ldpc_errors);
                // wrefresh(trafficW);
            } else if (status.crc_calculated != status.crc_extracted) {
                LOG(LOG_DEBUG, "CRC mismatch!\n");
                // wprintw(trafficW, "CRC mismatch!\n");
                // wrefresh(trafficW);
            }
            continue;
        }

        /* Real SNR estimate (dB, 2500 Hz reference) for this candidate,
           replacing the old (cand->score - 20) placeholder. */
        int32_t estSnr = (int32_t)lroundf(estimateSNR(mon, cand, noisePow));

        LOG(LOG_DEBUG, "Checking hash table for %4.1fs / %4.1fHz [%d]...\n", time_sec, freq_hz, cand->score);
        // wprintw(trafficW, "Checking hash table for %4.1fs / %4.1fHz [%d]...\n", time_sec, freq_hz, cand->score);
        // wrefresh(trafficW);

        int idx_hash = message.hash % K_MAX_MESSAGES;
        bool found_empty_slot = false;
        bool found_duplicate = false;
        int check_idx = idx_hash;
        do {
            if (rx_state.exit_flag)
                break; /* Abort case, final sig */

            if (decoded_hashtable[idx_hash] == NULL) {
                // LOG(LOG_DEBUG, "Decoded Found an empty slot\n");
                // wprintw(trafficW, "Found an empty slot\n");

                found_empty_slot = true;
            } else if ((decoded_hashtable[idx_hash]->hash == message.hash) && (0 == memcmp(decoded_hashtable[idx_hash]->payload, message.payload, sizeof(message.payload)))) {
                // LOG(LOG_DEBUG, "Decoded Found a duplicate!\n");
                // wprintw(trafficW, "Found a duplicate!\n");

                found_duplicate = true;
            } else {
                // LOG(LOG_DEBUG, "Decoded Hash table clash!\n");
                // wprintw(trafficW, "Hash table clash!\n");

                // Move on to check the next entry in hash table
                idx_hash = (idx_hash + 1) % K_MAX_MESSAGES;

                if (check_idx == idx_hash)  // We looped around, exit!
                    found_duplicate = true;
            }
            // wrefresh(trafficW);

        } while (!found_empty_slot && !found_duplicate);

        /*
                if (found_empty_slot) {
                    // Fill the empty hashtable slot
                    memcpy(&decoded[idx_hash], &message, sizeof(message));
                    decoded_hashtable[idx_hash] = &decoded[idx_hash];
                    ++num_decoded;

                    char text[FTX_MAX_MESSAGE_LENGTH];
                    ftx_message_rc_t unpack_status = ftx_message_decode(&message, &hash_if, text);
                    if (unpack_status != FTX_MESSAGE_RC_OK) {
                        snprintf(text, sizeof(text), "Error [%d] while unpacking!", (int)unpack_status);
                    }

                    // Fake WSJT-X-like output for now
                    float snr = cand->score * 0.5f;  // TODO: compute better approximation of SNR
                    printf("%02d%02d%02d %+05.1f %+4.2f %4.0f ~  %s\n",
                           tm_slot_start->tm_hour, tm_slot_start->tm_min, tm_slot_start->tm_sec,
                           snr, time_sec, freq_hz, text);
                }
        */
        /* Add this entry to an empty hashtable slot */
        if (found_empty_slot) {
            char msgToLog[FTX_MAX_MESSAGE_LENGTH];

            memcpy(&decoded[idx_hash], &message, sizeof(message));
            decoded_hashtable[idx_hash] = &decoded[idx_hash];

            char text[FTX_MAX_MESSAGE_LENGTH + 1];
            ftx_message_rc_t unpack_status = ftx_message_decode(&message, &hash_if, text);

            snprintf(msgToLog, FTX_MAX_MESSAGE_LENGTH, "%s", text);

            // wprintw(trafficW, "Message to print: %s\n", msgToPrint);
            // wrefresh(trafficW);

            char *strPtr = strtok((char *)text, " ");
            // Here if the message is mlformed strtok will return NULL, this needs to be handled

            if (NULL == strPtr) {
                text[FTX_MAX_MESSAGE_LENGTH] = (char)'\0';  // This is to be fixed
                int msgLen = strlen((char *)text);
                if (msgLen > 0) {
                    LOG(LOG_DEBUG, "Decoded : message syntax wrong : [%s]\n", text);
                } else {
                    LOG(LOG_DEBUG, "Decoded : message empty!\n");
                }

                // Skip this message
                continue;
            }

            // We have a real message now, strPtr points to the first non-space char

            if (!strncmp(strPtr, "CQ", 2)) {  // Only get the CQ messages

                struct plain_message qsoMsg;

                strPtr = strtok(NULL, " ");  // Move on the XY or Callsign part

                /* Guard against a malformed "CQ" with no following token */
                if (strPtr == NULL) {
                    LOG(LOG_DEBUG, "Decoded : bare CQ with no callsign, skipping\n");
                    continue;
                }

                pthread_mutex_lock(&msglock);  // Protect decodes structure

                sprintf(decodes[num_decoded].cmd, "CQ   ");
                // If what follows CQ is 2 chars then we keep it in CQ command
                if (2 == strlen(strPtr)) {
                    sprintf(decodes[num_decoded].cmd, "CQ %s", strPtr);
                    strPtr = strtok(NULL, " ");  // Move on the Callsign part
                }
                /*
                if (!strncmp(strPtr, "DX", 2)) {
                    sprintf(decodes[num_decoded].cmd, "CQ DX");
                    strPtr = strtok(NULL, " ");  // Move on the Callsign part
                }
                */

                /* strPtr (callsign) may be NULL here if the message was e.g.
                   "CQ DX" with nothing after; treat as empty rather than deref.
                   Strip <> from hash-resolved calls (e.g. <A6TEST>). */
                char callBuf[13];
                snprintf(decodes[num_decoded].call, sizeof(decodes[num_decoded].call),
                         "%.12s", stripBrackets(strPtr, callBuf, sizeof(callBuf)));
                strPtr = strtok(NULL, " ");  // Move on the Locator part
                snprintf(decodes[num_decoded].loc, sizeof(decodes[num_decoded].loc), "%.6s", strPtr ? strPtr : "");

                decodes[num_decoded].freq = (int32_t)freq_hz + 1500;
                decodes[num_decoded].snr = estSnr;  // real SNR estimate (dB, 2500 Hz ref)
                decodes[num_decoded].tempus = current_time;

                pthread_mutex_unlock(&msglock);

                /* Feed the QSO Handler machine */
                snprintf(qsoMsg.src, sizeof(qsoMsg.src), "%s", decodes[num_decoded].call);
                sprintf(qsoMsg.dest, "CQ");
                qsoMsg.freq = (int32_t)freq_hz + dec_options.freq + 1500;
                qsoMsg.ft8slot = thisSlot;  // This is useful only in QSO mode
                qsoMsg.snr = estSnr;        // real SNR estimate (dB)
                qsoMsg.tempus = current_time;

                tsq_push(qsoh_queue, &QSOHlock, qsoMsg);

                num_decoded++;
            } else
            // This is not a CQ, the first string is the destination
            {
                // Check if this is for us, in such case this will initiate a QSO
                if (!strncmp(strPtr, dec_options.rcall, strlen(dec_options.rcall))) {
                    struct plain_message qsoMsg;

                    char *dst = strPtr;
                    char *src = strtok(NULL, " ");
                    char *msg = strtok(NULL, " \n");
                    char srcBuf[13], dstBuf[13];
                    snprintf(qsoMsg.src, sizeof(qsoMsg.src), "%s", stripBrackets(src, srcBuf, sizeof(srcBuf)));
                    snprintf(qsoMsg.dest, sizeof(qsoMsg.dest), "%s", stripBrackets(dst, dstBuf, sizeof(dstBuf)));
                    snprintf(qsoMsg.message, sizeof(qsoMsg.message), "%s", msg ? msg : "");

                    qsoMsg.freq = (int32_t)freq_hz + dec_options.freq + 1500;
                    qsoMsg.snr = estSnr;  // real SNR estimate (dB)

                    qsoMsg.ft8slot = thisSlot;  // This is useful only in QSO mode
                    qsoMsg.tempus = current_time;

                    /* Feed the QSO Handler machine */
                    tsq_push(qsoh_queue, &QSOHlock, qsoMsg);
                }
            }
            // In any case we will log the message
            struct plain_message logMsg;

            strPtr = strtok(msgToLog, " ");

            char *dst = strPtr;
            char *src = strtok(NULL, " ");
            char *logtxt = strtok(NULL, " \n");
            char lsrcBuf[13], ldstBuf[13];
            snprintf(logMsg.src, sizeof(logMsg.src), "%s", stripBrackets(src, lsrcBuf, sizeof(lsrcBuf)));
            snprintf(logMsg.dest, sizeof(logMsg.dest), "%s", stripBrackets(dst, ldstBuf, sizeof(ldstBuf)));
            snprintf(logMsg.message, sizeof(logMsg.message), "%s", logtxt ? logtxt : "");

            logMsg.freq = (int32_t)freq_hz + dec_options.freq + 1500;
            logMsg.snr = estSnr;  // real SNR estimate (dB)
            logMsg.tempus = current_time;

            tsq_push(log_queue, &LOGlock, logMsg);

            // wprintw(trafficW, "%dHz - %02d - %s\n", (int32_t)freq_hz + dec_options.freq + 1500, (int32_t)cand->score, msgToPrint);

            // wrefresh(trafficW);
        }
    }
    // LOG(LOG_INFO, "Decoded %d messages, callsign hashtable size %d\n", num_decoded, callsign_hashtable_size);
    *n_results = num_decoded;

    /* Trigger the QSO Handler state machine */
    struct tick_message tickMsg;
    tickMsg.currentSlot = thisSlot;

    tsq_push(tick_queue, &Ticklock, tickMsg);
}

void closeRtlDevice(void) {
    if (rtl_device)
        rtlsdr_close(rtl_device);
}

bool startRtlDevice(char *resultText) {
    int rtl_count = 0;
    int rtl_result = 0;

    rtl_count = rtlsdr_get_device_count();
    if (!rtl_count) {
        sprintf(resultText, "Device crashed\n");
        return false;
    }
    rtl_result = rtlsdr_open(&rtl_device, rx_options.device);
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot open device\n");
        return false;
    }

    /* Determine the RTL-SDR generation so HF can pick the right reception path:
       - v3 (R820T2, reported as R820T) uses Q-branch direct sampling on HF (-d 2)
       - v4 (R828D) has an internal upconverter: direct sampling must stay OFF,
         we simply tune to the HF frequency and the driver handles the rest.
       Autodetected from the tuner type, overridable with --rtl3 / --rtl4. */
    rtl_gen_t gen = rx_options.rtlgen;
    if (gen == rtlAuto) {
        enum rtlsdr_tuner tuner = rtlsdr_get_tuner_type(rtl_device);
        gen = (tuner == RTLSDR_TUNER_R828D) ? rtlV4 : rtlV3;
        LOG(LOG_INFO, "RTL tuner type %d detected -> assuming RTL-SDR %s\n",
            (int)tuner, (gen == rtlV4) ? "v4" : "v3");
    } else {
        LOG(LOG_INFO, "RTL generation forced to %s by option\n",
            (gen == rtlV4) ? "v4" : "v3");
    }

    /* Choose the HF direct-sampling mode unless the user forced -d explicitly */
    if (rx_options.isHF && !rx_options.directset) {
        if (gen == rtlV4)
            rx_options.directsampling = 0;  // v4 internal upconverter: no direct sampling
        else
            rx_options.directsampling = 2;  // v3 Q-branch direct sampling
    }

    /* Report the detected device back to the caller (name + generation) */
    {
        const char *devName = rtlsdr_get_device_name(rx_options.device);
        snprintf(resultText, 96, "%s (RTL-SDR %s)",
                 (devName && devName[0]) ? devName : "RTL device",
                 (gen == rtlV4) ? "v4" : "v3");
    }

    if (rx_options.autogain)
        rtl_result = rtlsdr_set_tuner_gain_mode(rtl_device, 0);
    else
        rtl_result = rtlsdr_set_tuner_gain(rtl_device, rx_options.gain);
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot set gain\n");
        rtlsdr_close(rtl_device);
        rtl_device = NULL;
        return false;
    }
    if (rx_options.directsampling)
        rtl_result = rtlsdr_set_direct_sampling(rtl_device, rx_options.directsampling);
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot set direct sampling\n");
        rtlsdr_close(rtl_device);
        rtl_device = NULL;
        return false;
    }
    rtl_result = rtlsdr_set_sample_rate(rtl_device, SAMPLING_RATE);  // >= 0
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot set sampling rate\n");
        rtlsdr_close(rtl_device);
        rtl_device = NULL;
        return false;
    }
    if (rx_options.ppm != 0)
        rtl_result = rtlsdr_set_freq_correction(rtl_device, rx_options.ppm);
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot set frequency correction\n");
        rtlsdr_close(rtl_device);
        rtl_device = NULL;
        return false;
    }
    rtl_result = rtlsdr_set_center_freq(rtl_device, rx_options.realfreq + FS4_RATE + 1500);
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot set center frequency\n");
        rtlsdr_close(rtl_device);
        rtl_device = NULL;
        return false;
    }
    rtl_result = rtlsdr_reset_buffer(rtl_device);
    if (rtl_result < 0) {
        sprintf(resultText, "Cannot reset device buffer\n");
        rtlsdr_close(rtl_device);
        rtl_device = NULL;
        return false;
    }
    pthread_create(&rxThread, NULL, rtlsdr_rx, NULL);
    return true;
}

void usage(FILE *stream, int32_t status) {
    fprintf(stream,
            "rtlsdr_ft8d, a simple FT8 daemon for RTL receivers\n\n"
            "Use:\n\trtlsdr_ft8d -f frequency -c callsign -l locator [options]\n"
            "\t-f dial frequency [(,k,M) Hz] or band string\n"
            "\t   If band string is used, the default dial frequency will used.\n"
            "\t   Bands: 160m 80m 60m 40m 30m 20m 17m 15m 12m 10m 6m 4m 2m 1m25 70cm 23cm\n"
            "\t   (Check the band plan according your IARU Region)\n"
            "\t-c your callsign (12 chars max)\n"
            "\t-l your locator grid (6 chars max)\n"
            "Receiver extra options:\n"
            "\t-g gain [0-49] (default: 29)\n"
            "\t-a auto gain (off by default, no parameter)\n"
            "\t-o frequency offset (default: 0)\n"
            "\t-p crystal correction factor (ppm) (default: 0)\n"
            "\t-u upconverter (default: 0, example: 125M)\n"
            "\t-d direct dampling [0,1,2] (default: 0, 1 for I input, 2 for Q input)\n"
            "\t   (v3-only; on HF it is selected automatically per RTL generation)\n"
            "\t--rtl3 force RTL-SDR v3 behaviour (R820T2, Q-branch direct sampling on HF)\n"
            "\t--rtl4 force RTL-SDR v4 behaviour (R828D, internal upconverter on HF)\n"
            "\t   (by default the RTL generation is autodetected from the tuner type)\n"
            "\t-n max iterations (default: 0 = infinite loop)\n"
            "\t-i device index (in case of multiple receivers, default: 0)\n"
            "Debugging options:\n"
            "\t-x do not report any spots on web clusters (WSPRnet, PSKreporter...)\n"
            "\t-t decoder self-test (generate a signal & decode), no parameter\n"
            "\t--rx-test synthetic RX source: generate ~3 FT8 msgs/sec (A1TEST...) with no RTL device\n"
            "\t-w write received signal and exit [filename prefix]\n"
            "\t-r read signal with .iq or .c2 format, decode and exit [filename]\n"
            "\t   (raw format: 375sps, float 32 bits, 2 channels)\n"
            "Other options:\n"
            "\t--help show list of options\n"
            "\t--version show version of program\n"
            "Example:\n"
            "\trtlsdr_ft8d -f 2m -c A1XYZ -l AB12cd -g 29\n");
    exit(status);
}

int main(int argc, char **argv) {
    uint32_t opt;
    const char *short_options = "f:c:l:g:ao:p:u:d:n:i:xtw:r:";
    int32_t option_index = 0;
    struct option long_options[] = {
        {"help", no_argument, 0, 0},
        {"version", no_argument, 0, 0},
        {"rtl3", no_argument, 0, 0},
        {"rtl4", no_argument, 0, 0},
        {"rx-test", no_argument, 0, 0},
        {0, 0, 0, 0}};

    int32_t rtl_result;
    int32_t rtl_count;
    char rtl_vendor[256], rtl_product[256], rtl_serial[256];

    initrx_options();

    /* FFTW init & allocation */
    initFFTW();

    /* Stop condition setup */
    rx_state.exit_flag = false;
    uint32_t nLoop = 0;

    if (argc <= 1)
        usage(stdout, EXIT_SUCCESS);

    while ((opt = getopt_long(argc, argv, short_options, long_options, &option_index)) != -1) {
        switch (opt) {
            case 0:
                switch (option_index) {
                    case 0:  // --help
                        usage(stdout, EXIT_SUCCESS);
                        break;
                    case 1:  // --version
                        printf("rtlsdr_ft8d v%s\n", rtlsdr_ft8d_version);
                        exit(EXIT_FAILURE);
                        break;
                    case 2:  // --rtl3 : force RTL-SDR v3 (R820T2) behaviour
                        rx_options.rtlgen = rtlV3;
                        break;
                    case 3:  // --rtl4 : force RTL-SDR v4 (R828D) behaviour
                        rx_options.rtlgen = rtlV4;
                        break;
                    case 4:  // --rx-test : synthetic RX source, no real device
                        rx_options.rxtest = true;
                        /* Never report synthetic (fake A1TEST...) traffic to
                           the live PSKReporter database. Force reporting off. */
                        rx_options.noreport = true;
                        break;
                }
                break;  /* Prevent fall-through into 'f' with a NULL optarg */
            case 'f':  // Frequency
                if (!strcasecmp(optarg, "160m")) {
                    rx_options.dialfreq = 1840000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "80m")) {
                    rx_options.dialfreq = 3573000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "60m")) {
                    rx_options.dialfreq = 5357000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "40m")) {
                    rx_options.dialfreq = 7074000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "30m")) {
                    rx_options.dialfreq = 10136000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "20m")) {
                    rx_options.dialfreq = 14074000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "17m")) {
                    rx_options.dialfreq = 18100000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "15m")) {
                    rx_options.dialfreq = 21074000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "12m")) {
                    rx_options.dialfreq = 24915000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "10m")) {
                    rx_options.dialfreq = 28074000;
                    rx_options.isHF = true;
                } else if (!strcasecmp(optarg, "6m")) {
                    rx_options.dialfreq = 50313000;
                } else if (!strcasecmp(optarg, "4m")) {
                    rx_options.dialfreq = 70100000;
                } else if (!strcasecmp(optarg, "2m")) {
                    rx_options.dialfreq = 144174000;
                } else if (!strcasecmp(optarg, "1m25")) {
                    rx_options.dialfreq = 222065000;
                } else if (!strcasecmp(optarg, "70cm")) {
                    rx_options.dialfreq = 432065000;
                } else if (!strcasecmp(optarg, "23cm")) {
                    rx_options.dialfreq = 1296174000;
                } else {
                    rx_options.dialfreq = (uint32_t)atofs(optarg);
                    /* Raw frequency below 30 MHz is treated as HF */
                    if (rx_options.dialfreq && rx_options.dialfreq < 30000000)
                        rx_options.isHF = true;
                }
                break;
            case 'c':  // Callsign
                snprintf(dec_options.rcall, sizeof(dec_options.rcall), "%.12s", optarg);
                break;
            case 'l':  // Locator / Grid
                snprintf(dec_options.rloc, sizeof(dec_options.rloc), "%.6s", optarg);
                break;
            case 'g':  // Small signal amplifier gain
                rx_options.gain = atoi(optarg);
                if (rx_options.gain < 0) rx_options.gain = 0;
                if (rx_options.gain > 49) rx_options.gain = 49;
                rx_options.gain *= 10;
                break;
            case 'a':  // Auto gain
                rx_options.autogain = 1;
                break;
            case 'o':  // Fine frequency correction
                rx_options.shift = atoi(optarg);
                break;
            case 'p':  // Crystal correction
                rx_options.ppm = atoi(optarg);
                break;
            case 'u':  // Upconverter frequency
                rx_options.upconverter = (uint32_t)atofs(optarg);
                break;
            case 'd':  // Direct Sampling
                rx_options.directsampling = (uint32_t)atofs(optarg);
                rx_options.directset = true;
                break;
            case 'n':  // Stop after n iterations
                rx_options.maxloop = (uint32_t)atofs(optarg);
                break;
            case 'i':  // Select the device to use
                rx_options.device = (uint32_t)atofs(optarg);
                break;
            case 'x':  // Decoder option, single pass mode (same as original wsprd)
                rx_options.noreport = true;
                break;
            case 't':  // Seft test (used in unit-test CI pipeline)
                rx_options.selftest = true;
                break;
            case 'w':  // Write received signal to a file and exit
                rx_options.writefile = true;
                rx_options.filename = optarg;
                break;
            case 'r':  // Read a signal from a file and decode
                rx_options.readfile = true;
                rx_options.filename = optarg;
                break;
            default:
                printf("Offending option : ");
                putchar(opt);
                usage(stderr, EXIT_FAILURE);
                break;
        }
        printf("Option : ");
        putchar(opt);
    }

    if (rx_options.qso == true) {
        init_ncurses(rx_options.dialfreq);
    }

    if (rx_options.selftest == false) {
        if (rx_options.dialfreq == 0) {
            fprintf(stderr, "Please specify a dial frequency.\n");
            fprintf(stderr, " --help for usage...\n");
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }

        if (dec_options.rcall[0] == 0) {
            fprintf(stderr, "Please specify your callsign.\n");
            fprintf(stderr, " --help for usage...\n");
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }

        if (dec_options.rloc[0] == 0) {
            fprintf(stderr, "Please specify your locator.\n");
            fprintf(stderr, " --help for usage...\n");
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    }

    /* Safety: in synthetic RX-test mode, never report to the live database.
       Force reporting off and do not even construct the reporter. */
    if (rx_options.rxtest)
        rx_options.noreport = true;

    /* Always construct the reporter so that reporting can be toggled ON at
       runtime (via the "PSK ON" command) without dereferencing a NULL pointer.
       Whether spots are actually sent is gated by rx_options.noreport in the
       pskUploader thread. (Skipped entirely in rx-test mode.) */
    if (!rx_options.rxtest) {
        if (!rx_options.noreport) {
            wprintw(trafficW, "PSK Reporter Initialized!\n");
            wrefresh(trafficW);
        }
        reporter = new PskReporter(dec_options.rcall, dec_options.rloc, pskreporter_app_version);
    }

    /* Now we can mute stderr by redirecting it to a log file.
       Use freopen() rather than assigning the stderr macro (which is UB). */
    if (freopen("/tmp/ft8.log", "w+", stderr) == NULL) {
        /* Non-fatal: keep the original stderr if redirection fails */
        wprintw(trafficW, "Warning: cannot redirect stderr to /tmp/ft8.log\n");
        wrefresh(trafficW);
    }

    /* Calcule shift offset */
    rx_options.realfreq = rx_options.dialfreq + rx_options.shift + rx_options.upconverter;

    /* Store the frequency used for the decoder */
    dec_options.freq = rx_options.dialfreq;

    if (rx_options.selftest == true) {
        if (decoderSelfTest()) {
            wprintw(trafficW, "Self-test SUCCESS!\n");
            wrefresh(trafficW);
            return exit_ft8(rx_options.qso, EXIT_SUCCESS);
        } else {
            wprintw(trafficW, "Self-test FAILED!\n");
            wrefresh(trafficW);
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    }

    if (rx_options.readfile == true) {
        wprintw(trafficW, "Reading IQ file: %s\n", rx_options.filename);
        decodeRecordedFile(rx_options.filename);
        return exit_ft8(rx_options.qso, EXIT_SUCCESS);
    }

    if (rx_options.writefile == true) {
        wprintw(trafficW, "Saving IQ file planned with prefix: %.8s\n", rx_options.filename);
    }

    /* If something goes wrong... */
    signal(SIGINT, &sigint_callback_handler);
    signal(SIGTERM, &sigint_callback_handler);
    signal(SIGILL, &sigint_callback_handler);
    signal(SIGFPE, &sigint_callback_handler);
    // signal(SIGSEGV, &sigint_callback_handler);   /* SIGSEV cannot be ignored */
    signal(SIGABRT, &sigint_callback_handler);

    /* Init & parameter the device */
    char rtlDevResult[96];
    bool rtlOk;

    if (rx_options.rxtest) {
        /* Synthetic RX source: no real device is opened */
        snprintf(rtlDevResult, sizeof(rtlDevResult), "Synthetic RX-test source (no RTL device)");
        rtlOk = true;
    } else {
        rtlOk = startRtlDevice(rtlDevResult);
    }

    /* Show a centered splash for 5s: device found (or not), arch and version */
    if (rx_options.qso)
        showSplash(rtlOk, rtlDevResult, rtlsdr_ft8d_version);

    if (!rtlOk) {
        wprintw(trafficW, "%s\n", rtlDevResult);
        wrefresh(trafficW);
        sleep(1);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }


    /* Time alignment & info */
    struct timeval lTime;
    time_t rawtime;
    time(&rawtime);
    struct tm gtmv;
    struct tm *gtm = gmtime_r(&rawtime, &gtmv);

    /* Print used parameter */
    wprintw(trafficW, "\nStarting rtlsdr-ft8d (%04d-%02d-%02d, %02d:%02dz) -- Version %s\n",
            gtm->tm_year + 1900, gtm->tm_mon + 1, gtm->tm_mday, gtm->tm_hour, gtm->tm_min, rtlsdr_ft8d_version);
    wprintw(trafficW, "  Callsign     : %s\n", dec_options.rcall);
    wprintw(trafficW, "  Locator      : %s\n", dec_options.rloc);
    wprintw(trafficW, "  Dial freq.   : %d Hz\n", rx_options.dialfreq);
    wprintw(trafficW, "  Real freq.   : %d Hz\n", rx_options.realfreq);
    wprintw(trafficW, "  PPM factor   : %d\n", rx_options.ppm);
    if (rx_options.autogain)
        wprintw(trafficW, "  Auto gain    : enable\n");
    else
        wprintw(trafficW, "  Gain         : %d dB\n", rx_options.gain / 10);

    hashtable_init();
    initQsoState();

    /* Wait for timing alignment */
    gettimeofday(&lTime, NULL);
    uint32_t sec = lTime.tv_sec % FT8_PERIOD;
    uint32_t usec = sec * 1000000 + lTime.tv_usec;
    uint32_t uwait = FT8_BUFRESET - usec;
    uint32_t ft8wait;
    if (!rx_options.rxtest) {
        wprintw(trafficW, "Wait for time sync (start in %d sec)\n\n", uwait / 1000000);
        wrefresh(trafficW);
        sleep((uwait / 1000000) > 3 ? (uwait / 1000000) : 3);
    } else {
        wprintw(trafficW, "RX-test mode: generating ~3 synthetic FT8 msgs/sec\n\n");
        wrefresh(trafficW);
    }

    wclear(trafficW);
    wrefresh(trafficW);
    wattrset(trafficW, A_NORMAL);
    // printHeaders();

    /* Prepare a low priority param for the decoder thread */
    struct sched_param param;
    pthread_attr_init(&decThread.attr);
    pthread_attr_setschedpolicy(&decThread.attr, SCHED_RR);
    pthread_attr_getschedparam(&decThread.attr, &param);
    param.sched_priority = 90;  // = sched_get_priority_min();
    pthread_attr_setschedparam(&decThread.attr, &param);

    /* Create a thread and stuff for separate decoding
       Info : https://computing.llnl.gov/tutorials/pthreads/
    */
    pthread_cond_init(&decThread.ready_cond, NULL);
    pthread_mutex_init(&decThread.ready_mutex, NULL);
    if (!rx_options.rxtest)
        pthread_create(&rxThread, NULL, rtlsdr_rx, NULL);
    pthread_create(&decThread.thread, &decThread.attr, decoder, NULL);
    pthread_create(&pskThread, NULL, pskUploader, NULL);
    pthread_create(&CQHThread, NULL, CQHandler, NULL);
    pthread_create(&KBHThread, NULL, KBDHandler, NULL);
    pthread_create(&TXHThread, NULL, TXHandler, NULL);
    pthread_create(&qsoThread, NULL, QSOHandler, NULL);

    /* Main loop : Wait, read, decode */
    /* TODO */
    /*  - Decoder thread must be started at second 12.6 whilst buffer has to be reset at sec 15
        - Buffer also can be shorter, thus we may reset the bufferIndex twice
    */
    while (!rx_state.exit_flag && !(rx_options.maxloop && (rx_options.nloop >= rx_options.maxloop))) {
        if (rx_options.rxtest) {
            /* Synthetic slot: honour the real 15 s FT8 timing. Wait until the
               transmission window would end (FT8_TXTIME), fill the current
               buffer with a whole window of synthetic traffic, then flip and
               trigger the decoder exactly like the real path. */
            gettimeofday(&lTime, NULL);
            sec = lTime.tv_sec % FT8_PERIOD;
            usec = sec * 1000000 + lTime.tv_usec;
            ft8wait = FT8_TXTIME - usec;
            uwait = FT8_BUFRESET - usec;
            if (uwait > ft8wait) {
                usleep(ft8wait);
                /* Fill the buffer the decoder will read (current bufferIndex),
                   then flip so prevBuffer points back to it. */
                fillRxTestBuffer(rx_state.bufferIndex);
                rx_state.bufferIndex = (rx_state.bufferIndex + 1) % 2;
                rx_state.iqIndex[rx_state.bufferIndex] = 0;
                safe_cond_signal(&decThread.ready_cond, &decThread.ready_mutex);

                gettimeofday(&lTime, NULL);
                sec = lTime.tv_sec % FT8_PERIOD;
                usec = sec * 1000000 + lTime.tv_usec;
                uwait = FT8_BUFRESET - usec;
                usleep(uwait);
                rx_state.iqIndex[rx_state.bufferIndex] = 0;
            } else {
                usleep(uwait);
                rx_state.iqIndex[rx_state.bufferIndex] = 0;
            }
            continue;
        }

        /* Wait for time Sync on 15 secs */
        gettimeofday(&lTime, NULL);
        sec = lTime.tv_sec % FT8_PERIOD;
        usec = sec * 1000000 + lTime.tv_usec;
        ft8wait = FT8_TXTIME - usec;  // Time before decoding
        uwait = FT8_BUFRESET - usec;  // Time before buffer reset
        if (uwait > ft8wait)          // Normal case, we wait for FT8_TXTIME
        {
            usleep(ft8wait);  // Sync with peer transmission
            /* Switch to the other buffer and trigger the decoder */
            rx_state.bufferIndex = (rx_state.bufferIndex + 1) % 2;
            rx_state.iqIndex[rx_state.bufferIndex] = 0;
            safe_cond_signal(&decThread.ready_cond, &decThread.ready_mutex);

            // Sync with the FT8_PERIOD and reset the buffer index
            gettimeofday(&lTime, NULL);
            sec = lTime.tv_sec % FT8_PERIOD;
            usec = sec * 1000000 + lTime.tv_usec;
            uwait = FT8_BUFRESET - usec;  // Time before buffer reset
            usleep(uwait);
            rx_state.iqIndex[rx_state.bufferIndex] = 0;
        } else  // There's some problem, don't decode and just fix the index
        {
            usleep(uwait);
            rx_state.iqIndex[rx_state.bufferIndex] = 0;
        }

        // LOG(LOG_DEBUG, "Main thread -- Waiting %d seconds\n", uwait / 1000000);
        // usleep(uwait);
        // LOG(LOG_DEBUG, "Main thread -- Sending a GO to the decoder thread\n");

        /* Switch to the other buffer and trigger the decoder */
        // rx_state.bufferIndex = (rx_state.bufferIndex + 1) % 2;
        // rx_state.iqIndex[rx_state.bufferIndex] = 0;
        // safe_cond_signal(&decThread.ready_cond, &decThread.ready_mutex);

        usleep(100000); /* Give a chance to the other thread to update the nloop counter */

        // Test!!!
        if (callback_counter == callback_cnt_old) {
            /* Try to restart the device */
            char errorTxt[32];

            if (!startRtlDevice(errorTxt)) {
                wprintw(trafficW, "%s\n", errorTxt);
                wrefresh(trafficW);
                rx_state.exit_flag = true;
                sleep(3);
            }
        }
        callback_cnt_old = callback_counter;
    }  // While

    /* Stop the decoder thread */
    rx_state.exit_flag = true;
    safe_cond_signal(&decThread.ready_cond, &decThread.ready_mutex);

    if (rx_options.rxtest) {
        /* Synthetic source runs in the main loop; nothing to join */
    } else {
        /* Stop the RX and free the blocking function */
        rtlsdr_cancel_async(rtl_device);
        rtlsdr_close(rtl_device);
    }

    /* Free FFTW buffers */
    freeFFTW();

    /* Wait the thread join (send a signal before to terminate the job) */
    pthread_join(decThread.thread, NULL);
    if (!rx_options.rxtest)
        pthread_join(rxThread, NULL);

    /* Destroy QSO handler */
    close_qso_handler();
    pthread_join(qsoThread, NULL);

    /* Destroy PSK uploader */
    closePskThread();
    pthread_join(pskThread, NULL);

    /* Destroy TXThread */
    close_TxThread();
    pthread_join(TXHThread, NULL);

    /* Destroy Keyboard Handling thread */
    close_KbhThread();
    pthread_join(KBHThread, NULL);

    /* Destroy Keyboard Handling thread */
    close_CQThread();
    pthread_join(CQHThread, NULL);

    /* Destroy the lock/cond/thread */
    pthread_cond_destroy(&decThread.ready_cond);
    pthread_mutex_destroy(&decThread.ready_mutex);

    wprintw(trafficW, "Bye!\n");

    return exit_ft8(rx_options.qso, EXIT_SUCCESS);
}

/*
 * FT8 Protocol doc high level
 * - https://physics.princeton.edu/pulsar/k1jt/FT4_FT8_QEX.pdf
 * - http://laarc.weebly.com/uploads/7/3/2/9/73292865/ft8syncv8.pdf
 * - http://www.sportscliche.com/wb2fko/WB2FKO_TAPR_revised.pdf
 */
void ft8_subsystem(float *iSamples,
                   float *qSamples,
                   uint32_t samples_len,
                   struct decoder_results *decodes,
                   int32_t *n_results) {
    // UPDATE: adjust with samples_len !!

    // Compute FFT over the whole signal and store it
    uint8_t mag_power[MAG_ARRAY];

    int offset = 0;
    float max_mag = -120.0f;

    for (int idx_block = 0; idx_block < NUM_BLOCKS; ++idx_block) {
        // Loop over two possible time offsets (0 and BLOCK_SIZE/2)
        for (int time_sub = 0; time_sub < K_TIME_OSR; ++time_sub) {
            float mag_db[NFFT];

            // UPDATE : try FFT over 2 symbols, stepped by half symbols
            for (int i = 0; i < NFFT; ++i) {
                fft_in[i][0] = iSamples[(idx_block * BLOCK_SIZE) + (time_sub * SUB_BLOCK_SIZE) + i] * hann[i];
                fft_in[i][1] = qSamples[(idx_block * BLOCK_SIZE) + (time_sub * SUB_BLOCK_SIZE) + i] * hann[i];
            }
            fftwf_execute(fft_plan);

            // Compute log magnitude in decibels
            for (int i = 0; i < NFFT; ++i) {
                float mag2 = fft_out[i][0] * fft_out[i][0] + fft_out[i][1] * fft_out[i][1];
                mag_db[i] = 10.0f * log10f(1E-12f + mag2 * 4.0f / (NFFT * NFFT));
            }

            // Loop over two possible frequency bin offsets (for averaging)
            for (int freq_sub = 0; freq_sub < K_FREQ_OSR; ++freq_sub) {
                for (int pos = 0; pos < NUM_BIN; ++pos) {
                    float db = mag_db[pos * K_FREQ_OSR + freq_sub];
                    // Scale decibels to unsigned 8-bit range and clamp the value
                    // Range 0-240 covers -120..0 dB in 0.5 dB steps
                    int scaled = (int)(2 * db + 240);

                    mag_power[offset] = (scaled < 0) ? 0 : ((scaled > 255) ? 255 : scaled);
                    ++offset;

                    if (db > max_mag)
                        max_mag = db;
                }
            }
        }
    }

    /* Find top candidates by Costas sync score and localize them in time and frequency
    ftx_candidate_t candidate_list[K_MAX_CANDIDATES];

    ftx_waterfall_t power = {
    .num_blocks = NUM_BLOCKS,
    .num_bins = NUM_BIN,
    .time_osr = K_TIME_OSR,
    .freq_osr = K_FREQ_OSR,
    .mag = mag_power,
    .block_stride = (K_TIME_OSR * K_FREQ_OSR * NUM_BIN),
    .protocol = FTX_PROTOCOL_FT8};



int num_candidates = ftx_find_candidates(&power, K_MAX_CANDIDATES, candidate_list, K_MIN_SCORE);

        wprintw(trafficW, "Looking or candidates\n");

wprintw(trafficW, "Found %d candidates\n", num_candidates);
wrefresh(trafficW);
*/

    monitor_t mon;
    monitor_config_t mon_cfg = {
        .f_min = 200,
        .f_max = 3000,
        .sample_rate = SIGNAL_SAMPLE_RATE,
        .time_osr = K_TIME_OSR,
        .freq_osr = K_FREQ_OSR,
        .protocol = FTX_PROTOCOL_FT8};

    float symbol_period = (mon_cfg.protocol == FTX_PROTOCOL_FT4) ? FT4_SYMBOL_PERIOD : FT8_SYMBOL_PERIOD;

    mon.min_bin = (int)(mon_cfg.f_min * symbol_period);
    mon.max_bin = (int)(mon_cfg.f_max * symbol_period) + 1;
    const int num_bins = mon.max_bin - mon.min_bin;

    // waterfall_init(&me->wf, max_blocks, num_bins, mon_cfg.time_osr, mon_cfg.freq_osr);
    mon.wf.protocol = mon_cfg.protocol;

    mon.symbol_period = symbol_period;

    mon.max_mag = -120.0f;

    mon.wf.num_blocks = NUM_BLOCKS;
    mon.wf.num_bins = NUM_BIN;
    mon.wf.time_osr = K_TIME_OSR;
    mon.wf.freq_osr = K_FREQ_OSR;
    mon.wf.mag = mag_power;
    mon.wf.block_stride = (K_TIME_OSR * K_FREQ_OSR * NUM_BIN);
    mon.wf.protocol = FTX_PROTOCOL_FT8;

    decode(&mon, NULL, decodes, n_results);
}

void enableReporting(void) {
    rx_options.noreport = false;
}

void disableReporting(void) {
    rx_options.noreport = true;
}

bool getReportingStatus(void) {
    return (rx_options.noreport == false);
}

void programQuit(void) {
    rx_state.exit_flag = true;
}
