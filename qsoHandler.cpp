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

#define MAXQSOPEERS 512  // size of the Peer's database

#define MAXQSOLIFETIME 12  // in quarter of a minute

typedef enum qsostate_t { idle,
                          replyLoc,
                          replySig,
                          replyRR73,
                          reply73,
                          cqIng };
char qsoLogFileName[] = "QSOLOG.txt";

/* Variables */
static qsostate_t qsoState = idle;
static struct plain_message theQso;
static uint32_t ft8time = 0;
static uint32_t ft8tick = 0;
static bool txBusy;
static uint32_t peers[MAXQSOPEERS];
static uint32_t peersIdx = 0;

void initQsoState(void) {
    qsoState = idle;
    ft8time = 0;
    ft8tick = 0;
    theQso.src[0] = 0;   // This is the Peer for the QSO
    theQso.dest[0] = 0;  // This is the local callId
    theQso.message[0] = 0;
    theQso.freq = 0;
    theQso.snr = 0;
    theQso.tempus = 0;
    theQso.ft8slot = odd;
    txBusy = false;
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

uint32_t hashCallId(char *theCall) {
    uint32_t theValue = 0;

    uint32_t theLength = strlen(theCall);
    if(theLength > 6) theLength = 6;

    for (uint32_t i = 0; i < theLength; i++) {
        theValue *= 36;
        if (theCall[i] >= 'A')
        {
            theValue += theCall[i] - 'A' + 10;
        }
        else
        {
            theValue += theCall[i] - '0';
        }
    }
    return theValue;
}

bool checkPeer(char *thePeer) {
    uint32_t theHash = hashCallId(thePeer);

    for (uint32_t i = 0; i < peersIdx; i++)
        if (peers[peersIdx] == theHash)
            return true;

    return false;
}

bool addPeer(char *newPeer) {
    if (checkPeer(newPeer))
        return false;

    peers[peersIdx] = hashCallId(newPeer);
    peersIdx++;
    if (peersIdx > MAXQSOPEERS)
        peersIdx = 0;
    return true;
}

void queueTx(char *the String) {
    FT8Msg Txletter;

    sprintf(Txletter.ft8Message, "%s", theString);

    pthread_mutex_lock(&TXlock);  // Protect key queue structure
    tx_queue.push_back(Txletter);
    pthread_mutex_unlock(&TXlock);  // Protect key queue structure
}

bool handleTx(ft8slot_t theSlot) {
    if ((qsoState == idle) || (qsoState == cqIng))
        return false;
    else {
        if (txBusy == false) {
            /* If we are in the same slot than the QSO, we will queue the reply */
            if (theQso.ft8slot == the Slot) {
                char theMessage[255];
                char theLevel[255];
                switch (qsoState) {
                    case replyLoc:
                        // Reply FT8Tx FREQ DEST SRC LOC
                        sprintf(theMessage, "FT8Tx %d %s %s %s", theQso.freq, theQso.dest, theQso.src, dec_options.rloc);
                        break;
                    case replySig:
                        // Reply FT8Tx FREQ DEST SRC LEVEL
                        if (theQso.snr >= 0)
                            sprintf(theLevel, "+%02d", theQso.snr);
                        else
                            sprintf(theLevel, "-%02d", theQso.snr);
                        sprintf(theMessage, "FT8Tx %d %s %s %s", theQso.freq, theQso.dest, theQso.src, theLevel);
                        break;
                    case replyRR73:
                        sprintf(theMessage, "FT8Tx %d %s %s RR73", theQso.freq, theQso.dest, theQso.src);
                        // Reply DEST SRC RR73
                        break;
                    case reply73:
                        // Reply DEST SRC 73
                        sprintf(theMessage, "FT8Tx %d %s %s 73", theQso.freq, theQso.dest, theQso.src);
                        qsoState = idle;
                        break;
                }
                LOG(LOG_DEBUG, "%sn", theMessage);

#ifndef DEBUG
                queueTx(theMessage);
#endif
            }
        }  // Txbusy = false
        return true;
    }
}

bool queryCQ() {
    char theMessage[255];

    sprintf(theMessage, "FT8Tx %d CQ %s %s", rx_options.dialfreq + 1500, dec_options.rcall, dec_options.rloc);
    LOG(LOG_DEBUG, "%sn", theMessage);

#ifndef DEBUG
    queueTx(theMessage);
#endif
}

/*
This function is to be called at every slot
after scanning of ft8 messages.
queryCq() must be called AFTER this function
In case the Tx is idle, it returns true,
otherwise it returns false.
*/
bool updateQsoMachine(ft8slot_t theSlot) {
    ft8tick++;

    /* Complete the Tx session */
    txBusy = handleTx(theSlot);

    if (qsoState == cqIng)
        qsoState = idle;

    if (qsoState == idle) return true;

    /* Check if QSO in timeout, if so close it and return true */
    if (ft8tick >= ft8time) {
        /* Check if the QSO can be considered closed, if so log the record */
        if ((qsoState != replyLoc) && (qsoState != replySig)) {
            /* This QSO shall be logged */
            /* TBD */
        }
        resetQsoState();
        return true;
    }
    return false;
}

/* State Machine Description */
/*
INPUT - Prev State - Next State   Action
------+------------+------------+---------
CQ    |  !Idle     | No change  | Reject
------+------------+------------+---------
CQ    |  Idle      | replyLoc   | Accept
------+------------+------------+---------
LOC   |  replyLoc  | replySig   | Accept
------+------------+------------+---------
LOC   |  Idle      | replySig   | Accept
------+------------+------------+---------
LOC   |  replySig  | replySig   | Accept
------+------------+------------+---------
SIG   |  Idle      | replySig   | Accept
------+------------+------------+---------
SIG   |  replyLoc  | replySig   | Accept
------+------------+------------+---------
SIG   |  replySig  | replyRR73  | Accept
------+------------+------------+---------
SIG   |  replyRR73 | replyRR73  | Accept
------+------------+------------+---------
RR73  |  Idle      | reply73    | Close
------+------------+------------+---------
RR73  |  replyLoc  | reply73    | Close
------+------------+------------+---------
RR73  |  replySig  | reply73    | Close
------+------------+------------+---------
RR73  |  replyRR73 | reply73    | Close
------+------------+------------+---------
73    |  Idle      | Idle       | Close
------+------------+------------+---------
73    |  replyLoc  | Idle       | Close
------+------------+------------+---------
73    |  replySig  | Idle       | Close
------+------------+------------+---------
73    |  replyRR73 | Idle       | Close
------+------------+------------+---------
Tmo   |  Any       | Idle       | Close
------+------------+------------+---------

*/

bool addQso(struct plain_message *newQso) {
    /* If we had already a QSO with that peer we reject the QSO */
    if (checkPeer(newQso->src))
        return false;

    ft8time = ft8tick + MAXQSOLIFETIME;
    sprintf(theQso.src, "%s", newQso->src);    // This is the Peer for the QSO
    sprintf(theQso.dest, "%s", newQso->dest);  // This is the local callId
    sprintf(theQso.message, "%s", newQso->message);
    theQso.freq = newQso->freq;
    theQso.snr = newQso->snr;
    /// theQso.tempus = newQso->tempus;
    theQso.ft8slot = newQso->ft8slot;

    if (strlen(theQso.message) > 3)  // This is a Locator
        qsoState = replySig;
    else
        qsoState = replyLoc;  // This is a CQ

    txBusy = handleTx(theQso.ft8slot);
}

bool updateQso(struct plain_message *newQso) {
    /* It's valid only if we had already a QSO with that peer */
    if (checkPeer(newQso->src) == false)
        return false;

    // Check for all the cases
    if ((strstr(theQso.message, "+")) || (strstr(theQso.message, "-"))) {
        if (qsoState ==)
            qsoState = replyRR73;
    }
    if (strlen(theQso.message) > 3)  // This is a Locator
        qsoState = replySig;
    else
        qsoState = replyLoc;  // This is a CQ
}

/* Thi function is called when autometic CQ answer is enabled */
bool addCQ(struct plain_message *newQso) {
    /* If we had already a QSO with that peer we reject the QSO */
    if (checkPeer(newQso->src))
        return false;

    ft8time = ft8tick + MAXQSOLIFETIME;
    sprintf(theQso.src, "%s", newQso->src);  // This is the Peer for the QSO
    sprintf(theQso.dest, "\n");
    sprintf(theQso.message, "CQ");
    theQso.freq = newQso->freq;
    theQso.snr = newQso->snr;
    theQso.tempus = newQso->tempus;
    theQso.ft8slot = newQso->ft8slot;

    qsoState = replyLoc;  // This is a CQ
}
