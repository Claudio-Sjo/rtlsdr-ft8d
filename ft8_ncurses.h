/* Include file for QSO mode */
#include <curses.h>
#include <ncurses.h>

int init_ncurses();
int close_ncurses();
int n_printf (bool ncwin, char *prn);
int exit_ft8(bool qsomode,int status);

extern WINDOW *logw;

extern WINDOW *qso;

extern WINDOW *call;