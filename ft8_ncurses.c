#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <ncurses.h>
#include <curses.h>

WINDOW *root;

WINDOW *logw,*logw0;

WINDOW *qso,*qso0;

WINDOW *call,*call0;

int slines,scols,sposy,sposx;

int init_ncurses() {
    initscr();

    /* create subwindow on stdscr */

    root = subwin(stdscr, 1 , COLS - 4, 1, 2);

    logw0 = subwin(stdscr, LINES/2, COLS - 4, 2, 2);
    logw = subwin(stdscr, LINES/2, COLS - 6, 2, 3);

    qso0 = subwin(stdscr, (LINES/2) -7 , COLS - 4, LINES/2 + 2, 2);
    qso = subwin(stdscr, (LINES/2) -7 , COLS - 6, LINES/2 + 2, 3);

    call0 = subwin(stdscr, 3, COLS - 4, LINES-5, 2);
    call = subwin(stdscr, 3, COLS - 6, LINES-5, 3);

    start_color();
    init_pair(1,COLOR_YELLOW,COLOR_BLACK);
    init_pair(2,COLOR_RED,COLOR_BLACK);
    init_pair(3,COLOR_GREEN,COLOR_BLACK);
    init_pair(4,COLOR_CYAN,COLOR_BLACK);

    attrset(COLOR_PAIR(1) | A_BOLD);
    wattrset(logw0,COLOR_PAIR(1) | A_BOLD);
    wattrset(qso0,COLOR_PAIR(3) | A_BOLD);
    wattrset(call0,COLOR_PAIR(4) | A_BOLD);

    box(stdscr, 0, 0);

    box(logw0, 0, 0);

    box(qso0, 0, 0);

    box(call0, 0, 0);

    wattrset( root, COLOR_PAIR(2) | A_BOLD);

    mvwprintw( root, 0, COLS/2 -10 , "rtlsdr FT8 - QSO Mode\n");

    wattrset(logw,A_NORMAL);
    wattrset(qso,A_NORMAL);
    wattrset(call,A_NORMAL);

    refresh();

    return (0);

}

int close_ncurses() {
    refresh();
    wgetch(call);

    endwin();
    return (0);
}

int n_printf (bool ncwin, char *prn)
{

    if (ncwin == true)
    {
        wprintw(logw, prn);
        wrefresh(logw);
        getch();

    }
    else
    fprintf(stdout, "%s", prn);

    return (0);

}