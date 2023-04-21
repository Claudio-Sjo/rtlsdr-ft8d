#include <stdbool.h>

#define MIN(i, j) (((i) < (j)) ? (i) : (j))
#define MAX(i, j) (((i) > (j)) ? (i) : (j))

#define MSGTYP(msg)	(msg.type)

#define MAXMSGSIZE  40  // Max 40 bytes

#define SOCKNAME "/tmp/ft8S"

typedef enum  
{
	SEND_F8_REQ 			= 1,
	CHANGE_RTX_STATE 		= 2,
	TEST_SEND				= 3,
	SEND_ACK				= 4,
	REJECTED				= 5
} SignalType;


typedef struct FT8Msg_t 
{
	SignalType	type;
	char		ft8Message[MAXMSGSIZE];
    bool   RTXstate;
} FT8Msg;
