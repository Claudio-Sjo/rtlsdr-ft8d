/*
 * fakeTx.cpp -- hardware-free stand-in for the ft8 transmitter (testing only).
 *
 * See fakeTx.h and testability.md. Phase 1: socket-compatible fake transmitter.
 *
 * Design (multithreaded, to mimic the real target as closely as possible):
 *   - fakeTxStart() spawns a LISTENER thread that binds/listens on SOCKNAME
 *     (the same UNIX socket the real ft8 daemon uses) and select()/accept()s
 *     connections -- exactly like ft8.cpp's daemon loop.
 *   - For each accepted connection the listener spawns a HANDLER thread that
 *     reads one FT8Msg, dispatches on its type, applies the Option 3 frequency
 *     rule, replies (SEND_ACK then FREQ <absHz>), logs, and closes the socket.
 *     One handler per connection keeps the listener responsive and mirrors a
 *     real concurrent server.
 *
 * Phase 1 does NOT synthesize IQ or wait for the FT8 slot; it only exercises
 * the socket handshake, the ft8-style request parser, the Option 3 logic, the
 * frequency report-back, and the receiver's TXHandler reply parsing. The chosen
 * transmission is logged to a file for inspection (and, in Phase 2, will be
 * handed to the receiver's per-slot IQ generator).
 *
 * GPLv3, part of rtlsdr-ft8d.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <wordexp.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/stat.h>

#include <ft8tx/FT8Types.h>
#include <rtlsdr_ft8d.h>
#include "fakeTx.h"

/* ------------------------------------------------------------------------- */
/* Option 3 frequency rule -- kept in sync with ft8.cpp (kBandBase / isBandBase
 * / TX_AUDIO_MIN / TX_AUDIO_MAX). The fake must choose exactly as the real ft8
 * would: if the requested frequency equals a band base, pick a random audio
 * slot; otherwise transmit the requested frequency verbatim. */
#define FAKETX_AUDIO_MIN 300  /* Hz */
#define FAKETX_AUDIO_MAX 2800 /* Hz */

static const long kFakeBandBase[] = {
    1840000, 3573000, 5357000, 7074000, 10136000, 14074000,
    18100000, 21074000, 24915000, 28074000, 50313000, 70100000, 144174000};

static bool fakeIsBandBase(long freq) {
    for (unsigned i = 0; i < sizeof(kFakeBandBase) / sizeof(kFakeBandBase[0]); i++)
        if (freq == kFakeBandBase[i])
            return true;
    return false;
}

/* Log file for accepted transmissions (relative to CWD). */
#define FAKETX_LOG "faketx.log"

/* ------------------------------------------------------------------------- */
/* Listener state */

static pthread_t fakeTxThread;
static volatile bool fakeTxRunning = false;
static volatile bool fakeTxStopFlag = false;
static int fakeServerFd = -1;
static unsigned int fakeDialHz = 0; /* RX dial, for absFreq -> audio offset */

/* Count of live handler threads, so fakeTxStop() can wait for them to drain. */
static int fakeHandlerCount = 0;
static pthread_mutex_t fakeHandlerLock = PTHREAD_MUTEX_INITIALIZER;

/* Pending-transmission hand-off to the receiver's per-slot IQ generator.
   Two slots: the receiver's own transmission (echo) and a synthetic peer reply
   (QSO mode). Each is one-shot per slot. */
struct pendingSig {
    char msg[MAXMSGSIZE];
    float audioHz;
    bool valid;
};
static struct pendingSig fakeOwn;
static struct pendingSig fakePeer;
static pthread_mutex_t fakePendingLock = PTHREAD_MUTEX_INITIALIZER;

static void depositSig(struct pendingSig *slot, const char *msg, float audioHz) {
    pthread_mutex_lock(&fakePendingLock);
    snprintf(slot->msg, sizeof(slot->msg), "%s", msg);
    slot->audioHz = audioHz;
    slot->valid = true;
    pthread_mutex_unlock(&fakePendingLock);
}

static bool popSig(struct pendingSig *slot, char *msg, int msgCap, float *audioHz) {
    bool had = false;
    pthread_mutex_lock(&fakePendingLock);
    if (slot->valid) {
        snprintf(msg, msgCap, "%s", slot->msg);
        *audioHz = slot->audioHz;
        slot->valid = false;
        had = true;
    }
    pthread_mutex_unlock(&fakePendingLock);
    return had;
}

bool fakeTxPopOwn(char *msg, int msgCap, float *audioHz) {
    return popSig(&fakeOwn, msg, msgCap, audioHz);
}

bool fakeTxPopPeer(char *msg, int msgCap, float *audioHz) {
    return popSig(&fakePeer, msg, msgCap, audioHz);
}

/* ------------------------------------------------------------------------- */
/* Fake peer station (Phase 3): a fixed-identity responder that answers the
 * receiver's QSO so the RX state machine can be driven end to end. Standard
 * callsign/grid so the messages encode without a hash-table entry. */
#define FAKE_PEER_CALL "F1ABC"
#define FAKE_PEER_GRID "JN99"
#define FAKE_PEER_RREPORT "R-10"
#define FAKE_PEER_AUDIO_DELTA 250.0f /* peer sits this far from the RX's own slot */

/* Edge-case measurement mode (responder hardening, Step 1). When the
 * environment variable FAKETX_EDGE is set, the responder replies to the
 * receiver's CQ with a cycling series of NON-standard WSJT-X message formats
 * instead of the normal grid answer, so we can observe how the RX parser /
 * QSO state machine reacts to each. Addressed to the RX callsign captured from
 * its CQ. Behind the --fake-tx test path; production is unaffected. */
static bool fakeEdgeMode = false;
static int fakeEdgeIdx = 0;
static bool fakeFreeTextMode = false; /* FAKETX_FREETEXT: peer sends free text */
/* The peer holds one audio frequency per QSO (0 = not yet chosen). Reset when a
   fresh CQ is seen so each QSO gets its own stable frequency. */
static float fakePeerAudio = 0.0f;

/* Build the next edge-case peer message addressed to rxcall. Returns false when
 * the series is exhausted (so the caller can fall back to normal behaviour). */
static bool fakeEdgeMessage(const char *rxcall, char *out, int outCap) {
    /* Each entry is a message the RX might hear on the air but that the current
     * parser does not handle as a clean std-QSO token. %s = rxcall. */
    static const char *forms[] = {
        "%s F1ABC RRR",          /* older roger: parser only knows RR73        */
        "%s F1ABC/P JN99",       /* peer with /P suffix (nonstd call)          */
        "%s <F1ABC> RR73",       /* hashed/bracketed peer call                 */
        "TNX 73 GL",             /* free text (no dest/src structure)          */
        "%s F1ABC R 579 MA",     /* ARRL-RTTY-style contest exchange           */
        "%s F1ABC 559 0013",     /* generic serial/report contest form         */
    };
    const int n = (int)(sizeof(forms) / sizeof(forms[0]));
    if (fakeEdgeIdx >= n)
        return false;
    snprintf(out, outCap, forms[fakeEdgeIdx], rxcall);
    fakeEdgeIdx++;
    return true;
}

/* Given the FT8 message the receiver just transmitted (tok0 tok1 tok2...),
 * produce the peer's next reply addressed to the RX, or return false if no
 * reply is warranted. rxAudio is the RX's own audio slot; the peer reply is
 * placed a little away from it (kept inside the passband).
 *
 * Sequencing (RX is the CQ caller):
 *   RX "CQ <rxcall> <grid>"      -> peer "<rxcall> F1ABC JN99"   (grid/answer)
 *   RX "F1ABC <rxcall> <report>" -> peer "<rxcall> F1ABC R-10"   (signal)
 *   RX "F1ABC <rxcall> RR73"     -> peer "<rxcall> F1ABC 73"     (finish)
 *   RX "F1ABC <rxcall> 73"       -> (QSO complete, no reply)
 */
static bool fakePeerReply(char *tok0, char *tok1, char *tok2,
                          char *out, int outCap) {
    if (!tok0)
        return false;

    if (!strcmp(tok0, "CQ") && tok1) {
        /* A fresh CQ starts a new QSO: forget the previous peer frequency so a
           new one is chosen for this QSO. */
        fakePeerAudio = 0.0f;
        /* tok1 = rxcall (tok2 = rxgrid, ignored). In edge-case measurement mode
           reply with the next non-standard form; otherwise answer with grid. */
        if (fakeEdgeMode && fakeEdgeMessage(tok1, out, outCap))
            return true;
        snprintf(out, outCap, "%s %s %s", tok1, FAKE_PEER_CALL, FAKE_PEER_GRID);
        return true;
    }

    /* Directed message: tok0 = dest, tok1 = src(=rxcall), tok2 = message. Only
       reply if it is addressed to us (the peer). */
    if (!strcmp(tok0, FAKE_PEER_CALL) && tok1 && tok2) {
        const char *rxcall = tok1;
        /* Edge-case measurement: keep feeding non-standard forms so the series
           advances on every RX transmission, even mid-exchange. */
        if (fakeEdgeMode && fakeEdgeMessage(rxcall, out, outCap))
            return true;
        /* Free-text test hook: once the QSO is under way, reply with plain free
           text (no dest/src) on the peer's frequency, to exercise the receiver's
           frequency+slot attribution of free text to the QSO. */
        if (fakeFreeTextMode) {
            /* Free text on the peer's frequency, to exercise the receiver's
               frequency+slot attribution of free text to the QSO. */
            snprintf(out, outCap, "TNX 73 GL");
            return true;
        }
        if (!strcmp(tok2, "RR73")) {
            snprintf(out, outCap, "%s %s 73", rxcall, FAKE_PEER_CALL);
            return true;
        }
        if (!strcmp(tok2, "73")) {
            return false; /* QSO complete */
        }
        /* A signal report or grid from the RX -> reply with R-report. */
        snprintf(out, outCap, "%s %s %s", rxcall, FAKE_PEER_CALL, FAKE_PEER_RREPORT);
        return true;
    }

    return false;
}

static void fakeLog(const char *fmt, ...) {
    /* Timestamped append to the log file; best-effort. */
    FILE *fp = fopen(FAKETX_LOG, "a");
    if (!fp)
        return;
    time_t now = time(NULL);
    struct tm tmv;
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tmv));
    fprintf(fp, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fclose(fp);
}

/* ------------------------------------------------------------------------- */
/* Per-connection handler thread.
 *
 * Reads one FT8Msg, dispatches on type. For SEND_F8_REQ it parses the request
 * string like ft8 (first free token = frequency or band, rest = message),
 * applies Option 3, replies SEND_ACK then FREQ <absHz>, and logs. TEST_SEND and
 * SEND_WSPR are ACKed; anything else is REJECTED -- matching ft8's switch. */

struct handlerArg {
    int fd;
};

static void *fakeHandler(void *argp) {
    struct handlerArg *ha = (struct handlerArg *)argp;
    int fd = ha->fd;
    free(ha);

    FT8Msg Rxletter, Txletter;
    memset(&Txletter, 0, sizeof(Txletter));

    ssize_t valread = read(fd, &Rxletter, sizeof(Rxletter));
    if (valread == (ssize_t)sizeof(Rxletter)) {
        switch (Rxletter.type) {
            case SEND_F8_REQ: {
                /* Parse "FT8Tx <freq|band> <dest> <src> <extra...>" the way ft8
                   does: split into argv, first free token is the frequency. */
                wordexp_t params;
                long absFreq = 0;
                char message[MAXMSGSIZE] = {0};
                /* Copies of the message tokens (index 2,3,4) for the QSO
                   responder, captured before wordfree. */
                char tk0[16] = {0}, tk1[16] = {0}, tk2[16] = {0};

                if (wordexp(Rxletter.ft8Message, &params, 0) == 0) {
                    /* Token 0 is "FT8Tx"; token 1 is the frequency/band; the
                       rest form the FT8 message. Mirror ft8's band table. */
                    long parsed = 0;
                    if (params.we_wordc >= 2) {
                        const char *f = params.we_wordv[1];
                        if (!strcasecmp(f, "160m")) parsed = 1840000;
                        else if (!strcasecmp(f, "80m")) parsed = 3573000;
                        else if (!strcasecmp(f, "60m")) parsed = 5357000;
                        else if (!strcasecmp(f, "40m")) parsed = 7074000;
                        else if (!strcasecmp(f, "30m")) parsed = 10136000;
                        else if (!strcasecmp(f, "20m")) parsed = 14074000;
                        else if (!strcasecmp(f, "17m")) parsed = 18100000;
                        else if (!strcasecmp(f, "15m")) parsed = 21074000;
                        else if (!strcasecmp(f, "12m")) parsed = 24915000;
                        else if (!strcasecmp(f, "10m")) parsed = 28074000;
                        else if (!strcasecmp(f, "6m")) parsed = 50313000;
                        else if (!strcasecmp(f, "4m")) parsed = 70100000;
                        else if (!strcasecmp(f, "2m")) parsed = 144174000;
                        else parsed = strtol(f, NULL, 10);
                    }

                    /* Reassemble the message tokens (index 2..end). */
                    for (size_t i = 2; i < params.we_wordc; i++) {
                        if (message[0])
                            strncat(message, " ", sizeof(message) - strlen(message) - 1);
                        strncat(message, params.we_wordv[i], sizeof(message) - strlen(message) - 1);
                    }

                    /* Capture the first three message tokens for the QSO
                       responder before freeing (tok0 tok1 tok2). */
                    if (params.we_wordc > 2) snprintf(tk0, sizeof(tk0), "%s", params.we_wordv[2]);
                    if (params.we_wordc > 3) snprintf(tk1, sizeof(tk1), "%s", params.we_wordv[3]);
                    if (params.we_wordc > 4) snprintf(tk2, sizeof(tk2), "%s", params.we_wordv[4]);

                    /* Option 3: band base -> choose an audio slot; otherwise use
                       the requested frequency verbatim. */
                    if (fakeIsBandBase(parsed)) {
                        long audio = FAKETX_AUDIO_MIN +
                                     (long)((rand() / ((double)RAND_MAX + 1.0)) *
                                            (FAKETX_AUDIO_MAX - FAKETX_AUDIO_MIN));
                        absFreq = parsed + audio;
                    } else {
                        absFreq = parsed;
                    }
                    wordfree(&params);
                }

                /* Reply 1: SEND_ACK (same as real ft8, sent before "transmit"). */
                Txletter.type = SEND_ACK;
                snprintf(Txletter.ft8Message, MAXMSGSIZE, "SEND_F8_REQ");
                send(fd, &Txletter, sizeof(Txletter), 0);

                /* Reply 2: report the actual (possibly self-chosen) frequency,
                   exactly as ft8's transmit loop now does (Option 3). */
                Txletter.type = CHANGE_RTX_STATE;
                Txletter.RTXstate = true;
                snprintf(Txletter.ft8Message, MAXMSGSIZE, "FREQ %ld", absFreq);
                send(fd, &Txletter, sizeof(Txletter), 0);

                /* Phase 2/3: hand the transmission to the receiver's per-slot IQ
                   generator. The RX renders at an audio offset from its dial:
                   audio = absFreq - dial. Deposit the RX's own echo, and -- in
                   QSO mode -- a synthetic peer reply addressed back to the RX so
                   its state machine advances. */
                if (message[0] && fakeDialHz > 0) {
                    float ownAudio = (float)(absFreq - (long)fakeDialHz);
                    if (ownAudio > 0.0f && ownAudio < (float)SIGNAL_SAMPLE_RATE / 2.0f) {
                        depositSig(&fakeOwn, message, ownAudio);

                        /* Compute the peer reply from the RX's message tokens. */
                        char peerMsg[MAXMSGSIZE];
                        if (fakePeerReply(tk0[0] ? tk0 : NULL,
                                          tk1[0] ? tk1 : NULL,
                                          tk2[0] ? tk2 : NULL,
                                          peerMsg, sizeof(peerMsg))) {
                            /* The peer keeps ONE frequency for the whole QSO
                               (like a real station). It is chosen once, a little
                               away from where the RX first called, and then
                               reused -- so free text / reports from the peer all
                               land on the same frequency the RX records as the
                               QSO frequency. */
                            float nyq = (float)SIGNAL_SAMPLE_RATE / 2.0f;
                            if (fakePeerAudio <= 0.0f) {
                                float a = ownAudio + FAKE_PEER_AUDIO_DELTA;
                                if (a >= nyq - 100.0f)
                                    a = ownAudio - FAKE_PEER_AUDIO_DELTA;
                                if (a < 100.0f)
                                    a = 100.0f;
                                fakePeerAudio = a;
                            }
                            depositSig(&fakePeer, peerMsg, fakePeerAudio);
                            fakeLog("  peer reply: \"%s\" @ %.0f Hz\n", peerMsg, fakePeerAudio);
                        }
                    }
                }

                fakeLog("SEND_F8_REQ freq=%ld msg=\"%s\"\n", absFreq, message);
                break;
            }
            case TEST_SEND:
                Txletter.type = SEND_ACK;
                snprintf(Txletter.ft8Message, MAXMSGSIZE, "TEST_SEND");
                send(fd, &Txletter, sizeof(Txletter), 0);
                fakeLog("TEST_SEND\n");
                break;
            case SEND_WSPR:
                Txletter.type = SEND_ACK;
                snprintf(Txletter.ft8Message, MAXMSGSIZE, "SEND_WSPR_REQ");
                send(fd, &Txletter, sizeof(Txletter), 0);
                fakeLog("SEND_WSPR\n");
                break;
            default:
                Txletter.type = REJECTED;
                snprintf(Txletter.ft8Message, MAXMSGSIZE, "Wrong Signal");
                send(fd, &Txletter, sizeof(Txletter), 0);
                fakeLog("REJECTED type=%d\n", (int)Rxletter.type);
                break;
        }
    }

    close(fd);

    pthread_mutex_lock(&fakeHandlerLock);
    fakeHandlerCount--;
    pthread_mutex_unlock(&fakeHandlerLock);
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Listener thread: bind/listen, then select()/accept() and spawn a detached
 * handler per connection. Mirrors ft8.cpp's daemon loop; adds a stop flag and a
 * select() timeout so shutdown is clean. */

static void *fakeListener(void *arg) {
    (void)arg;
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, SOCKNAME, sizeof(address.sun_path) - 1);

    fakeServerFd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fakeServerFd < 0) {
        perror("fakeTx: socket");
        return NULL;
    }

    unlink(SOCKNAME);
    if (bind(fakeServerFd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("fakeTx: bind");
        close(fakeServerFd);
        fakeServerFd = -1;
        return NULL;
    }
    chmod(SOCKNAME, 0777);

    if (listen(fakeServerFd, 3) < 0) {
        perror("fakeTx: listen");
        close(fakeServerFd);
        fakeServerFd = -1;
        return NULL;
    }

    fakeLog("fakeTx listener started on %s\n", SOCKNAME);

    while (!fakeTxStopFlag) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fakeServerFd, &readfds);

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000; /* 200 ms: bounded wait so we can see the stop flag */

        int st = select(fakeServerFd + 1, &readfds, NULL, NULL, &tv);
        if (st < 0) {
            if (errno == EINTR)
                continue;
            perror("fakeTx: select");
            break;
        }
        if (st == 0)
            continue; /* timeout -> re-check stop flag */

        if (FD_ISSET(fakeServerFd, &readfds)) {
            int client = accept(fakeServerFd, NULL, NULL);
            if (client < 0) {
                if (errno == EINTR)
                    continue;
                perror("fakeTx: accept");
                continue;
            }

            struct handlerArg *ha = (struct handlerArg *)malloc(sizeof(*ha));
            if (!ha) {
                close(client);
                continue;
            }
            ha->fd = client;

            pthread_mutex_lock(&fakeHandlerLock);
            fakeHandlerCount++;
            pthread_mutex_unlock(&fakeHandlerLock);

            pthread_t h;
            if (pthread_create(&h, NULL, fakeHandler, ha) != 0) {
                perror("fakeTx: pthread_create handler");
                pthread_mutex_lock(&fakeHandlerLock);
                fakeHandlerCount--;
                pthread_mutex_unlock(&fakeHandlerLock);
                close(client);
                free(ha);
                continue;
            }
            pthread_detach(h); /* fire-and-forget; drained via fakeHandlerCount */
        }
    }

    close(fakeServerFd);
    fakeServerFd = -1;
    unlink(SOCKNAME);
    fakeLog("fakeTx listener stopped\n");
    return NULL;
}

/* ------------------------------------------------------------------------- */

int fakeTxStart(unsigned int dialHz) {
    if (fakeTxRunning)
        return 0;
    fakeDialHz = dialHz;
    fakeEdgeMode = (getenv("FAKETX_EDGE") != NULL);
    fakeEdgeIdx = 0;
    fakeFreeTextMode = (getenv("FAKETX_FREETEXT") != NULL);
    fakeTxStopFlag = false;
    if (pthread_create(&fakeTxThread, NULL, fakeListener, NULL) != 0) {
        perror("fakeTx: pthread_create listener");
        return -1;
    }
    fakeTxRunning = true;
    return 0;
}

void fakeTxStop(void) {
    if (!fakeTxRunning)
        return;
    fakeTxStopFlag = true;
    pthread_join(fakeTxThread, NULL);

    /* Wait (bounded) for any in-flight handler threads to finish. */
    for (int i = 0; i < 50; i++) {
        pthread_mutex_lock(&fakeHandlerLock);
        int n = fakeHandlerCount;
        pthread_mutex_unlock(&fakeHandlerLock);
        if (n == 0)
            break;
        usleep(20000); /* 20 ms */
    }
    fakeTxRunning = false;
}
