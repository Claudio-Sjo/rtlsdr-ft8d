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

#include "./rtlsdr_ft8d.h"
#include "./ft8tx/FT8Types.h"

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

WINDOW *logw0L, *logw0R, *logwL, *logwR, *logwLH;

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
char destString[MAXTXSTRING];

struct decoder_results cqReq[MAXCQ];
struct plain_message qsoReq[MAXCQ];
int cqFirst, cqLast, cqIdx;
int qsoFirst, qsoLast, qsoIdx;

int logWLines;
int qsoWLines;
uint32_t qsoFreq;
uint32_t reportedCQ;

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

    /* create subwindow on stdscr */

    header = subwin(stdscr, 1, COLS - 2, 1, 1);

    logw0L = subwin(stdscr, LINES / 2, COLS / 2, 2, 1);
    logw0R = subwin(stdscr, LINES / 2, (COLS / 2) - 2, 2, (COLS / 2) + 1);
    logwLH = subwin(stdscr, 1, COLS / 2 - 3, 3, 3);
    logwL = subwin(stdscr, (LINES / 2) - 3, COLS / 2 - 3, 4, 3);
    logwR = subwin(stdscr, (LINES / 2) - 2, (COLS / 2) - 5, 3, (COLS / 2) + 2);

    logWLines = (LINES / 2) - 3;  // Lines for scroll need not to include the Header Line

    qso0 = subwin(stdscr, (LINES / 2) - 5, COLS - 2, LINES / 2 + 2, 1);
    qso = subwin(stdscr, (LINES / 2) - 7, COLS - 4, LINES / 2 + 3, 3);
    qsoWLines = getmaxy(qso);

    call0 = subwin(stdscr, 3, COLS - 2, LINES - 4, 1);
    call = subwin(stdscr, 1, COLS - 5, LINES - 3, 3);

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

    attrset(COLOR_PAIR(1) | A_BOLD);
    wattrset(logw0L, COLOR_PAIR(3) | A_BOLD);
    wattrset(logw0R, COLOR_PAIR(3) | A_BOLD);
    wattrset(qso0, COLOR_PAIR(3) | A_BOLD);
    wattrset(call0, COLOR_PAIR(4) | A_BOLD);

    box(stdscr, 0, 0);

    box(logw0L, 0, 0);
    box(logw0R, 0, 0);

    box(qso0, 0, 0);

    box(call0, 0, 0);

    wattrset(header, COLOR_PAIR(2) | A_BOLD);
    /* Print the header */
    mvwprintw(header, 0, 1, "%s - %s  %dHz\n", dec_options.rcall, dec_options.rloc, qsoFreq);

    mvwprintw(header, 0, COLS / 2 - 10, "rtlsdr FT8 - QSO Mode\n");

    mvwprintw(header, 0, COLS - (strlen(rtlsdr_ft8d_version) + 6), "%s", rtlsdr_ft8d_version);

    mvwprintw(logw0R, 0, 10, " CQ Reply Mode ");

    wattrset(logwL, A_NORMAL);
    wattrset(logwR, A_NORMAL);
    wattrset(qso, A_NORMAL);
    wattrset(call, A_NORMAL);

    scrollok(logwL, true);
    idlok(logwL, true);

    scrollok(logwL, true);
    idlok(logwL, true);

    scrollok(qso, true);
    idlok(qso, true);

    refresh();

    /* Initiate the call's database */
    for (uint32_t i; i < MAXCQ; i++) {
        sprintf(cqReq[i].call, "NONE");
    }

    sprintf(txString, "");
    sprintf(editString, "");

    cqFirst = cqLast = cqIdx = 0;
    qsoFirst = qsoLast = qsoIdx = 0;

    /* End initialization */
    return (0);
}

int close_ncurses() {
    getch();

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
    struct sockaddr serv_addr = {AF_UNIX, SOCKNAME};

    FT8Msg Txletter, Rxletter;
    char key = 0;

    txStatusFlag = TX_IDLE;

    while (true) {
        if (tx_queue.size()) {
            // Receive the message from the queue
            pthread_mutex_lock(&TXlock);
            Txletter = tx_queue.front();
            tx_queue.erase(tx_queue.begin());
            pthread_mutex_unlock(&TXlock);

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
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            kbd_queue.push_back(key);
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            valread = read(client_fd, &Rxletter, sizeof(Rxletter));
            if (!valread) {
                perror("Error, nothing read");
            }
            txStatusFlag = TX_ONGOING;
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            kbd_queue.push_back(key);
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            valread = read(client_fd, &Rxletter, sizeof(Rxletter));
            if (!valread) {
                perror("Error, nothing read");
            }
            txStatusFlag = TX_END;
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            kbd_queue.push_back(key);
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure
            sleep(1);

            txStatusFlag = TX_IDLE;
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            kbd_queue.push_back(key);
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            // closing the connected socket
            close(client_fd);
        }
        usleep(10000);  // Wait 10msec
    }
}

// End of the tx interface

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

/* KBD Handler Thread */
/*
Time for addition of Frequency Handling: when in Freetext mode, use the local Frequency
When in CQ answer or in QSO mode, use the frequency from the CQ or QSO
*/
void *KBDHandler(void *vargp) {
    static int status = IDLE;
    FT8Msg Txletter;

    while (true) {
        /* CQlock */

        char key = toupper(wgetch(call));
        int ix = strlen(editString);

        switch (status) {
            case IDLE:
                switch (key) {
                    case ESC1:
                        status = key;
                        key = 0;
                        break;

                    case TAB:
                        if (++activeWin > TXWIN)
                            activeWin = CQWIN;
                        // key = 0;
                        break;

                    case ENTER:  // Activate transmission
                        switch (activeWin) {
                            case CQWIN:
                                sprintf(Txletter.ft8Message, "FT8Tx %d %s %s %s ", cqReq[(cqIdx + cqFirst) % logWLines].freq, cqReq[(cqIdx + cqFirst) % logWLines].call, dec_options.rcall, dec_options.rloc);
                                break;
                            case QSOWIN:
                                sprintf(Txletter.ft8Message, "FT8Tx %d %s %s %s", qsoReq[(qsoIdx + qsoFirst) % qsoWLines].freq, qsoReq[(qsoIdx + qsoFirst) % qsoWLines].dest, dec_options.rcall, editString);
                                break;
                            case TXWIN:
                                sprintf(Txletter.ft8Message, "FT8Tx %d %s", qsoFreq, editString);
                                break;
                            default:
                                sprintf(Txletter.ft8Message, "FT8Tx %d SA0PRF SA0PRF JO99", qsoFreq);
                                break;
                        }

                        pthread_mutex_lock(&TXlock);  // Protect key queue structure
                        tx_queue.push_back(Txletter);
                        pthread_mutex_unlock(&TXlock);  // Protect key queue structure
                        break;
                    case DEL:
                        if (ix)
                            editString[ix - 1] = 0;
                        break;

                    default:
                        editString[ix] = key;
                        editString[ix + 1] = 0;
                }
                // wprintw(qso, "Key Pressed %d, editString %s\n", key, editString);
                // wrefresh(qso);
                break;
            case ESC1:
                if (key == ESC2) {
                    status = key;
                    key = 0;
                } else
                    status = IDLE;
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
                        } else if (cqIdx < (logWLines - 1))
                            cqIdx++;
                    }
                    key = 0;
                }
                status = IDLE;
                break;

            default:
                status = IDLE;
        }

        // wprintw(qso, "Key Pressed %d\n", key);
        // wprintw(qso, "Active Win %d\n", activeWin);
        // wrefresh(qso);

        pthread_mutex_lock(&KBDlock);  // Protect key queue structure
        kbd_queue.push_back(key);
        pthread_mutex_unlock(&KBDlock);  // Protect key queue structure
        usleep(10000);                   // Wait 10msec
    }
}

void printCQ(bool refresh) {
    if (!refresh)
        return;

    // Print on the Log Window
    /* Reset the cursor position */

    if (cqFirst != cqLast) {
        wmove(logwR, 0, 0);
        wrefresh(logwR);

        wattrset(logwR, A_NORMAL | A_BOLD);

        wprintw(logwR, "    Incoming CQ Requests\n");

        if (cqLast > cqFirst) {
            for (int i = cqFirst; i < cqLast; i++) {
                if (i == cqIdx) {
                    if (activeWin == CQWIN)
                        wattrset(logwR, COLOR_PAIR(12) | A_BOLD);
                    else
                        wattrset(logwR, COLOR_PAIR(2) | A_BOLD);
                } else
                    wattrset(logwR, A_NORMAL);

                wprintw(logwR, " %.6s DE  %13s freq. %8dHz %2ddB\n",
                        cqReq[i].cmd,
                        cqReq[i].call,
                        cqReq[i].freq,
                        cqReq[i].snr);
            }
        } else {
            int idx = cqFirst;
            for (int i = 0; i < logWLines; i++) {
                if (i == cqIdx) {
                    if (activeWin == CQWIN)
                        wattrset(logwR, COLOR_PAIR(12) | A_BOLD);
                    else
                        wattrset(logwR, COLOR_PAIR(2) | A_BOLD);
                } else
                    wattrset(logwR, A_NORMAL);

                wprintw(logwR, " %.6s DE  %13s freq. %8dHz %2ddB\n",
                        cqReq[(i + cqFirst) % logWLines].cmd,
                        cqReq[(i + cqFirst) % logWLines].call,
                        cqReq[(i + cqFirst) % logWLines].freq,
                        cqReq[(i + cqFirst) % logWLines].snr);

                idx = (idx + 1) % logWLines;
            }
        }
        wattrset(logwR, A_NORMAL);
        wrefresh(logwR);
    }
}

// Print on the QSO window
void printQSO(bool refresh) {
    if (!refresh)
        return;

    if (qsoFirst != qsoLast) {
        if (qsoLast > qsoFirst) {
            for (int i = qsoFirst; i < qsoLast; i++) {
                if (i == qsoIdx) {
                    if (activeWin == QSOWIN)
                        wattrset(qso, COLOR_PAIR(12) | A_BOLD);
                    else
                        wattrset(qso, COLOR_PAIR(2) | A_BOLD);
                } else
                    wattrset(qso, A_NORMAL);

                char timeString[10];

                /* convert to localtime */
                struct tm *local = localtime(&qsoReq[i].tempus);

                /* and set the string */
                sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

                wprintw(qso, " %s %dHz  %ddB %s %s\n",
                        timeString,
                        qsoReq[i].freq,
                        qsoReq[i].snr - 20,
                        qsoReq[i].src,
                        qsoReq[i].message);
            }
        } else {
            int idx = qsoFirst;
            for (int i = 0; i < qsoWLines; i++) {
                if (i == qsoIdx) {
                    if (activeWin == QSOWIN)
                        wattrset(qso, COLOR_PAIR(12) | A_BOLD);
                    else
                        wattrset(qso, COLOR_PAIR(2) | A_BOLD);
                } else
                    wattrset(qso, A_NORMAL);

                wprintw(qso, " %dHz  %ddB %s %s\n",
                        qsoReq[i].freq,
                        qsoReq[i].snr - 20,
                        qsoReq[i].src,
                        qsoReq[i].message);

                idx = (idx + 1) % qsoWLines;
            }
        }
        wattrset(qso, A_NORMAL);
        wrefresh(qso);
    }
}

// Print on the Call window
void printCall(bool refresh) {
    if (!refresh)
        return;

    if (activeWin == CQWIN)
        sprintf(txString, "%s %s %s ", cqReq[(cqIdx + cqFirst) % logWLines].call, dec_options.rcall, dec_options.rloc);
    if (activeWin == QSOWIN)
        sprintf(txString, "%s %s", qsoReq[(qsoIdx + qsoFirst) % qsoWLines].dest, dec_options.rcall);
    if (activeWin == TXWIN)
        sprintf(txString, "");

    wmove(call, 0, 0);  // Y,X
    werase(call);
    wattrset(call, COLOR_PAIR(1) | A_BOLD);
    waddstr(call, txString);
    wattrset(call, COLOR_PAIR(3) | A_BOLD);
    waddstr(call, editString);

    switch (txStatusFlag) {
        case TX_IDLE:

            break;
        case TX_WAITING:
            wmove(call, 0, COLS / 2);  // Y,X
            wattrset(call, COLOR_PAIR(2) | A_BOLD);
            waddstr(call, txString);
            waddstr(call, editString);
            break;
        case TX_ONGOING:
            wmove(call, 0, COLS / 2);  // Y,X
            wattrset(call, COLOR_PAIR(3) | A_BOLD);
            waddstr(call, txString);
            waddstr(call, editString);
            break;
        case TX_END:
            wmove(call, 0, COLS / 2);  // Y,X
            wattrset(call, A_NORMAL);
            waddstr(call, txString);
            waddstr(call, editString);
            break;
    }

    wrefresh(call);
    wattrset(call, A_NORMAL);
}

void printLog(plain_message *logMsg) {
    char timeString[10];

    wattrset(logwL, A_NORMAL);

    if (!strncmp(logMsg->dest, "CQ", 2))          // CQ messages are RED
        wattrset(logwL, COLOR_PAIR(2) | A_BOLD);  // QSO are GREEN

    if (!strncmp(logMsg->message, dec_options.rcall, strlen(dec_options.rcall)))
        wattrset(logwL, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

    /* convert to localtime */
    struct tm *local = localtime(&logMsg->tempus);

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wprintw(logwL, "%s %dHz  %3ddB %s %s %s\n",
            timeString,
            logMsg->freq,
            logMsg->snr - 20,
            logMsg->dest,
            logMsg->src,
            logMsg->message);

    wrefresh(logwL);
    wattrset(logwL, A_NORMAL);
}

/* A new CQ request has been received, let's add it to the table */
bool addToCQ(struct decoder_results *dr) {
    /* Is the caller already in the table? */
    for (uint32_t i = 0; i < logWLines; i++)
        if (strcmp(dr->call, cqReq[i].call) == 0) {
            cqReq[i].tempus = dr->tempus;
            return false;  // Found! we don't add anything
        }

    cqReq[cqLast].freq = dr->freq;
    cqReq[cqLast].snr = dr->snr;
    strcpy(cqReq[cqLast].cmd, dr->cmd);
    strcpy(cqReq[cqLast].call, dr->call);
    cqReq[cqLast].tempus = dr->tempus;

    cqLast = (cqLast + 1) % logWLines;
    if (cqLast == cqFirst)
        cqFirst = (cqFirst + 1) % logWLines;

    return true;
}

bool addToQSO(struct plain_message *qsoMsg) {
    for (uint32_t i = 0; i < qsoWLines; i++)
        if (strcmp(qsoMsg->src, qsoReq[i].src) == 0)
            return false;  // Found! we don't add anything

    qsoReq[qsoLast].freq = qsoMsg->freq;
    qsoReq[qsoLast].snr = qsoMsg->snr;
    strcpy(qsoReq[qsoLast].message, qsoMsg->message);
    strcpy(qsoReq[qsoLast].src, qsoMsg->src);
    qsoReq[qsoLast].tempus = qsoMsg->tempus;

    qsoLast = (qsoLast + 1) % qsoWLines;
    if (qsoLast == qsoFirst)
        qsoFirst = (qsoFirst + 1) % qsoWLines;

    return true;
}

void printHeaders(void) {
    wattrset(logwLH, A_NORMAL | A_BOLD);
    mvwprintw(logwLH, 0, 0, "   Freq       SNR Msg\n");
    wattrset(logwLH, A_NORMAL);

    wrefresh(logwLH);

    wattrset(logwR, A_NORMAL | A_BOLD);

    wprintw(logwR, "    Incoming CQ Requests\n");
    wattrset(logwR, A_NORMAL);

    wrefresh(logwR);
}

// Focus on the chosen window
void focusOnWin(int whatWin) {
    switch (whatWin) {
        case CQWIN:
            box(logw0R, 0, 0);
            mvwprintw(logw0R, 0, 10, " CQ Reply Mode ");
            wrefresh(logw0R);
            box(qso0, 0, 0);
            wrefresh(qso0);
            box(call0, 0, 0);
            wrefresh(call0);
            break;
        case QSOWIN:
            box(qso0, 0, 0);
            mvwprintw(qso0, 0, 10, " QSO Reply Mode ");
            wrefresh(qso0);
            box(logw0R, 0, 0);
            wrefresh(logw0R);
            box(call0, 0, 0);
            wrefresh(call0);
            break;
        case TXWIN:
            box(call0, 0, 0);
            mvwprintw(call0, 0, 10, " TX Freetext Mode ");
            wrefresh(call0);
            box(qso0, 0, 0);
            wrefresh(qso0);
            box(logw0R, 0, 0);
            wrefresh(logw0R);
            break;
    }
}

#define FORCEREFRESH 100  // in 10th of msec

/* CQ Handler Thread */
void *CQHandler(void *vargp) {
    static bool termRefresh = false;
    int dynamicRefresh = 0;

    while (true) {
        char key;
        struct decoder_results dr;
        struct plain_message qsoMsg;
        struct plain_message logMsg;

        if (log_queue.size()) {
            pthread_mutex_lock(&LOGlock);
            logMsg = log_queue.front();
            log_queue.erase(log_queue.begin());
            pthread_mutex_unlock(&LOGlock);

            printLog(&logMsg);
        }
        if (cq_queue.size()) {
            pthread_mutex_lock(&CQlock);
            dr = cq_queue.front();
            cq_queue.erase(cq_queue.begin());
            pthread_mutex_unlock(&CQlock);

            printCQ(addToCQ(&dr));
        }
        if (kbd_queue.size()) {
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            key = kbd_queue.front();
            kbd_queue.erase(kbd_queue.begin());
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            if (key == TAB)
                focusOnWin(activeWin);

            termRefresh = true;
        }
        if (qso_queue.size()) {
            pthread_mutex_lock(&QSOlock);

            qsoMsg = qso_queue.front();
            qso_queue.erase(qso_queue.begin());
            pthread_mutex_unlock(&QSOlock);

            printQSO(addToQSO(&qsoMsg));
        }
        /* if needed update the screen */
        if (termRefresh)
            dynamicRefresh = 0;
        else
            dynamicRefresh++;
        if (dynamicRefresh > FORCEREFRESH) {
            termRefresh = true;
            dynamicRefresh = 0;
        }

        printCQ(termRefresh);
        printQSO(termRefresh);
        printCall(termRefresh);
        if (termRefresh)
            refresh();
        termRefresh = false;
        usleep(10000); /* Wait 10 msec.*/
    }
}
