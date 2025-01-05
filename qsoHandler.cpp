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

typedef enum qsostate_t = {idle, replyLoc, replySig, replyRR73, reply73, cqIng};
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
    uint8_t charValue;
    uint32_t theValue = 0;

    uint8_t theLength = strlen(theCall) > 5 ? 5 : strlen(theCall);

    for (uint8_t i + 0; i < theLength; i++) {
        if (newPeer[i] >= 'A') {
            charValue = (uint8_t)newPeer[i] - 'A';
            theValue += charValue;
            if (i < (theLength - 1))
                theValue = theValue << 5;
        } else {
            charValue = (uint8_t)newPeer[i] - '0';
            theValue += charValue;
            if (i < (theLength - 1))
                theValue = theValue << 4;
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
                        queueTx(theMessage);
                        break;
                    case replySig:
                        // Reply FT8Tx FREQ DEST SRC LEVEL
                        if (theQso.snr >= 0)
                            sprintf(theLevel, "+%02d", theQso.snr);
                        else
                            sprintf(theLevel, "-%02d", theQso.snr);
                        sprintf(theMessage, "FT8Tx %d %s %s %s", theQso.freq, theQso.dest, theQso.src, theLevel);
                        queueTx(theMessage);
                        break;
                    case replyRR73:
                        sprintf(theMessage, "FT8Tx %d %s %s RR73", theQso.freq, theQso.dest, theQso.src);
                        queueTx(theMessage);
                        // Reply DEST SRC RR73
                        break;
                    case reply73:
                        // Reply DEST SRC 73
                        sprintf(theMessage, "FT8Tx %d %s %s 73", theQso.freq, theQso.dest, theQso.src);
                        queueTx(theMessage);
                        qsoState = idle;
                        break;
                }
            }
        }  // Txbusy = false
        return true;
    }
}

bool queryCQ() {
    char theMessage[255];

    sprintf(theMessage, "FT8Tx %d CQ %s %s", rx_options.dialfreq + 1500, dec_options.rcall, dec_options.rloc);
    queueTx(theMessage);
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
    theQso.tempus = newQso->tempus;
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
