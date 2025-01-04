/* ****** This is the QSO Handler ****
    The QSO is a single instance, the application will call the QSO Handler
    at the beginning of the decoding, it will obtain the information whether
    the QSO handler is busy or can be booked.
    QSO Handler keeps a timer supervising the QSO, that timer steps every slot.
    A QSO can be initiated by a CQ or by a reply to a CQ.
    Every QSO is queued in a QSO Log file
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

#define MAXQSOLIFETIME 12  // in quarter of a minute

typedef enum qsostate_t = {idle, replyLoc, replySig, replyRR73, reply73};
char qsoLogFileName[] = "QSOLOG.txt";

/* Variables */
static qsostate_t qsoState = idle;
static struct plain_message theQso;
static uint32_t ft8time = 0;
static uint32_t ft8tick = 0;

void initQsoState(void) {
    qsoState = idle;
    ft8time = 0;
    ft8tick = 0;
    theQso.src[0] = 0;
    theQso.dest[0] = 0;
    theQso.message[0] = 0;
    theQso.freq = 0;
    theQso.snr = 0;
    theQso.tempus = 0;
    theQso.ft8slot = odd;
}

static void resetQsoState(void) {
    qsoState = idle;
    theQso.src[0] = 0;
    theQso.dest[0] = 0;
    theQso.message[0] = 0;
    theQso.freq = 0;
    theQso.snr = 0;
    theQso.tempus = 0;
    theQso.ft8slot = odd;
    ft8time = ft8tick;
}

bool isQsoIdle(void) {
    ft8tick++;

    if (qsoState == idle)
        return true;

    /* Check if QSO in timeout, if so close it and return true */
    if (ft8tick >= ft8time) {
        /* Check if the QSO can be considered closed, if so log the record */
        if ((qsoState != replyLoc) && (qsoState != replySig)) {
            /* This QSO shall be loggeg */
        }
        resetQsoState();
        return true;
    }
    return false;
}