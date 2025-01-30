#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <math.h>
#include <stdbool.h>
#include <string.h>
#include <getopt.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <rtl-sdr.h>
#include <fftw3.h>
#include <sys/socket.h>
#include <netdb.h>
#include <curl/curl.h>

#include <rtlsdr_ft8d.h>

#include <ft8/constants.h>
#include <ft8/ldpc.h>
#include <ft8/crc.h>
#include <ft8/decode.h>
#include <ft8/encode.h>
#include <common/wave.h>
#include <common/monitor.h>
#include <common/audio.h>

#include <pskreporter.hpp>
#include <ft8_ncurses.h>

#include <rtlsdr_ft8d.h>
#include <ft8tx/FT8Types.h>

typedef enum _qsostate_t { idle,
                           replyLoc,
                           replySig,
                           replyRR73,
                           reply73,
                           cqIng } qsostate_t;

typedef enum _peermsg_t { cqMsg,
                          locMsg,
                          sigMsg,
                          RR73Msg,
                          s73Msg } peermsg_t;

void initQsoState(void);
bool queryCQ(void);
bool updateQsoMachine(ft8slot_t theSlot);
bool addQso(struct plain_message *newQso);
bool addCQ(struct plain_message *newQso);

void enableAutoCQ(void);
void disableAutoCQ(void);
bool getAutoCQStatus(void);
void enableAutoCQReply(void);
void disableAutoCQReply(void);
bool getAutoCQReplyStatus(void);
void enableAutoQSO(void);
void disableAutoQSO(void);
bool getAutoQSOStatus(void);
void setActiveSlot(ft8slot_t value);
ft8slot_t getActiveSlot(void);