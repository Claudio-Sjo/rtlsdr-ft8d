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

#include "./rtlsdr_ft8d.h"

extern char rtlsdr_ft8d_version[];
extern char pskreporter_app_version[];
extern std::vector<struct decoder_results> cq_queue;
extern std::vector<struct plain_message> qso_queue;
extern pthread_mutex_t CQlock;
extern pthread_mutex_t QSOlock;
extern struct decoder_options dec_options;

pthread_mutex_t KBDlock;
std::vector<char> kbd_queue;

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

struct decoder_results cqReq[MAXCQ];
int cqFirst, cqLast, cqIdx;

int logWLines;

int init_ncurses() {
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

    call0 = subwin(stdscr, 3, COLS - 2, LINES - 4, 1);
    call = subwin(stdscr, 1, COLS - 5, LINES - 3, 3);

    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);
    init_pair(4, COLOR_CYAN, COLOR_BLACK);
    init_pair(11, COLOR_YELLOW, COLOR_RED);
    init_pair(12, COLOR_BLACK, COLOR_RED);
    init_pair(13, COLOR_BLACK, COLOR_GREEN);
    init_pair(14, COLOR_BLACK, COLOR_CYAN);

    attrset(COLOR_PAIR(1) | A_BOLD);
    wattrset(logw0L, COLOR_PAIR(3) | A_BOLD);
    wattrset(qso0, COLOR_PAIR(1) | A_BOLD);
    wattrset(call0, COLOR_PAIR(4) | A_BOLD);

    box(stdscr, 0, 0);

    box(logw0L, 0, 0);
    box(logw0R, 0, 0);

    box(qso0, 0, 0);

    box(call0, 0, 0);

    wattrset(header, COLOR_PAIR(2) | A_BOLD);
    /* Print the header */
    mvwprintw(header, 0, 1, "SA0PRF\n");

    mvwprintw(header, 0, COLS / 2 - 10, "rtlsdr FT8 - QSO Mode\n");

    mvwprintw(header, 0, COLS - (strlen(rtlsdr_ft8d_version) + 6), rtlsdr_ft8d_version);

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

    cqFirst = cqLast = cqIdx = 0;

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

#define IDLE 0
#define ESC1 27
#define ESC2 91
#define UP 65
#define DOWN 66
#define RIGHT 67
#define LEFT 68

/* KBD Handler Thread */
void *KBDHandler(void *vargp) {
    static int status = IDLE;
    while (true) {
        /* CQlock */

        char key = wgetch(call);

        switch (status) {
            case IDLE:
                if (key == ESC1) {
                    status = key;
                    key = 0;

                } else {
                    if (key == 9) {
                        if (++activeWin > TXWIN)
                            activeWin = CQWIN;
                        key = 0;
                    }
                }

                break;
            case ESC1:
                if (key == ESC2) {
                    status = key;
                    key = 0;
                }
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

                wprintw(logwR, "%.6s DE  %13s freq. %8dHz %2ddB\n",
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

                wprintw(logwR, "%.6s DE  %13s freq. %8dHz %2ddB\n",
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
    // Print on the call window
    sprintf(txString, "%s %s %s", cqReq[(cqIdx + cqFirst) % logWLines].call, dec_options.rcall, dec_options.rloc);
    if (activeWin == TXWIN)
        wattrset(call, COLOR_PAIR(12) | A_BOLD);
    else
        wattrset(call, COLOR_PAIR(2) | A_BOLD);
    wmove(call, 0, 0);
    werase(call);
    wprintw(call, "%s", txString);
    wrefresh(call);
    wattrset(call, A_NORMAL);
}

/* A new CQ request has been received, let's add it to the table */
bool addToCQ(struct decoder_results *dr) {
    /* Is the caller already in the table? */
    for (uint32_t i = 0; i < logWLines; i++)
        if (strcmp(dr->call, cqReq[i].call) == 0)
            return false;  // Found! we don't add anything

    cqReq[cqLast].freq = dr->freq;
    cqReq[cqLast].snr = dr->snr;
    strcpy(cqReq[cqLast].cmd, dr->cmd);
    strcpy(cqReq[cqLast].call, dr->call);

    cqLast = (cqLast + 1) % logWLines;
    if (cqLast == cqFirst)
        cqFirst = (cqFirst + 1) % logWLines;

    return true;
}

bool addToQSO(struct plain_message *qsoMsg) {
    char *dst = strtok(qsoMsg->message, " ");

    if (activeWin == QSOWIN)
        wattrset(logwR, COLOR_PAIR(13) | A_BOLD);
    else
        wattrset(logwR, COLOR_PAIR(3) | A_BOLD);

    wprintw(qso, "%s\n", qsoMsg->message);
    wattrset(qso, A_NORMAL);
    wrefresh(qso);

    return true;
}

/* CQ Handler Thread */
void *CQHandler(void *vargp) {
    static bool termRefresh = false;

    while (true) {
        char key;
        struct decoder_results dr;
        struct plain_message qsoMsg;

        if (cq_queue.size()) {
            pthread_mutex_lock(&CQlock);
            dr = cq_queue.front();
            cq_queue.erase(cq_queue.begin());
            pthread_mutex_unlock(&CQlock);

            if (addToCQ(&dr))
                termRefresh = true;
        }
        if (kbd_queue.size()) {
            pthread_mutex_lock(&KBDlock);  // Protect key queue structure
            key = kbd_queue.front();
            kbd_queue.erase(kbd_queue.begin());
            pthread_mutex_unlock(&KBDlock);  // Protect key queue structure

            termRefresh = true;
        }
        if (qso_queue.size()) {
            pthread_mutex_lock(&QSOlock);

            qsoMsg = qso_queue.front();
            qso_queue.erase(qso_queue.begin());
            pthread_mutex_unlock(&QSOlock);

            termRefresh = true;
        }
        /* if needed update the screen */
        printCQ(termRefresh);
        if (termRefresh)
            refresh();
        termRefresh = false;
        usleep(10000); /* Wait 10 msec.*/
    }
}
