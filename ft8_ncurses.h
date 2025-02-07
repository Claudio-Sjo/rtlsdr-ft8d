/* Include file for QSO mode */
#include <curses.h>
#include <ncurses.h>

int init_ncurses(uint32_t);
int close_ncurses();
int n_printf (bool ncwin, char *prn);
int exit_ft8(bool qsomode,int status);

extern WINDOW *statusW;
extern WINDOW *statusW0;
extern WINDOW *trafficW;
extern WINDOW *trafficWH;

extern WINDOW *qso;

extern WINDOW *call;

/* CQ Handler Thread */
void *CQHandler(void *vargp);
void *KBDHandler(void *vargp);
void *TXHandler(void *vargp);
void printHeaders(void);
void displayTxString(char *txMsg);

void close_TxThread(void);
void close_KbhThread(void);
