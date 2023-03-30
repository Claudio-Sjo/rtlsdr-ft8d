#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <ncurses.h>

WINDOW *sub;

WINDOW *logw;

WINDOW *qso;

WINDOW *call;

int slines,scols,sposy,sposx;

int init_ncurses() {
    initscr();

    /* create subwindow on stdscr */
    sub = subwin(stdscr, LINES - 2, COLS - 2, 1, 1);

    logw = subwin(sub, LINES/2, COLS - 4, 2, 2);

    qso = subwin(sub, (LINES/2) -7 , COLS - 4, LINES/2 + 2, 2);

    call = subwin(sub, 3, COLS - 4, LINES-5, 2);

    start_color();
    init_pair(1,COLOR_YELLOW,COLOR_BLACK);

    attrset(COLOR_PAIR(1) | A_BOLD);
    box(stdscr, 0, 0);

    box(sub, 0, 0);

    box(logw, 0, 0);

    box(qso, 0, 0);

    box(call, 0, 0);

    attrset(A_NORMAL);

    waddstr(sub, "rtlsdr FT8 - QSO Mode\n");

    refresh();
    getch();

    return (0);

}

int close_ncurses() {
    refresh();
    getch();

    endwin();
    return (0);
}

int n_printf (bool ncwin, char *prn)
{

    if (ncwin == true)
    {
        waddstr(sub, prn);
        printf(prn);
        refresh();
            getch();

    }
    else
    fprintf(stdout, "%s", prn);

    return (0);

}