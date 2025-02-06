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

// struct decoder_results cqReq[MAXCQ];
struct plain_message qsoReq[MAXCQ];
int cqFirst, cqLast, cqIdx;
int qsoFirst, qsoLast, qsoIdx;

int trafficWLines;
int qsoWLines;
uint32_t qsoFreq;
uint32_t reportedCQ;
ft8slot_t thisSlot;

bool transmitting = false;

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
    box(stdscr, 0, 0);

    box(trafficW0, 0, 0);
    mvwprintw(trafficW0, 0, 10, " FT8 Traffic ");
    wrefresh(trafficW0);

    box(statusW0, 0, 0);
    mvwprintw(statusW0, 0, 10, " Transceiver Status ");
    wrefresh(statusW0);

    box(qso0, 0, 0);
    mvwprintw(qso0, 0, 10, " Ongoing QSO ");
    wrefresh(qso0);

    box(cqW0, 0, 0);
    mvwprintw(cqW0, 0, 10, " Incoming CQ ");
    wrefresh(cqW0);
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
    struct tm tm = *localtime(&currentTime);

    /* create subwindow on stdscr */

    header = subwin(stdscr, 1, COLS - 2, 1, 1);

    /* Status Window is in the right, traffic window is at the left */

    // WINDOW *subwin(n_raw, n_col, init_raw, init_col);

    cqW = subwin(stdscr, (LINES / 2) - 3, COLS / 2 - 3, 4, 3);
    cqW0 = subwin(stdscr, LINES / 2, COLS / 2, 2, 1);

    statusW = subwin(stdscr, (LINES / 2) - 3, COLS / 2 - 5, 4, (COLS / 2) + 2);
    statusW0 = subwin(stdscr, LINES / 2, COLS / 2 - 3, 2, (COLS / 2) + 1);

    trafficW0 = subwin(stdscr, (LINES / 2) - 6, (COLS / 2) - 3, LINES / 2 + 2, (COLS / 2) + 1);
    trafficW = subwin(stdscr, (LINES / 2) - 8, (COLS / 2) - 5, LINES / 2 + 3, (COLS / 2) + 2);
    trafficWLines = (LINES / 2) - 8;  // Lines for scroll need not to include the Header Line

    qso0 = subwin(stdscr, (LINES / 2) - 6, (COLS / 2), (LINES / 2) + 2, 1);
    qso = subwin(stdscr, (LINES / 2) - 8, (COLS / 2) - 3, (LINES / 2) + 3, 3);
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
    wattrset(trafficW0, COLOR_PAIR(3) | A_BOLD);
    wattrset(cqW0, COLOR_PAIR(3) | A_BOLD);
    wattrset(qso0, COLOR_PAIR(3) | A_BOLD);
    wattrset(call0, COLOR_PAIR(4) | A_BOLD);
    wattrset(statusW0, COLOR_PAIR(3) | A_BOLD);

    box(stdscr, 0, 0);

    box(trafficW0, 0, 0);
    box(cqW0, 0, 0);

    box(qso0, 0, 0);

    box(call0, 0, 0);

    box(statusW0, 0, 0);

    wattrset(header, COLOR_PAIR(2) | A_BOLD);
    /* Print the header */
    mvwprintw(header, 0, 1, "%s - %s  %dHz\n", dec_options.rcall, dec_options.rloc, qsoFreq);

    mvwprintw(header, 0, COLS / 2 - 12, "rtlsdr FT8 %s - QSO Mode\n", rtlsdr_ft8d_version);

    //   mvwprintw(header, 0, COLS - 23, "%d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    mvwprintw(cqW0, 0, 10, " CQ Reply Mode ");

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

    /* Headers */
    box(trafficW0, 0, 0);
    mvwprintw(trafficW0, 0, 10, " FT8 Traffic ");
    wrefresh(trafficW0);

    box(statusW0, 0, 0);
    mvwprintw(statusW0, 0, 10, " Transceiver Status ");
    wrefresh(statusW0);

    box(qso0, 0, 0);
    mvwprintw(qso0, 0, 10, " Ongoing QSO ");
    wrefresh(qso0);

    box(cqW0, 0, 0);
    mvwprintw(cqW0, 0, 10, " Incoming CQ ");
    wrefresh(cqW0);

    refresh();

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
            setTransmitting();

            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            kbd_queue.push_back(key);
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            valread = read(client_fd, &Rxletter, sizeof(Rxletter));
            if (!valread) {
                perror("Error, nothing read");
            }
            txStatusFlag = TX_END;
            resetTransmitting();

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
        wattrset(cqW, A_NORMAL);

    } else {
        wattrset(statusW, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

        mvwprintw(statusW, 6, 17, "Rx");
        wattrset(cqW, A_NORMAL);
    }

    mvwprintw(statusW, 8, 3, "Commands: PSK ON/OFF, SLOT ODD/EVEN, AUTOCQ ON/OFF");
    mvwprintw(statusW, 9, 3, "          AUTOREPLY ON/OFF, AUTOQSO ON/OFF");

    wrefresh(statusW);
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

                    case ENTER:  // Parse the message and take a decision
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

                        sprintf(editString, "");
                        /*
                                                pthread_mutex_lock(&TXlock);  // Protect key queue structure
                                                tx_queue.push_back(Txletter);
                                                pthread_mutex_unlock(&TXlock);  // Protect key queue structure
                                                */
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
                        } else if (cqIdx < (trafficWLines - 1))
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

void printCQ(struct decoder_results *cqReq) {
    // Print on the Log Window
    /* Reset the cursor position */
    char timeString[10];

    if (!strncmp(cqReq->call, dec_options.rcall, strlen(dec_options.rcall)))
        wattrset(cqW, COLOR_PAIR(2) | A_BOLD);  // QSO are GREEN
    else
        wattrset(cqW, A_NORMAL);

    /* convert to localtime */
    struct tm *local = localtime(&cqReq->tempus);
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

    wrefresh(cqW);
    wattrset(cqW, A_NORMAL);
}

void printQSORemote(plain_message *logMsg) {
    char timeString[10];

    wattrset(qso, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

    /* convert to localtime */
    struct tm *local = localtime(&logMsg->tempus);
    ft8slot_t thisSlot = ((logMsg->tempus / FT8_PERIOD) & 0x01) ? odd : even;

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wprintw(qso, "%s %dHz  %3ddB %s %s %s %s\n",
            timeString,
            logMsg->freq,
            logMsg->snr - 20,
            logMsg->dest,
            logMsg->src,
            logMsg->message,
            (thisSlot == odd) ? "ODD " : "EVEN");  // -20dB already computed

    wrefresh(qso);
    wattrset(qso, A_NORMAL);
}

void displayTxString(char *txMessage) {
    char timeString[10];

    time_t rawtime;
    time(&rawtime);
    struct tm *local = gmtime(&rawtime);

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wattrset(qso, COLOR_PAIR(2) | A_BOLD);  // Local messages are RED

    wprintw(qso, "%s, %s\n", timeString, txMessage);

    wrefresh(qso);
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
    struct tm tm = *localtime(&current_time);

    wattrset(header, COLOR_PAIR(2) | A_BOLD);

    mvwprintw(header, 0, COLS - 24, "%d-%02d-%02d %02d:%02d:%02d %s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
              tm.tm_hour, tm.tm_min, tm.tm_sec, (thisSlot == odd) ? "O" : "E");
    wrefresh(header);
}

void printLog(plain_message *logMsg) {
    char timeString[10];

    wattrset(trafficW, A_NORMAL);

    if (!strncmp(logMsg->dest, "CQ", 2))             // CQ messages are RED
        wattrset(trafficW, COLOR_PAIR(2) | A_BOLD);  // QSO are GREEN

    if (!strncmp(logMsg->message, dec_options.rcall, strlen(dec_options.rcall)))
        wattrset(trafficW, COLOR_PAIR(3) | A_BOLD);  // QSO are GREEN

    /* convert to localtime */
    struct tm *local = localtime(&logMsg->tempus);
    ft8slot_t thisSlot = ((logMsg->tempus / FT8_PERIOD) & 0x01) ? odd : even;

    /* and set the string */
    sprintf(timeString, "%02d:%02d:%02d", local->tm_hour, local->tm_min, local->tm_sec);

    wprintw(trafficW, "%s %dHz  %3ddB %s %s %s %s\n",
            timeString,
            logMsg->freq,
            logMsg->snr - 20,
            logMsg->dest,
            logMsg->src,
            logMsg->message,
            (thisSlot == odd) ? "ODD " : "EVEN");  // -20dB already computed

    wrefresh(trafficW);
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

    wrefresh(call);
    wattrset(call, A_NORMAL);
}

/* CQ Handler Thread */
void *CQHandler(void *vargp) {
    static bool termRefresh = true;
    int dynamicRefresh = 0;
    uint32_t clockRefresh = 60;

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
            termRefresh = true;
        }
        if (cq_queue.size()) {
            pthread_mutex_lock(&CQlock);
            dr = cq_queue.front();
            cq_queue.erase(cq_queue.begin());
            pthread_mutex_unlock(&CQlock);

            printCQ(&dr);
            termRefresh = true;
        }
        if (kbd_queue.size()) {
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            key = kbd_queue.front();
            kbd_queue.erase(kbd_queue.begin());
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            /*
                        if (key == TAB)
                            focusOnWin(activeWin);
            */
            termRefresh = true;
        }
        if (qso_queue.size()) {
            pthread_mutex_lock(&QSOlock);

            qsoMsg = qso_queue.front();
            qso_queue.erase(qso_queue.begin());
            pthread_mutex_unlock(&QSOlock);

            printQSORemote(&qsoMsg);
            termRefresh = true;
        }

        printCall(termRefresh);
        refreshStatus(termRefresh);

        if (clockRefresh-- == 0) {
            printClock();
            clockRefresh = 20;
            wrefresh(stdscr);
            refreshBoxes();
            refresh();
        } else {
            if (termRefresh) {
                wrefresh(stdscr);
                refreshBoxes();
                refresh();
            }
        }
        termRefresh = false;

        usleep(10000); /* Wait 10 msec.*/
    }
}
