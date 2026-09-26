#include <sys/types.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> 
#include <netdb.h>

#define MAX_MESS_LEN 1400
#define MAX_PAYLOAD 1000
#define MAX_FILENAME 256

#define MODE_LAN 1
#define MODE_WAN 2

enum {
    MSG_DATA = 1,
    MSG_ACK = 2,
    MSG_NACK = 3,
    MSG_SETUP = 4,
    MSG_BUSY = 5,
    USER_MSG_FIN = 6
};

typedef struct dummy_ncp_msg {
    /* Fill in header information needed for your protocol */
    char payload[MAX_PAYLOAD];
    int32_t type;
    int32_t seq;
    int32_t payload_len;
} ncp_msg;

/* Uncomment and fill in fields for your protocol */
/*
typedef struct dummy_rcv_msg {
} rcv_msg;
*/
