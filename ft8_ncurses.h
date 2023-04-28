/* Include file for QSO mode */
#include <curses.h>
#include <ncurses.h>

int init_ncurses();
int close_ncurses();
int n_printf (bool ncwin, char *prn);
int exit_ft8(bool qsomode,int status);

extern WINDOW *logw;
extern WINDOW *logwR;
extern WINDOW *logwL;
extern WINDOW *logwLH;

extern WINDOW *qso;

extern WINDOW *call;

/* CQ Handler Thread */
void *CQHandler(void *vargp);
void *KBDHandler(void *vargp);
