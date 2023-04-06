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
extern pthread_mutex_t CQlock;

pthread_mutex_t KBDlock;
std::vector<char> kbd_queue;

WINDOW *root;

WINDOW *logw, *logw0, *logw1;

WINDOW *qso, *qso0;

WINDOW *call, *call0;

int logWLines;

int init_ncurses() {
    initscr();

    /* create subwindow on stdscr */

    root = subwin(stdscr, 1, COLS - 4, 1, 2);

    logw0 = subwin(stdscr, LINES / 2, COLS - 4, 2, 2);
    logw = subwin(stdscr, (LINES / 2) - 2, COLS - 6, 2, 3);
    logw1 = subwin(stdscr, (LINES / 2) - 3, COLS - 6, 4, 3);

    logWLines = (LINES / 2) - 3;

    qso0 = subwin(stdscr, (LINES / 2) - 7, COLS - 4, LINES / 2 + 2, 2);
    qso = subwin(stdscr, (LINES / 2) - 9, COLS - 6, LINES / 2 + 3, 3);

    call0 = subwin(stdscr, 3, COLS - 4, LINES - 5, 2);
    call = subwin(stdscr, 3, COLS - 6, LINES - 5, 3);

    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_GREEN, COLOR_BLACK);
    init_pair(4, COLOR_CYAN, COLOR_BLACK);

    attrset(COLOR_PAIR(1) | A_BOLD);
    wattrset(logw0, COLOR_PAIR(3) | A_BOLD);
    wattrset(qso0, COLOR_PAIR(1) | A_BOLD);
    wattrset(call0, COLOR_PAIR(4) | A_BOLD);

    box(stdscr, 0, 0);

    box(logw0, 0, 0);

    box(qso0, 0, 0);

    box(call0, 0, 0);

    wattrset(root, COLOR_PAIR(2) | A_BOLD);
    /* Print the header */
    mvwprintw(root, 0, 1, "SA0PRF\n");

    mvwprintw(root, 0, COLS / 2 - 10, "rtlsdr FT8 - QSO Mode\n");

    mvwprintw(root, 0, COLS - (strlen(rtlsdr_ft8d_version) + 6), rtlsdr_ft8d_version);

    wattrset(logw, A_NORMAL);
    wattrset(logw1, A_NORMAL);
    wattrset(qso, A_NORMAL);
    wattrset(call, A_NORMAL);

    scrollok(qso, true);
    idlok(qso, true);

    scrollok(logw1, true);
    idlok(logw1, true);

    refresh();

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

int n_printf(bool ncwin, char *prn) {
    if (ncwin == true) {
        wprintw(logw, prn);
        wrefresh(logw);
        getch();

    } else
        fprintf(stdout, "%s", prn);

    return (0);
}

/* KBD Handler Thread */
void *KBDHandler(void *vargp) {
    while (true) {
        /* CQlock */

        char key = wgetch(call);

        pthread_mutex_lock(&KBDlock);  // Protect decodes structure

        kbd_queue.push_back(key);

        pthread_mutex_unlock(&KBDlock);  // Protect decodes structure
        usleep(10000); // Wait 10msec
    }
}

/* CQ Handler Thread */
void *CQHandler(void *vargp) {

    static bool refresh = false;

    mvwprintw(logw, 1, 2, "Time    SNR   Freq       Msg       Caller   Loc\n");
    wrefresh(logw);

    while (true) {
        char key;

        if (cq_queue.size()){
            pthread_mutex_lock(&CQlock);
            struct decoder_results dr = cq_queue.front();
            cq_queue.erase(cq_queue.begin());
            pthread_mutex_unlock(&CQlock);


            refresh = true;
        }
        if (kbd_queue.size()){
            key = kbd_queue.front();
            kbd_queue.erase(kbd_queue.begin());

            refresh = true;
        }
        /* if needed update the screen */
        if (refresh) {

        }

        refresh = false;
        usleep(10000); /* Wait 10 msec.*/
    }
}
