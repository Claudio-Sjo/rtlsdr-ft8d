#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdarg.h>
#include <ncurses.h>
#include <curses.h>
#include <pthread.h>
#include <assert.h>
#include <vector>
#include <ctype.h>
#include <sys/time.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <rtlsdr_ft8d.h>
#include <ft8tx/FT8Types.h>
#include <qsoHandler.h>
#include <tsqueue.h>

extern const char *rtlsdr_ft8d_version;
extern char pskreporter_app_version[];
extern std::vector<struct decoder_results> cq_queue;
extern std::vector<struct plain_message> qso_queue;
extern std::vector<struct plain_message> log_queue;
extern pthread_mutex_t CQlock;
extern pthread_mutex_t QSOlock;
extern pthread_mutex_t LOGlock;
extern struct decoder_options dec_options;

pthread_mutex_t KBDlock;
std::vector<char> kbd_queue;

pthread_mutex_t TXlock;
std::vector<FT8Msg> tx_queue;

WINDOW *header;

WINDOW *statusW, *statusW0;

WINDOW *trafficW0, *cqW0, *trafficW, *cqW, *trafficWH;

WINDOW *qso, *qso0;

WINDOW *call, *call0;

#define MAXTXSTRING 40
#define MAXCQ 25

#define CQWIN 0
#define QSOWIN 1
#define TXWIN 2

int activeWin = CQWIN;

char txString[MAXTXSTRING];
char editString[MAXTXSTRING];

/* Thread related flags */
volatile bool exitTxThread = false;
volatile bool exitKBHThread = false;
volatile bool exitCQThread = false;

// struct decoder_results cqReq[MAXCQ];
struct plain_message qsoReq[MAXCQ];
int cqFirst, cqLast, cqIdx;
int qsoFirst, qsoLast, qsoIdx;

int trafficWLines;
int qsoWLines;
uint32_t qsoFreq;
uint32_t reportedCQ;
ft8slot_t thisSlot;

volatile bool transmitting = false;

void setTransmitting(void) {
    transmitting = true;
}

void resetTransmitting(void) {
    transmitting = false;
}

bool getTransmitting(void) {
    return (transmitting == true);
}

void refreshBoxes(void) {
    /* Draw each panel's border and title on its own independent window.
       These windows do not share cells, so this cannot corrupt other panels. */
    wattrset(trafficW0, COLOR_PAIR(3) | A_BOLD);
    box(trafficW0, 0, 0);
    mvwprintw(trafficW0, 0, 10, " FT8 Traffic ");
    wnoutrefresh(trafficW0);

    wattrset(statusW0, COLOR_PAIR(3) | A_BOLD);
    box(statusW0, 0, 0);
    mvwprintw(statusW0, 0, 10, " Transceiver Status ");
    wnoutrefresh(statusW0);

    wattrset(qso0, COLOR_PAIR(3) | A_BOLD);
    box(qso0, 0, 0);
    mvwprintw(qso0, 0, 10, " Ongoing QSO ");
    wnoutrefresh(qso0);

    wattrset(cqW0, COLOR_PAIR(3) | A_BOLD);
    box(cqW0, 0, 0);
    mvwprintw(cqW0, 0, 10, " Incoming CQ ");
    wnoutrefresh(cqW0);

    wattrset(call0, COLOR_PAIR(4) | A_BOLD);
    box(call0, 0, 0);
    wnoutrefresh(call0);
}

int init_ncurses(uint32_t initialFreq) {
    /*
        Initialize the working values from the main program
    */

    qsoFreq = initialFreq + 1500;  // Base freq + 1500Hz offset
    reportedCQ = 0;

    /*
        End initialization
    */

    initscr();

    /* Hide the cursor */
    curs_set(0);

    /* No echo when getting chars */
    noecho();

    /* We need the time of the day */
    time_t currentTime = time(NULL);
    struct tm tmv;
    struct tm tm = *localtime_r(&currentTime, &tmv);

    /* Colors first (needed before we style the windows) */
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);
    init_pair(4, COLOR_CYAN, COLOR_BLACK);
    init_pair(10, COLOR_BLACK, COLOR_YELLOW);
    init_pair(11, COLOR_YELLOW, COLOR_RED);
    init_pair(12, COLOR_BLACK, COLOR_RED);
    init_pair(13, COLOR_BLACK, COLOR_GREEN);
    init_pair(14, COLOR_BLACK, COLOR_CYAN);

    /*
     * Layout: independent top-level windows (newwin), NOT overlapping subwins
     * of stdscr. Each panel is a bordered outer window (*0) with a derwin
     * content area (inner) inset by one cell. Because the panels do not share
     * cells with each other, refreshing one panel cannot corrupt another --
     * this fixes the display bleed seen during long runs.
     *
     * Grid (stdscr border kept):
     *   row 1            : header (full width)
     *   rows 2..LINES/2  : CQ (left half) | Status (right half)
     *   rows LINES/2..-4 : QSO (left half) | Traffic (right half)
     *   bottom 3 rows    : command line (full width)
     */
    int topH = LINES / 2 - 2;         // height of the CQ/Status row
    int midY = 2 + topH;              // first row of the QSO/Traffic row
    int midH = (LINES - 4) - midY;    // height of the QSO/Traffic row
    int leftW = COLS / 2 - 1;         // width of the left column
    int rightW = COLS - 2 - leftW;    // width of the right column (fills remainder)

    header = newwin(1, COLS - 2, 1, 1);

    /* Top row: CQ (left), Status (right) */
    cqW0 = newwin(topH, leftW, 2, 1);
    statusW0 = newwin(topH, rightW, 2, 1 + leftW);

    /* Middle row: QSO (left), Traffic (right) */
    qso0 = newwin(midH, leftW, midY, 1);
    trafficW0 = newwin(midH, rightW, midY, 1 + leftW);

    /* Command line (full width) */
    call0 = newwin(3, COLS - 2, LINES - 4, 1);

    /* Inner content areas (inset by 1 on every side) */
    cqW = derwin(cqW0, topH - 2, leftW - 2, 1, 1);
    statusW = derwin(statusW0, topH - 2, rightW - 2, 1, 1);
    qso = derwin(qso0, midH - 2, leftW - 2, 1, 1);
    trafficW = derwin(trafficW0, midH - 2, rightW - 2, 1, 1);
    call = derwin(call0, 1, COLS - 4, 1, 1);

    trafficWLines = midH - 2;  // scrollable content lines
    qsoWLines = midH - 2;

    /* Border/title attributes on the outer windows */
    wattrset(trafficW0, COLOR_PAIR(3) | A_BOLD);
    wattrset(cqW0, COLOR_PAIR(3) | A_BOLD);
    wattrset(qso0, COLOR_PAIR(3) | A_BOLD);
    wattrset(call0, COLOR_PAIR(4) | A_BOLD);
    wattrset(statusW0, COLOR_PAIR(3) | A_BOLD);

    /* stdscr outer border */
    attrset(COLOR_PAIR(1) | A_BOLD);
    box(stdscr, 0, 0);
    wnoutrefresh(stdscr);

    /* Header line */
    wattrset(header, COLOR_PAIR(2) | A_BOLD);
    mvwprintw(header, 0, 1, "%s - %s  %dHz", dec_options.rcall, dec_options.rloc, qsoFreq);
    mvwprintw(header, 0, COLS / 2 - 12, "rtlsdr FT8 %s - QSO Mode", rtlsdr_ft8d_version);

    /* Content windows: normal attr + scrolling */
    wattrset(trafficW, A_NORMAL);
    wattrset(cqW, A_NORMAL);
    wattrset(qso, A_NORMAL);
    wattrset(call, A_NORMAL);

    scrollok(trafficW, true);
    idlok(trafficW, true);
    scrollok(cqW, true);
    idlok(cqW, true);
    scrollok(qso, true);
    idlok(qso, true);

    /* Keyboard is polled non-blocking on the UI thread (CQHandler) */
    nodelay(call, true);
    keypad(call, true);

    /* Draw the panel borders + titles once and stage everything */
    refreshBoxes();
    wnoutrefresh(header);
    wnoutrefresh(cqW);
    wnoutrefresh(statusW);
    wnoutrefresh(qso);
    wnoutrefresh(trafficW);
    wnoutrefresh(call);
    doupdate();

    sprintf(txString, "");
    sprintf(editString, "");

    cqFirst = cqLast = cqIdx = 0;
    qsoFirst = qsoLast = qsoIdx = 0;

    /* End initialization */
    return (0);
}

/*
 * Show a centered splash window for ~5 seconds at startup.
 * Green border, yellow text. Reports the detected RTL-SDR device (or that
 * none was found), the build architecture (x86 or ARM) and the SW version.
 */
void showSplash(bool deviceFound, const char *deviceInfo, const char *swVersion) {
/* Build architecture string, resolved at compile time.
   The Makefile passes -Dx86 for PC builds and -DRPI1/-DRPI23/-DRPI4 for the Pi. */
#if defined(x86) || defined(__x86_64__) || defined(__i386__)
    const char *archStr = "x86 version";
#elif defined(__aarch64__) || defined(__arm__) || defined(RPI1) || defined(RPI23) || defined(RPI4)
    const char *archStr = "ARM version";
#else
    const char *archStr = "unknown-arch version";
#endif

    char line1[128];
    if (deviceFound)
        snprintf(line1, sizeof(line1), "RTL-SDR found: %s", deviceInfo ? deviceInfo : "unknown");
    else
        snprintf(line1, sizeof(line1), "No RTL-SDR device found!");

    char line2[64];
    snprintf(line2, sizeof(line2), "rtlsdr-ft8d %s  (%s)", swVersion ? swVersion : "?", archStr);

    /* Size the window to the widest line, with padding and borders */
    int innerW = (int)strlen(line1);
    if ((int)strlen(line2) > innerW)
        innerW = (int)strlen(line2);
    int winW = innerW + 6;  // 2 borders + padding
    int winH = 5;           // border + line1 + blank + line2 + border

    if (winW > COLS - 2)
        winW = COLS - 2;
    if (winW < 20)
        winW = 20;

    int startY = (LINES - winH) / 2;
    int startX = (COLS - winW) / 2;
    if (startY < 0)
        startY = 0;
    if (startX < 0)
        startX = 0;

    WINDOW *splash = newwin(winH, winW, startY, startX);
    if (splash == NULL)
        return;

    /* Green border */
    wattrset(splash, COLOR_PAIR(3) | A_BOLD);
    box(splash, 0, 0);

    /* Yellow text, centered on each line */
    wattrset(splash, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(splash, 1, (winW - (int)strlen(line1)) / 2, "%s", line1);
    mvwprintw(splash, 3, (winW - (int)strlen(line2)) / 2, "%s", line2);

    wrefresh(splash);

    /* Keep it on screen for 5 seconds */
    sleep(5);

    /* Tear down the splash and restore the underlying UI.
       touchwin() forces the covered windows to be fully redrawn; everything
       is staged with wnoutrefresh() and flushed with a single doupdate(). */
    werase(splash);
    wnoutrefresh(splash);
    delwin(splash);

    touchwin(stdscr);
    wnoutrefresh(stdscr);
    refreshBoxes();
    touchwin(header);   wnoutrefresh(header);
    touchwin(statusW);  wnoutrefresh(statusW);
    touchwin(cqW);      wnoutrefresh(cqW);
    touchwin(trafficW); wnoutrefresh(trafficW);
    touchwin(qso);      wnoutrefresh(qso);
    touchwin(call);     wnoutrefresh(call);
    doupdate();
}

int close_ncurses() {
    // getch();

    endwin();

    return (0);
}

int exit_ft8(bool qsomode, int status) {
    if (qsomode == true) {
        wrefresh(qso);
        close_ncurses();
    }
    return status;
}

// This is the interface with the tx task

/* Program constants */
#define SOCKNAME "/tmp/ft8S"
#define MAXCONNECT 5
#define FOREVER 1

#define SUCCESSFUL_RUN 0
#define FTOK_FAIL 1
#define MSGGET_FAIL 2

/* Maximum size for a string, normally it's useful :-) */
#define MAXSTRING 255

/* The first socket descriptor of the table is the listening one */
#define SD 0

/* Error values */
#define NO_ERROR 0
#define ERR_NULL_POINTER 1001
#define ERR_MSG_SEND 1002
#define ERR_MSG_RECEIVE 1003
#define ERR_MISSING_SOCK 1004
#define ERR_PARSER 1005

#define RECEIVE_CMD 2000
#define QUIT_PROGRAM 2001
#define WAIT_PROGRAM 2002
#define WAIT_KEYBD 2003

/* Valid actions */
#define MESSAGE_SEND 1
#define MESSAGE_RECV 2

#define TX_IDLE 0
#define TX_WAITING 1
#define TX_ONGOING 2
#define TX_END 3

int txStatusFlag;

void *TXHandler(void *vargp) {
    int status, valread, client_fd;
    struct sockaddr_un serv_addr;

    FT8Msg Txletter, Rxletter;

    txStatusFlag = TX_IDLE;

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, SOCKNAME, sizeof(serv_addr.sun_path) - 1);

    while (exitTxThread == false) {
        if (tsq_pop(tx_queue, &TXlock, &Txletter)) {

            // sprintf(Txletter.ft8Message, "FT8Tx 20m SA0PRF SA0PRF JO99");
            Txletter.type = SEND_F8_REQ;

            if ((client_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
                perror("\n Socket creation error \n");
            }

            if ((status = connect(client_fd, (struct sockaddr *)&serv_addr,
                                  sizeof(serv_addr))) < 0) {
                perror("\nConnection Failed \n");
            }
            send(client_fd, &Txletter, sizeof(Txletter), 0);

            valread = read(client_fd, &Rxletter, sizeof(Rxletter));
            if (!valread) {
                perror("Error, nothing read");
            }
            txStatusFlag = TX_WAITING;

            valread = read(client_fd, &Rxletter, sizeof(Rxletter));
            if (!valread) {
                perror("Error, nothing read");
            }
            txStatusFlag = TX_ONGOING;
            setTransmitting();

            valread = read(client_fd, &Rxletter, sizeof(Rxletter));
            if (!valread) {
                perror("Error, nothing read");
            }
            txStatusFlag = TX_END;
            resetTransmitting();

            sleep(1);

            txStatusFlag = TX_IDLE;

            // closing the connected socket
            close(client_fd);
        }
        usleep(10000);  // Wait 10msec
    }

    return NULL;
}

// End of the tx interface

/* Status Interface */
void refreshStatus(bool refresh) {
    if (!refresh)
        return;
    mvwprintw(statusW, 0, 3, " PSK Report : %s", getReportingStatus() ? "ON " : "OFF");
    mvwprintw(statusW, 1, 3, " Auto Reply : %s", getAutoCQReplyStatus() ? "ON " : "OFF");
    mvwprintw(statusW, 2, 3, " Self CQ    : %s", getAutoCQStatus() ? "ON " : "OFF");
    mvwprintw(statusW, 3, 3, " Auto QSO   : %s", getAutoQSOStatus() ? "ON " : "OFF");
    mvwprintw(statusW, 4, 3, " Active Slot: %s", (getActiveSlot() == odd) ? "ODD " : "EVEN");

    mvwprintw(statusW, 6, 3, " RTx        : ");
    if (getTransmitting()) {
        wattrset(statusW, COLOR_PAIR(2) | A_BOLD);  // QSO are GREEN

        mvwprintw(statusW, 6, 17, "Tx");
        wattrset(statusW, A_NORMAL);

    } else {
        wattrset(statusW, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

        mvwprintw(statusW, 6, 17, "Rx");
        wattrset(statusW, A_NORMAL);
    }

    mvwprintw(statusW, 8, 3, "Commands: PSK ON/OFF, SLOT ODD/EVEN, AUTOCQ ON/OFF");
    mvwprintw(statusW, 9, 3, "          AUTOREPLY ON/OFF, AUTOQSO ON/OFF");

    wnoutrefresh(statusW);
}

#define IDLE 0
#define ESC1 27
#define ESC2 91
#define UP 65
#define DOWN 66
#define RIGHT 67
#define LEFT 68
#define DEL 127
#define TAB 9
#define ENTER 10

/*
 * Keyboard state-machine, run ONLY on the UI thread (CQHandler).
 * ncurses is not thread-safe, so all curses access -- rendering AND keyboard
 * input -- lives on a single thread. processKey() contains no curses calls; it
 * only mutates UI state (editString, activeWin, cqIdx) and toggles modes.
 * Returns true if the display should be refreshed.
 */
static int kbdStatus = IDLE;

static bool processKey(int rawKey) {
    int key = toupper(rawKey);
    int ix = strlen(editString);

    switch (kbdStatus) {
        case IDLE:
            switch (key) {
                case ESC1:
                    kbdStatus = key;
                    break;

                case TAB:
                    if (++activeWin > TXWIN)
                        activeWin = CQWIN;
                    break;

                case ENTER:  // Parse the command and act on it
                    if (!strcmp(editString, "AUTOCQ ON"))
                        enableAutoCQ();
                    if (!strcmp(editString, "AUTOCQ OFF"))
                        disableAutoCQ();

                    if (!strcmp(editString, "PSK ON"))
                        enableReporting();
                    if (!strcmp(editString, "PSK OFF"))
                        disableReporting();

                    if (!strcmp(editString, "AUTOREPLY ON"))
                        enableAutoCQReply();
                    if (!strcmp(editString, "AUTOREPLY OFF"))
                        disableAutoCQReply();

                    if (!strcmp(editString, "AUTOQSO ON"))
                        enableAutoQSO();
                    if (!strcmp(editString, "AUTOQSO OFF"))
                        disableAutoQSO();

                    if (!strcmp(editString, "SLOT ODD"))
                        setActiveSlot(odd);

                    if (!strcmp(editString, "SLOT EVEN"))
                        setActiveSlot(even);

                    if (!strcmp(editString, "QUIT"))
                        programQuit();

                    editString[0] = '\0';
                    break;

                case DEL:
                    if (ix)
                        editString[ix - 1] = 0;
                    break;

                default:
                    if (ix < MAXTXSTRING - 1) {  /* Guard against buffer overflow */
                        editString[ix] = (char)key;
                        editString[ix + 1] = 0;
                    }
            }
            break;

        case ESC1:
            if (key == ESC2)
                kbdStatus = key;
            else
                kbdStatus = IDLE;
            break;

        case ESC2:
            if (activeWin == CQWIN) {
                if (key == UP) {
                    if (cqIdx)
                        cqIdx--;
                }
                if (key == DOWN) {
                    if (cqFirst < cqLast) {
                        if (cqIdx < (cqLast - 1))
                            cqIdx++;
                    } else if (cqIdx < (trafficWLines - 1))
                        cqIdx++;
                }
            }
            kbdStatus = IDLE;
            break;

        default:
            kbdStatus = IDLE;
    }

    return true;  // a keypress always warrants a redraw
}

/*
 * Legacy keyboard thread -- now a no-op. Keyboard input is handled on the UI
 * thread (CQHandler) via processKey() so that ncurses is only ever touched by
 * one thread. Kept as a thread body so main()'s create/join symmetry is intact.
 */
void *KBDHandler(void *vargp) {
    while (exitKBHThread == false) {
        usleep(100000);
    }
    return NULL;
}

void printCQ(struct decoder_results *cqReq) {
    // Print on the Log Window
    /* Reset the cursor position */
    char timeString[10];

    if (!strncmp(cqReq->call, dec_options.rcall, strlen(dec_options.rcall)))
        wattrset(cqW, COLOR_PAIR(2) | A_BOLD);  // QSO are GREEN
    else
        wattrset(cqW, A_NORMAL);

    /* convert to localtime */
    struct tm localv;
    struct tm *local = localtime_r(&cqReq->tempus, &localv);
    ft8slot_t thisSlot = ((cqReq->tempus / FT8_PERIOD) & 0x01) ? odd : even;

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wprintw(cqW, "%s %8dHz %.6s DE  %13s %2ddB %s\n",
            timeString,
            cqReq->freq,
            cqReq->cmd,
            cqReq->call,
            cqReq->snr,
            (thisSlot == odd) ? "ODD " : "EVEN");  // -20dB already computed

    wnoutrefresh(cqW);
    wattrset(cqW, A_NORMAL);
}

void printQSORemote(plain_message *logMsg) {
    char timeString[10];

    ft8slot_t thisSlot = ((logMsg->tempus / FT8_PERIOD) & 0x01) ? odd : even;

    if (!strncmp(logMsg->src, dec_options.rcall, strlen(dec_options.rcall))) {
        wattrset(qso, COLOR_PAIR(2) | A_BOLD);  // Print in RED
        /* convert to localtime */
        time_t t = time(NULL);
        struct tm localv;
        struct tm *local = localtime_r(&t, &localv);
        sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);
        wprintw(qso, "%s %dHz %s %s %s %s\n",
                timeString,
                logMsg->freq,
                logMsg->dest,
                logMsg->src,
                logMsg->message,
                (thisSlot == odd) ? "ODD " : "EVEN");  // -20dB already computed
    }
    else {
        wattrset(qso, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

        /* convert to localtime */
        struct tm localv;
        struct tm *local = localtime_r(&logMsg->tempus, &localv);

        /* and set the string */
        sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

        wprintw(qso, "%s %dHz  %3ddB %s %s %s %s\n",
                timeString,
                logMsg->freq,
                logMsg->snr,
                logMsg->dest,
                logMsg->src,
                logMsg->message,
                (thisSlot == odd) ? "ODD " : "EVEN");  // snr is a real dB estimate
    }
    wnoutrefresh(qso);
    wattrset(qso, A_NORMAL);
}

void displayTxString(char *txMessage) {
    char timeString[10];

    time_t rawtime;
    time(&rawtime);
    struct tm localv;
    struct tm *local = gmtime_r(&rawtime, &localv);

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wattrset(qso, COLOR_PAIR(2) | A_BOLD);  // Local messages are RED

    wprintw(qso, "%s %s\n", timeString, txMessage);

    wnoutrefresh(qso);
    wattrset(qso, A_NORMAL);
}

/* Update the Clock */
void printClock(void) {
    /* We need the time of the day */
    struct timeval lTime;
    gettimeofday(&lTime, NULL);

    thisSlot = ((lTime.tv_sec / FT8_PERIOD) & 0x01) ? odd : even;

    // time_t current_time = time(NULL);
    time_t current_time = lTime.tv_sec;
    struct tm tmv;
    struct tm tm = *localtime_r(&current_time, &tmv);

    wattrset(header, COLOR_PAIR(2) | A_BOLD);

    mvwprintw(header, 0, COLS - 24, "%d-%02d-%02d %02d:%02d:%02d %s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec, (thisSlot == odd) ? "O" : "E");
    wnoutrefresh(header);
}

void printLog(plain_message *logMsg) {
    char timeString[10];

    wattrset(trafficW, A_NORMAL);

    if (!strncmp(logMsg->dest, "CQ", 2))             // CQ messages are RED
        wattrset(trafficW, COLOR_PAIR(2) | A_BOLD);  // QSO are GREEN

    if (!strncmp(logMsg->dest, dec_options.rcall, strlen(dec_options.rcall)))
        wattrset(trafficW, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

    /* convert to localtime */
    struct tm localv;
    struct tm *local = localtime_r(&logMsg->tempus, &localv);
    ft8slot_t thisSlot = ((logMsg->tempus / FT8_PERIOD) & 0x01) ? odd : even;

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wprintw(trafficW, "%s %dHz  %3ddB %s %s %s %s\n",
            timeString,
            logMsg->freq,
            logMsg->snr,
            logMsg->dest,
            logMsg->src,
            logMsg->message,
            (thisSlot == odd) ? "ODD " : "EVEN");  // snr is a real dB estimate

    wnoutrefresh(trafficW);
    wattrset(trafficW, A_NORMAL);
}

// Print on the Call window
void printCall(bool refresh) {
    if (!refresh)
        return;

    wmove(call, 0, 0);  // Y,X
    werase(call);
    wattrset(call, COLOR_PAIR(1) | A_BOLD);
    waddstr(call, "CMD>");
    waddstr(call, editString);

    wnoutrefresh(call);
    wattrset(call, A_NORMAL);
}

/* CQ Handler Thread */
void *CQHandler(void *vargp) {
    static bool termRefresh = true;
    int dynamicRefresh = 0;
    uint32_t clockRefresh = 60;

    while (exitCQThread == false) {
        struct decoder_results dr;
        struct plain_message qsoMsg;
        struct plain_message logMsg;

        if (tsq_pop(log_queue, &LOGlock, &logMsg)) {
            printLog(&logMsg);
            termRefresh = true;
        }
        if (tsq_pop(cq_queue, &CQlock, &dr)) {
            printCQ(&dr);
            termRefresh = true;
        }
        /* Poll the keyboard on the UI thread (ncurses is single-threaded here).
           Drain all pending keys this iteration. */
        int key;
        while ((key = wgetch(call)) != ERR) {
            if (processKey(key))
                termRefresh = true;
        }
        if (tsq_pop(qso_queue, &QSOlock, &qsoMsg)) {
            printQSORemote(&qsoMsg);
            termRefresh = true;
        }

        printCall(termRefresh);
        refreshStatus(termRefresh);

        /* Refresh the clock about once per second (it only shows seconds).
           The loop runs every 10 ms, so 100 iterations ~= 1 s. */
        bool needUpdate = termRefresh;
        if (clockRefresh-- == 0) {
            printClock();
            clockRefresh = 100;
            needUpdate = true;
        }

        /* Single coalesced flush to the terminal, only when something changed.
           The static window borders are drawn once at init and are not
           repainted here, which keeps SSH traffic minimal. */
        if (needUpdate)
            doupdate();

        termRefresh = false;

        usleep(10000); /* Wait 10 msec.*/
    }

    return NULL;
}

void close_TxThread(void) {
    exitTxThread = true;
}

void close_KbhThread(void) {
    exitKBHThread = true;
}

void close_CQThread(void) {
    exitCQThread = true;
}