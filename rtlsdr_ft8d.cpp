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

/* Defines for debug */
// #define TXWINTEST

/* End defines for debug */

void printSpots(uint32_t n_results);

/* Thread for decoding */
static struct decoder_thread decThread;

/* Thread for 'spotting' */
pthread_t spottingThread;

/* Thread for RX (blocking function used) & RTL struct */
static pthread_t rxThread;
static rtlsdr_dev_t *rtl_device = NULL;

/* FFTW pointers & stuff */
static fftwf_plan fft_plan;
static fftwf_complex *fft_in, *fft_out;
static FILE *fp_fftw_wisdom_file;
static float *hann;

/* Global declaration for states & options, shared with other external objects */

struct receiver_state rx_state;
struct receiver_options rx_options;
struct decoder_options dec_options;
struct decoder_results dec_results[50];
vector<struct decoder_results> dec_results_queue;
vector<struct decoder_results> cq_queue;
vector<struct plain_message> qso_queue;
vector<struct plain_message> log_queue;
int dec_results_queue_index = 0;

/* mutex for thread sync */
pthread_mutex_t msglock = PTHREAD_MUTEX_INITIALIZER;
;
pthread_mutex_t CQlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t QSOlock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t LOGlock = PTHREAD_MUTEX_INITIALIZER;

/* Could be nice to update this one with the CI */
const char *rtlsdr_ft8d_version = "0.4.1";
char pskreporter_app_version[] = "rtlsdr-ft8d_v0.4.1";

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
}

static void sigint_callback_handler(int signum) {
    fprintf(stderr, "Signal caught %d, exiting!\n", signum);
    rx_state.exit_flag = true;
    rtlsdr_cancel_async(rtl_device);
}

/* Thread used for this RX blocking function */
static void *rtlsdr_rx(void *arg) {
    rtlsdr_read_async(rtl_device, rtlsdr_callback, NULL, 0, DEFAULT_BUF_LENGTH);
    rtlsdr_cancel_async(rtl_device);
    return NULL;
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
        rx_state.gtm = gmtime(&unixtime);

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

PskReporter *reporter = NULL;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

#define MAX_REPORTS_PER_PACKET 64

void *pskUploader(void *vargp) {
    while (1) {
        if (dec_results_queue.size() > 0) {
            sleep(60);
            pthread_mutex_lock(&lock);

            while (dec_results_queue.size() > MAX_REPORTS_PER_PACKET) {
                for (int i = 0; i < MAX_REPORTS_PER_PACKET; i++) {
                    struct decoder_results dr = dec_results_queue.front();
                    reporter->addReceiveRecord(dr.call, dr.freq, dr.snr);
                    dec_results_queue.erase(dec_results_queue.begin());
                }
                reporter->send();
            }

            while (dec_results_queue.size()) {
                struct decoder_results dr = dec_results_queue.front();
                reporter->addReceiveRecord(dr.call, dr.freq, dr.snr);
                dec_results_queue.erase(dec_results_queue.begin());
            }
            reporter->send();
            pthread_mutex_unlock(&lock);

        } else {
            sleep(60);
        }
    }
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

    pthread_mutex_lock(&lock);

    for (uint32_t i = 0; i < n_results && dec_results_queue_index < 2048; i++) {
        struct decoder_results dr;

        // strncpy(dr.call, dec_results[i].call, strlen(dec_results[i].call));
        snprintf(dr.call, sizeof(dr.call), "%.12s", dec_results[i].call);
        dr.freq = dec_results[i].freq + dec_options.freq;
        dr.snr = dec_results[i].snr - 20;
        dec_results_queue.push_back(dr);
    }
    pthread_mutex_unlock(&lock);
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
    struct curl_httppost *post = NULL;
    struct curl_httppost *last = NULL;

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
        curl_formadd(&post, &last, CURLFORM_COPYNAME, "_mycall", CURLFORM_COPYCONTENTS, myCall, CURLFORM_END);
        curl_formadd(&post, &last, CURLFORM_COPYNAME, "_dxcall", CURLFORM_COPYCONTENTS, dxCall, CURLFORM_END);
        curl_formadd(&post, &last, CURLFORM_COPYNAME, "_freq", CURLFORM_COPYCONTENTS, dxFreq, CURLFORM_END);
        curl_formadd(&post, &last, CURLFORM_COPYNAME, "_info", CURLFORM_COPYCONTENTS, info, CURLFORM_END);

        curl = curl_easy_init();
        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, "http://mycluster.localhost/sends.php");
            curl_easy_setopt(curl, CURLOPT_HTTPPOST, post);
            res = curl_easy_perform(curl);

            if (res != CURLE_OK)
                fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

            curl_easy_cleanup(curl);
            curl_formfree(post);
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
        pthread_mutex_lock(&msglock);  // Protect decodes structure
        /* CQlock */
        pthread_mutex_lock(&CQlock);  // Protect decodes structure

#ifdef TXWINTEST

        wprintw(logwLL, "%02d:%02dz  %2ddB  %8dHz %5s %10s %6s\n",
                rx_state.gtm->tm_hour,
                rx_state.gtm->tm_min,
                dec_results[i].snr,
                dec_results[i].freq + dec_options.freq,
                dec_results[i].cmd,
                dec_results[i].call,
                dec_results[i].loc);

#endif

        /* Rather than printing the results, we exploit a queue */
        struct decoder_results dr;

        // strncpy(dr.call, dec_results[i].call, strlen(dec_results[i].call));
        snprintf(dr.call, sizeof(dr.call), "%.12s", dec_results[i].call);
        snprintf(dr.cmd, sizeof(dr.cmd), "%s", dec_results[i].cmd);
        dr.freq = dec_results[i].freq + dec_options.freq;
        dr.snr = dec_results[i].snr - 20;
        dr.tempus = time(NULL);
        cq_queue.push_back(dr);

        pthread_mutex_unlock(&CQlock);   // Protect decodes structure
        pthread_mutex_unlock(&msglock);  // Protect decodes structure
    }

#ifdef TXWINTEST
    wrefresh(logwLL);
#endif
}

void saveSample(float *iSamples, float *qSamples) {
    if (rx_options.writefile == true) {
        char filename[32];

        time_t rawtime;
        time(&rawtime);
        struct tm *gtm = gmtime(&rawtime);

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
        rx_state.gtm = gmtime(&unixtime);

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

    /*
    uint8_t packed[FTX_LDPC_K_BYTES];

    if (pack77(message, packed) < 0) {
        wprintw(logwL, "Cannot parse message!\n");
        return 0;
    }
    */

    // First, pack the text data into binary message
    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, message);
    if (rc != FTX_MESSAGE_RC_OK) {
        printf("Cannot parse message!\n");
        printf("RC = %d\n", (int)rc);
        return -2;
    }

    // Second, encode the binary message as a sequence of FSK tones
    uint8_t tones[FT8_NN];
    ft8_encode(msg.payload, tones);

    // Encoding, simple FSK modulation
    float f0 = 50.0;
    float t0 = 0.0;  // Caution!! Possible buffer overflow with the index calculation (no user input here!)
    float amp = 0.5;
    float wgn = 0.02;
    double phi = 0.0;
    double df = 3200.0 / 512.0;  // EVAL : #define SIGNAL_SAMPLE_RATE as int or float ??
    double dt = 1 / 3200.0;

    // Add signal
    for (int i = 0; i < FT8_NN; i++) {
        double dphi = 2.0 * M_PI * dt * (f0 + ((double)tones[i] - 3.5) * df);
        for (int j = 0; j < 512; j++) {
            int index = t0 / dt + 512 * i + j;
            iSamples[index] = amp * cos(phi) + whiteGaussianNoise(wgn);
            qSamples[index] = amp * sin(phi) + whiteGaussianNoise(wgn);
            phi += dphi;
        }
    }

    /* Save the test sample */
    writeRawIQfile(iSamples, qSamples, (char *)"selftest.iq");

    /* Search & decode the signal */
    ft8_subsystem(iSamples, qSamples, samples_len, dec_results, &n_results);

    printSpots(n_results);

    /* Simple consistency check */
    if (strcmp(dec_results[0].call, "K1JT") &&
        strcmp(dec_results[0].loc, "FN20")) {
        return 0;
    } else {
        return 1;
    }
}


#define CALLSIGN_HASHTABLE_SIZE 256

static struct
{
    char callsign[12]; ///> Up to 11 symbols of callsign + trailing zeros (always filled)
    uint32_t hash;     ///> 8 MSBs contain the age of callsign; 22 LSBs contain hash value
} callsign_hashtable[CALLSIGN_HASHTABLE_SIZE];

static int callsign_hashtable_size;

void hashtable_init(void)
{
    callsign_hashtable_size = 0;
    memset(callsign_hashtable, 0, sizeof(callsign_hashtable));
}

void hashtable_cleanup(uint8_t max_age)
{
    for (int idx_hash = 0; idx_hash < CALLSIGN_HASHTABLE_SIZE; ++idx_hash)
    {
        if (callsign_hashtable[idx_hash].callsign[0] != '\0')
        {
            uint8_t age = (uint8_t)(callsign_hashtable[idx_hash].hash >> 24);
            if (age > max_age)
            {
                LOG(LOG_INFO, "Removing [%s] from hash table, age = %d\n", callsign_hashtable[idx_hash].callsign, age);
                // free the hash entry
                callsign_hashtable[idx_hash].callsign[0] = '\0';
                callsign_hashtable[idx_hash].hash = 0;
                callsign_hashtable_size--;
            }
            else
            {
                // increase callsign age
                callsign_hashtable[idx_hash].hash = (((uint32_t)age + 1u) << 24) | (callsign_hashtable[idx_hash].hash & 0x3FFFFFu);
            }
        }
    }
}

void hashtable_add(const char* callsign, uint32_t hash)
{
    uint16_t hash10 = (hash >> 12) & 0x3FFu;
    int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    while (callsign_hashtable[idx_hash].callsign[0] != '\0')
    {
        if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) == hash) && (0 == strcmp(callsign_hashtable[idx_hash].callsign, callsign)))
        {
            // reset age
            callsign_hashtable[idx_hash].hash &= 0x3FFFFFu;
            LOG(LOG_DEBUG, "Found a duplicate [%s]\n", callsign);
            return;
        }
        else
        {
            LOG(LOG_DEBUG, "Hash table clash!\n");
            // Move on to check the next entry in hash table
            if (idx_hash + 1 > CALLSIGN_HASHTABLE_SIZE)
                {
                LOG(LOG_DEBUG, "Hash table overridden!\n");
                callsign_hashtable[0].callsign[0] = '\0';
                callsign_hashtable[0].hash = 0;
                }
            idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
        }
    }
    if ((callsign_hashtable_size + 1) < CALLSIGN_HASHTABLE_SIZE)
        callsign_hashtable_size++;
    strncpy(callsign_hashtable[idx_hash].callsign, callsign, 11);
    callsign_hashtable[idx_hash].callsign[11] = '\0';
    callsign_hashtable[idx_hash].hash = hash;
}

bool hashtable_lookup(ftx_callsign_hash_type_t hash_type, uint32_t hash, char* callsign)
{
    uint8_t hash_shift = (hash_type == FTX_CALLSIGN_HASH_10_BITS) ? 12 : (hash_type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0);
    uint16_t hash10 = (hash >> (12 - hash_shift)) & 0x3FFu;
    int idx_hash = (hash10 * 23) % CALLSIGN_HASHTABLE_SIZE;
    while (callsign_hashtable[idx_hash].callsign[0] != '\0')
    {
        if (((callsign_hashtable[idx_hash].hash & 0x3FFFFFu) >> hash_shift) == hash)
        {
            strcpy(callsign, callsign_hashtable[idx_hash].callsign);
            return true;
        }
        // Move on to check the next entry in hash table
        idx_hash = (idx_hash + 1) % CALLSIGN_HASHTABLE_SIZE;
    }
    callsign[0] = '\0';
    return false;
}


ftx_callsign_hash_interface_t hash_if = {
    .lookup_hash = hashtable_lookup,
    .save_hash = hashtable_add
};


void decode(const monitor_t *mon, struct tm *tm_slot_start, struct decoder_results *decodes, int32_t *n_results) {
    const ftx_waterfall_t *wf = &mon->wf;
    // Find top candidates by Costas sync score and localize them in time and frequency
    ftx_candidate_t candidate_list[K_MAX_CANDIDATES];
    int num_candidates = ftx_find_candidates(wf, K_MAX_CANDIDATES, candidate_list, K_MIN_SCORE);

    // wprintw(logwL, "Found %d candidates\n", num_candidates);
    // wrefresh(logwL);

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
                // wprintw(logwL, "LDPC decode: %d errors\n", status.ldpc_errors);
                // wrefresh(logwL);
            } else if (status.crc_calculated != status.crc_extracted) {
                LOG(LOG_DEBUG, "CRC mismatch!\n");
                // wprintw(logwL, "CRC mismatch!\n");
                // wrefresh(logwL);
            }
            continue;
        }

        LOG(LOG_DEBUG, "Checking hash table for %4.1fs / %4.1fHz [%d]...\n", time_sec, freq_hz, cand->score);
        // wprintw(logwL, "Checking hash table for %4.1fs / %4.1fHz [%d]...\n", time_sec, freq_hz, cand->score);
        // wrefresh(logwL);

        int idx_hash = message.hash % K_MAX_MESSAGES;
        bool found_empty_slot = false;
        bool found_duplicate = false;
        do {
            if (decoded_hashtable[idx_hash] == NULL) {
                LOG(LOG_DEBUG, "Found an empty slot\n");
                // wprintw(logwL, "Found an empty slot\n");

                found_empty_slot = true;
            } else if ((decoded_hashtable[idx_hash]->hash == message.hash) && (0 == memcmp(decoded_hashtable[idx_hash]->payload, message.payload, sizeof(message.payload)))) {
                LOG(LOG_DEBUG, "Found a duplicate!\n");
                // wprintw(logwL, "Found a duplicate!\n");

                found_duplicate = true;
            } else {
                LOG(LOG_DEBUG, "Hash table clash!\n");
                // wprintw(logwL, "Hash table clash!\n");

                // Move on to check the next entry in hash table
                idx_hash = (idx_hash + 1) % K_MAX_MESSAGES;
            }
            // wrefresh(logwL);

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
            char msgToPrint[FTX_MAX_MESSAGE_LENGTH];
            char msgToLog[FTX_MAX_MESSAGE_LENGTH];

            memcpy(&decoded[idx_hash], &message, sizeof(message));
            decoded_hashtable[idx_hash] = &decoded[idx_hash];

            char text[FTX_MAX_MESSAGE_LENGTH];
            ftx_message_rc_t unpack_status = ftx_message_decode(&message, &hash_if, text);

            snprintf(msgToPrint, FTX_MAX_MESSAGE_LENGTH, "%s", text);
            snprintf(msgToLog, FTX_MAX_MESSAGE_LENGTH, "%s", text);

            // wprintw(logwL, "Message to print: %s\n", msgToPrint);
            // wrefresh(logwL);

            char *strPtr = strtok((char *)text, " ");
            if (!strncmp(strPtr, "CQ", 2)) {  // Only get the CQ messages

                strPtr = strtok(NULL, " ");  // Move on the XY or Callsign part

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

                snprintf(decodes[num_decoded].call, sizeof(decodes[num_decoded].call), "%.12s", strPtr);
                strPtr = strtok(NULL, " ");  // Move on the Locator part
                snprintf(decodes[num_decoded].loc, sizeof(decodes[num_decoded].loc), "%.6s", strPtr);

                decodes[num_decoded].freq = (int32_t)freq_hz + 1500;
                decodes[num_decoded].snr = (int32_t)cand->score;  // UPDATE: it's not true, score != snr

                pthread_mutex_unlock(&msglock);

                num_decoded++;
            } else {
                if (!strncmp(msgToPrint, dec_options.rcall, strlen(dec_options.rcall))) {
                    struct plain_message qsoMsg;

                    char *dst = strPtr;
                    char *src = strtok(NULL, " ");
                    snprintf(qsoMsg.src, sizeof(qsoMsg.src), "%s", src);
                    snprintf(qsoMsg.dest, sizeof(qsoMsg.dest), "%s", dst);
                    snprintf(qsoMsg.message, sizeof(qsoMsg.message), "%s", strtok(NULL, " \n"));

                    qsoMsg.freq = (int32_t)freq_hz + dec_options.freq + 1500;
                    qsoMsg.snr = (int32_t)cand->score;  // UPDATE: it's not true, score != snr

                    pthread_mutex_lock(&QSOlock);  // Protect decodes structure
                    qso_queue.push_back(qsoMsg);
                    pthread_mutex_unlock(&QSOlock);  // Protect decodes structure
                }
            }
            struct plain_message logMsg;

            strPtr = strtok(msgToLog, " ");

            char *dst = strPtr;
            char *src = strtok(NULL, " ");
            snprintf(logMsg.src, sizeof(logMsg.src), "%s", src);
            snprintf(logMsg.dest, sizeof(logMsg.dest), "%s", dst);
            snprintf(logMsg.message, sizeof(logMsg.message), "%s", strtok(NULL, " \n"));

            logMsg.freq = (int32_t)freq_hz + dec_options.freq + 1500;
            logMsg.snr = (int32_t)cand->score;  // UPDATE: it's not true, score != snr
            logMsg.tempus = time(NULL);

            pthread_mutex_lock(&LOGlock);  // Protect decodes structure
            log_queue.push_back(logMsg);

            pthread_mutex_unlock(&LOGlock);  // Protect decodes structure

            // wprintw(logwL, "%dHz - %02d - %s\n", (int32_t)freq_hz + dec_options.freq + 1500, (int32_t)cand->score, msgToPrint);

            // wrefresh(logwL);
        }
    }
    // LOG(LOG_INFO, "Decoded %d messages, callsign hashtable size %d\n", num_decoded, callsign_hashtable_size);
    *n_results = num_decoded;
}

void usage(FILE *stream, int32_t status) {
    fprintf(stream,
            "rtlsdr_ft8d, a simple FT8 daemon for RTL receivers\n\n"
            "Use:\rtlsdr_ft8d -f frequency -c callsign -l locator [options]\n"
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
            "\t-n max iterations (default: 0 = infinite loop)\n"
            "\t-i device index (in case of multiple receivers, default: 0)\n"
            "Debugging options:\n"
            "\t-x do not report any spots on web clusters (WSPRnet, PSKreporter...)\n"
            "\t-t decoder self-test (generate a signal & decode), no parameter\n"
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
        {0, 0, 0, 0}};

    int32_t rtl_result;
    int32_t rtl_count;
    char rtl_vendor[256], rtl_product[256], rtl_serial[256];

    FILE *stream;
    stream = fopen("/tmp/ft8.log", "w+");

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
                }
            case 'f':  // Frequency
                if (!strcasecmp(optarg, "160m")) {
                    rx_options.dialfreq = 1840000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "80m")) {
                    rx_options.dialfreq = 3573000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "60m")) {
                    rx_options.dialfreq = 5357000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "40m")) {
                    rx_options.dialfreq = 7074000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "30m")) {
                    rx_options.dialfreq = 10136000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "20m")) {
                    rx_options.dialfreq = 14074000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "17m")) {
                    rx_options.dialfreq = 18100000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "15m")) {
                    rx_options.dialfreq = 21074000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "12m")) {
                    rx_options.dialfreq = 24915000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
                } else if (!strcasecmp(optarg, "10m")) {
                    rx_options.dialfreq = 28074000;
                    if (!rx_options.directsampling)
                        rx_options.directsampling = 2;
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
            case 'w':  // Read a signal and decode
                rx_options.writefile = true;
                rx_options.filename = optarg;
                break;
            case 'r':  // Write a signal and exit
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

    if (!rx_options.noreport) {
        wprintw(logwL, "PSK Reporter Initialized!\n");
        wrefresh(logwL);
        reporter = new PskReporter(dec_options.rcall, dec_options.rloc, pskreporter_app_version);
    }

    /* Now we can mute stderr */
    stderr = stream;

    /* Calcule shift offset */
    rx_options.realfreq = rx_options.dialfreq + rx_options.shift + rx_options.upconverter;

    /* Store the frequency used for the decoder */
    dec_options.freq = rx_options.dialfreq;

    if (rx_options.selftest == true) {
        if (decoderSelfTest()) {
            wprintw(logwL, "Self-test SUCCESS!\n");
            wrefresh(logwL);
            return exit_ft8(rx_options.qso, EXIT_SUCCESS);
        } else {
            wprintw(logwL, "Self-test FAILED!\n");
            wrefresh(logwL);
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    }

    if (rx_options.readfile == true) {
        wprintw(logwL, "Reading IQ file: %s\n", rx_options.filename);
        decodeRecordedFile(rx_options.filename);
        return exit_ft8(rx_options.qso, EXIT_SUCCESS);
    }

    if (rx_options.writefile == true) {
        wprintw(logwL, "Saving IQ file planned with prefix: %.8s\n", rx_options.filename);
    }

    /* If something goes wrong... */
    signal(SIGINT, &sigint_callback_handler);
    signal(SIGTERM, &sigint_callback_handler);
    signal(SIGILL, &sigint_callback_handler);
    signal(SIGFPE, &sigint_callback_handler);
    signal(SIGSEGV, &sigint_callback_handler);
    signal(SIGABRT, &sigint_callback_handler);

    /* Init & parameter the device */
    rtl_count = rtlsdr_get_device_count();
    if (!rtl_count) {
        wprintw(logwL, "No supported devices found\n");
        wrefresh(logwL);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }

    wprintw(logwL, "Found %d device(s):\n", rtl_count);
    for (uint32_t i = 0; i < rtl_count; i++) {
        rtlsdr_get_device_usb_strings(i, rtl_vendor, rtl_product, rtl_serial);
        wprintw(logwL, "  %d:  %s, %s, SN: %s\n", i, rtl_vendor, rtl_product, rtl_serial);
    }

    wprintw(logwL, "\nUsing device %d: %s\n", rx_options.device, rtlsdr_get_device_name(rx_options.device));
    wrefresh(logwL);

    rtl_result = rtlsdr_open(&rtl_device, rx_options.device);
    if (rtl_result < 0) {
        wprintw(logwL, "ERROR: Failed to open rtlsdr device #%d.\n", rx_options.device);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }

    if (rx_options.directsampling) {
        rtl_result = rtlsdr_set_direct_sampling(rtl_device, rx_options.directsampling);
        if (rtl_result < 0) {
            wprintw(logwL, "ERROR: Failed to set direct sampling\n");
            rtlsdr_close(rtl_device);
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    }

    rtl_result = rtlsdr_set_sample_rate(rtl_device, SAMPLING_RATE);
    if (rtl_result < 0) {
        wprintw(logwL, "ERROR: Failed to set sample rate\n");
        rtlsdr_close(rtl_device);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }

    rtl_result = rtlsdr_set_tuner_gain_mode(rtl_device, 1);
    if (rtl_result < 0) {
        wprintw(logwL, "ERROR: Failed to enable manual gain\n");
        rtlsdr_close(rtl_device);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }

    if (rx_options.autogain) {
        rtl_result = rtlsdr_set_tuner_gain_mode(rtl_device, 0);
        if (rtl_result != 0) {
            wprintw(logwL, "ERROR: Failed to set tuner gain\n");
            rtlsdr_close(rtl_device);
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    } else {
        rtl_result = rtlsdr_set_tuner_gain(rtl_device, rx_options.gain);
        if (rtl_result != 0) {
            wprintw(logwL, "ERROR: Failed to set tuner gain\n");
            rtlsdr_close(rtl_device);
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    }

    if (rx_options.ppm != 0) {
        rtl_result = rtlsdr_set_freq_correction(rtl_device, rx_options.ppm);
        if (rtl_result < 0) {
            wprintw(logwL, "ERROR: Failed to set ppm error\n");
            rtlsdr_close(rtl_device);
            return exit_ft8(rx_options.qso, EXIT_FAILURE);
        }
    }

    rtl_result = rtlsdr_set_center_freq(rtl_device, rx_options.realfreq + FS4_RATE + 1500);
    if (rtl_result < 0) {
        wprintw(logwL, "ERROR: Failed to set frequency\n");
        rtlsdr_close(rtl_device);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }

    rtl_result = rtlsdr_reset_buffer(rtl_device);
    if (rtl_result < 0) {
        wprintw(logwL, "ERROR: Failed to reset buffers.\n");
        rtlsdr_close(rtl_device);
        return exit_ft8(rx_options.qso, EXIT_FAILURE);
    }

    /* Time alignment & info */
    struct timeval lTime;
    time_t rawtime;
    time(&rawtime);
    struct tm *gtm = gmtime(&rawtime);

    /* Print used parameter */
    wprintw(logwL, "\nStarting rtlsdr-ft8d (%04d-%02d-%02d, %02d:%02dz) -- Version %s\n",
            gtm->tm_year + 1900, gtm->tm_mon + 1, gtm->tm_mday, gtm->tm_hour, gtm->tm_min, rtlsdr_ft8d_version);
    wprintw(logwL, "  Callsign     : %s\n", dec_options.rcall);
    wprintw(logwL, "  Locator      : %s\n", dec_options.rloc);
    wprintw(logwL, "  Dial freq.   : %d Hz\n", rx_options.dialfreq);
    wprintw(logwL, "  Real freq.   : %d Hz\n", rx_options.realfreq);
    wprintw(logwL, "  PPM factor   : %d\n", rx_options.ppm);
    if (rx_options.autogain)
        wprintw(logwL, "  Auto gain    : enable\n");
    else
        wprintw(logwL, "  Gain         : %d dB\n", rx_options.gain / 10);


    hashtable_init();

    /* Wait for timing alignment */
    gettimeofday(&lTime, NULL);
    uint32_t sec = lTime.tv_sec % 15;
    uint32_t usec = sec * 1000000 + lTime.tv_usec;
    uint32_t uwait = 15000000 - usec;
    wprintw(logwL, "Wait for time sync (start in %d sec)\n\n", uwait / 1000000);
    wrefresh(logwL);

    sleep((uwait / 1000000) > 3 ? (uwait / 1000000) : 3);

    wclear(logwL);
    wrefresh(logwL);
    wattrset(logwL, A_NORMAL);
    printHeaders();

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
    pthread_create(&rxThread, NULL, rtlsdr_rx, NULL);
    pthread_create(&decThread.thread, &decThread.attr, decoder, NULL);
    pthread_create(&spottingThread, NULL, pskUploader, NULL);
    pthread_create(&spottingThread, NULL, CQHandler, NULL);
    pthread_create(&spottingThread, NULL, KBDHandler, NULL);
    pthread_create(&spottingThread, NULL, TXHandler, NULL);

    /* Main loop : Wait, read, decode */
    while (!rx_state.exit_flag && !(rx_options.maxloop && (rx_options.nloop >= rx_options.maxloop))) {
        /* Wait for time Sync on 15 secs */
        gettimeofday(&lTime, NULL);
        sec = lTime.tv_sec % 15;
        usec = sec * 1000000 + lTime.tv_usec;
        uwait = 15000000 - usec;
        LOG(LOG_DEBUG, "Main thread -- Waiting %d seconds\n", uwait / 1000000);
        usleep(uwait);
        LOG(LOG_DEBUG, "Main thread -- Sending a GO to the decoder thread\n");

        /* Switch to the other buffer and trigger the decoder */
        rx_state.bufferIndex = (rx_state.bufferIndex + 1) % 2;
        rx_state.iqIndex[rx_state.bufferIndex] = 0;
        safe_cond_signal(&decThread.ready_cond, &decThread.ready_mutex);
        usleep(100000); /* Give a chance to the other thread to update the nloop counter */
    }

    /* Stop the decoder thread */
    rx_state.exit_flag = true;
    safe_cond_signal(&decThread.ready_cond, &decThread.ready_mutex);

    /* Stop the RX and free the blocking function */
    rtlsdr_cancel_async(rtl_device);
    rtlsdr_close(rtl_device);

    /* Free FFTW buffers */
    freeFFTW();

    /* Wait the thread join (send a signal before to terminate the job) */
    pthread_join(decThread.thread, NULL);
    pthread_join(rxThread, NULL);

    /* Destroy the lock/cond/thread */
    pthread_cond_destroy(&decThread.ready_cond);
    pthread_mutex_destroy(&decThread.ready_mutex);

    wprintw(logwL, "Bye!\n");

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

        wprintw(logwL, "Looking or candidates\n");

wprintw(logwL, "Found %d candidates\n", num_candidates);
wrefresh(logwL);
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
