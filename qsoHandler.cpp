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

#include <rtlsdr_ft8d.h>
#include <ft8tx/FT8Types.h>
#include <qsoHandler.h>

extern const char *rtlsdr_ft8d_version;
extern char pskreporter_app_version[];
extern std::vector<struct decoder_results> cq_queue;
extern std::vector<struct plain_message> qso_queue;
extern std::vector<struct plain_message> log_queue;
extern pthread_mutex_t CQlock;
extern pthread_mutex_t QSOlock;
extern pthread_mutex_t LOGlock;
extern struct decoder_options dec_options;
extern struct receiver_options rx_options;

extern pthread_mutex_t TXlock;
extern std::vector<FT8Msg> tx_queue;

/* When testing define the symbol TESTQSO */
#define TESTQSO

#define MAXQSOPEERS 512  // size of the Peer's database

#define MAXQSOLIFETIME 4  // in quarter of a minute
#define QUERYCQDELAY 3    // in quarter of a minute

/* Variables */
static qsostate_t qsoState = idle;
static struct plain_message currentQSO;
static uint32_t ft8time = 0;
static uint32_t ft8tick = 0;
static bool txBusy;
static uint32_t peers[MAXQSOPEERS];
static uint32_t peersIdx = 0;

void initQsoState(void) {
    qsoState = idle;
    ft8time = 0;
    ft8tick = 0;
    currentQSO.src[0] = 0;   // This is the Peer for the QSO
    currentQSO.dest[0] = 0;  // This is the local callId
    currentQSO.message[0] = 0;
    currentQSO.freq = 0;
    currentQSO.snr = 0;
    currentQSO.tempus = 0;
    currentQSO.ft8slot = odd;
    txBusy = false;
}

static void resetQsoState(void) {
    qsoState = idle;
    currentQSO.src[0] = 0;
    currentQSO.dest[0] = 0;
    currentQSO.message[0] = 0;
    currentQSO.freq = 0;
    currentQSO.snr = 0;
    currentQSO.tempus = 0;
    currentQSO.ft8slot = odd;
    ft8time = ft8tick;
}

/* Methods for QSO logging */

void logQSO(struct plain_message *completedQSO) {
    char qsoLogFileName[] = "QSOLOG.txt";
    FILE *qsoLogFile;
    char timeBuff[20];

    qsoLogFile = fopen(qsoLogFileName, "a");

    strftime(timeBuff, 20, "%Y-%m-%d %H:%M:%S", localtime(&completedQSO->tempus));

    fprintf(qsoLogFile, "%s %d %02d %s %s \n", timeBuff, completedQSO->freq, completedQSO->snr, completedQSO->src, completedQSO->dest);

    fclose(qsoLogFile);
}

uint32_t hashCallId2(const char *callId) {
    uint32_t hash = 0;

    for (uint32_t i = 6; *callId && i; i--, callId++) {
        hash *= 36;
        hash += *callId - '0';
        if (*callId >= 'A') hash += '0' + 10 - 'A';
    }
    return hash;
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

void queueTx(char *txString) {
    FT8Msg Txletter;

    sprintf(Txletter.ft8Message, "%s", txString);

    pthread_mutex_lock(&TXlock);  // Protect key queue structure
    tx_queue.push_back(Txletter);
    pthread_mutex_unlock(&TXlock);  // Protect key queue structure
}

bool handleTx(ft8slot_t txSlot) {
    if ((qsoState == idle) || (qsoState == cqIng))
        return false;
    else {
        if (txBusy == false) {
            /* If we are in the same slot than the QSO, we will queue the reply */
            if (currentQSO.ft8slot == txSlot) {
                char theMessage[255];
                char theLevel[255];
                switch (qsoState) {
                    case replyLoc:
                        // Reply FT8Tx FREQ DEST SRC LOC
                        sprintf(theMessage, "FT8Tx %d %s %s %s", currentQSO.freq, currentQSO.src, dec_options.rcall, dec_options.rloc);
                        break;
                    case replySig:
                        // Reply FT8Tx FREQ DEST SRC LEVEL
                        if (currentQSO.snr >= 0)
                            sprintf(theLevel, "+%02d", currentQSO.snr);
                        else
                            sprintf(theLevel, "%03d", currentQSO.snr);
                        sprintf(theMessage, "FT8Tx %d %s %s %s", currentQSO.freq, currentQSO.src, dec_options.rcall, theLevel);
                        break;
                    case replyRR73:
                        sprintf(theMessage, "FT8Tx %d %s %s RR73", currentQSO.freq, currentQSO.src, dec_options.rcall);
                        // Reply DEST SRC RR73
                        break;
                    case reply73:
                        // Reply DEST SRC 73
                        sprintf(theMessage, "FT8Tx %d %s %s 73", currentQSO.freq, currentQSO.src, dec_options.rcall);
                        qsoState = idle;
                        break;
                    default:
                        qsoState = idle;  // It should NEVER happen
                        break;
                }
                LOG(LOG_DEBUG, "handleTx Transmitting %s\n", theMessage);

                queueTx(theMessage);
                displayTxString(theMessage);
            }
        }  // Txbusy = false
        return false;
    }
}

bool queryCQ(void) {
    char cqMessage[255];
    static uint32_t queryRepeat = 0;

    if (ft8tick >= queryRepeat) {
        sprintf(cqMessage, "FT8Tx %d CQ %s %s", rx_options.dialfreq + 1500, dec_options.rcall, dec_options.rloc);
        LOG(LOG_DEBUG, "queryCq Transmitting %s\n", cqMessage);

        queueTx(cqMessage);
        displayTxString(cqMessage);
        queryRepeat = ft8tick + QUERYCQDELAY;
        return true;
    }
    return false;
}

/* Test routines */
#ifdef TESTQSO
bool addQso(struct plain_message *newQso);

static struct plain_message testQSO;

void initTestQSO(void) {
    testQSO.src[0] = 0;   // This is the Peer for the QSO
    testQSO.dest[0] = 0;  // This is the local callId
    testQSO.loc[0] = 0;   // This is the Remote Loc
    testQSO.message[0] = 0;
    testQSO.freq = 14076001;
    testQSO.snr = 0;
    testQSO.tempus = 0;
    testQSO.ft8slot = odd;
}

uint32_t testCase = 0;

const char *remotes[] = {"AA0ABC", "AB1ABC", "BF9CDE", "FR5BAC"};
const char *rloc[] = {"AB44", "JF12BC", "ZQ14", "ST02"};
int rpower[] = {-4, 2, -7, -23};

void testCaseExec(ft8slot_t theSlot) {
    bool testResult;

    if (testCase == 0)
        initTestQSO();

    if (testCase > 3)
        return;

    if (testQSO.ft8slot == theSlot) {
        sprintf(testQSO.src, "%s", remotes[testCase]);
        testQSO.snr = rpower[testCase];

        if (qsoState == idle) {
            sprintf(testQSO.message, "%s", rloc[testCase]);
        }
        if (qsoState == replyLoc) {
            sprintf(testQSO.message, "%02d", rpower[testCase]);
        }
        if (qsoState == replySig) {
            sprintf(testQSO.message, "%s", "RR73");
        }
        if (qsoState == reply73) {
            sprintf(testQSO.message, "%s", "73");
            testCase++;
        }

        testResult = addQso(&testQSO);
        if (testResult == true)
            LOG(LOG_DEBUG, "testCaseExec case %d\n", testCase);
    }
}

#endif

/*
This function is to be called at every slot
after scanning of ft8 messages.
queryCq() must be called AFTER this function
In case the Tx is idle, it returns true,
otherwise it returns false.
*/
bool updateQsoMachine(ft8slot_t theSlot) {
    ft8tick++;

#ifdef TESTQSO
    testCaseExec(theSlot);
#endif

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
CQ    |  !Idle     | No change  | Ignore
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
RR73  |  Idle      | reply73    | Ignore
------+------------+------------+---------
RR73  |  replyLoc  | reply73    | Close
------+------------+------------+---------
RR73  |  replySig  | reply73    | Close
------+------------+------------+---------
RR73  |  replyRR73 | reply73    | Close
------+------------+------------+---------
73    |  Idle      | Idle       | Ignore
------+------------+------------+---------
73    |  replyLoc  | Idle       | Ignore
------+------------+------------+---------
73    |  replySig  | Idle       | Ignore
------+------------+------------+---------
73    |  replyRR73 | Idle       | Ignore
------+------------+------------+---------
Tmo   |  Any       | Idle       | Ignore
------+------------+------------+---------

*/
peermsg_t parseMsg(char *msg) {
    char *ptr = msg;

    if (isdigit(*ptr)) {
        if (atoi(msg) == 73)
            return s73Msg;
        else
            return sigMsg;
    }

    /* Let's check if this is a number */
    if ((*ptr == '+') || (*ptr == '-')) {
        return sigMsg;
    }

    if (strlen(msg) == 4) {
        if ((msg[0] == 'R') && (msg[1] == 'R'))
            return RR73Msg;
        else
            return locMsg;
    }

    return locMsg;
}

bool addQso(struct plain_message *newQso) {
    /* We accept new QSO if we are Idle */
    if (qsoState == idle) {
        /* If we had already a QSO with that peer we reject the QSO */
        if (checkPeer(newQso->src))
            return false;

        ft8time = ft8tick + MAXQSOLIFETIME;
        sprintf(currentQSO.src, "%s", newQso->src);    // This is the Peer for the QSO
        sprintf(currentQSO.dest, "%s", newQso->dest);  // This is the local callId
        sprintf(currentQSO.message, "%s", newQso->message);
        currentQSO.freq = newQso->freq;
        currentQSO.snr = newQso->snr;
        /// currentQSO.tempus = newQso->tempus;
        currentQSO.ft8slot = newQso->ft8slot;

        /* We can only add a QSO when in Idle state, CQ are handled in addCQ() */

        switch (parseMsg(currentQSO.message)) {
            case locMsg:  // He sent LOC here, we reply with SIG
            case sigMsg:  // He sent SIG here, we laso reply with our SIG
                qsoState = replySig;
                break;
            case RR73Msg:  // He sent RR73 when we are Idle. We ignore this
                break;
            case s73Msg:  // He sent 73 when we are IDLE. We ignore this
                break;
            default:  // This should NEVER happen
                break;
        }
        if (qsoState != idle)
            return true;
    } else {
        /* This may be a spurious request or an update to the current QSO */

        if (strcmp(newQso->src, currentQSO.src) != 0)
            return false;

        /* Here we are in the case of updating the QSO */
        switch (parseMsg(currentQSO.message)) {
            case locMsg:  // We are not in Idle, when receiving LOC we reply SIG
                qsoState = replySig;
                break;
            case sigMsg:
                if (qsoState == replyLoc)  // We have sent LOC, we reply SIG
                    qsoState = replySig;
                else  // Otherwise we reply RR73
                    qsoState = replyRR73;
                break;
            case RR73Msg:  // If we receive RR73 we reply 73 and close the QSO
                qsoState = reply73;
                /* Log the QSO */
                break;
            case s73Msg:  // When receiving 73 we go to Idle and clean the data
                resetQsoState(void);
                // qsoState = idle;
                break;
            default:
                resetQsoState(void); // This should NEVER happen
                // qsoState = idle;  
                break;
        }
        if (qsoState != idle)
            return true;
    }
    return false;
}

/* Thi function is called when automatic CQ answer is enabled */
bool addCQ(struct plain_message *newQso) {
    if (qsoState != idle)
        return false;

    /* If we had already a QSO with that peer we reject the QSO */
    if (checkPeer(newQso->src))
        return false;

    ft8time = ft8tick + MAXQSOLIFETIME;
    sprintf(currentQSO.src, "%s", newQso->src);  // This is the Peer for the QSO
    sprintf(currentQSO.dest, "");
    sprintf(currentQSO.message, "CQ");
    currentQSO.freq = newQso->freq;
    currentQSO.snr = newQso->snr;
    currentQSO.tempus = newQso->tempus;
    currentQSO.ft8slot = newQso->ft8slot;

    LOG(LOG_DEBUG, "addCQ From %s\n", currentQSO.src);

    qsoState = replyLoc;  // This is a CQ

    return true;
}
