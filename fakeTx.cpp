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

/* Pending-transmission hand-off to the receiver's per-slot IQ generator. */
static struct {
    char msg[MAXMSGSIZE];
    float audioHz;
    bool valid;
} fakePending;
static pthread_mutex_t fakePendingLock = PTHREAD_MUTEX_INITIALIZER;

/* Deposit a transmission for the RX to render next slot (Phase 2). */
static void fakeTxDeposit(const char *msg, float audioHz) {
    pthread_mutex_lock(&fakePendingLock);
    snprintf(fakePending.msg, sizeof(fakePending.msg), "%s", msg);
    fakePending.audioHz = audioHz;
    fakePending.valid = true;
    pthread_mutex_unlock(&fakePendingLock);
}

bool fakeTxPopPending(char *msg, int msgCap, float *audioHz) {
    bool had = false;
    pthread_mutex_lock(&fakePendingLock);
    if (fakePending.valid) {
        snprintf(msg, msgCap, "%s", fakePending.msg);
        *audioHz = fakePending.audioHz;
        fakePending.valid = false;
        had = true;
    }
    pthread_mutex_unlock(&fakePendingLock);
    return had;
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

                /* Phase 2: hand the transmission to the receiver's per-slot IQ
                   generator. The RX renders at an audio offset from its dial:
                   audio = absFreq - dial. Only deposit if we have a message and
                   the offset lands in a sane audio range. */
                if (message[0] && fakeDialHz > 0) {
                    float audio = (float)(absFreq - (long)fakeDialHz);
                    if (audio > 0.0f && audio < (float)SIGNAL_SAMPLE_RATE / 2.0f)
                        fakeTxDeposit(message, audio);
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
