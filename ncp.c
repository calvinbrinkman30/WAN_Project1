#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "sendto_dbg.h"
#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

#define SETUP_TIMEOUT_SEC 1
#define SETUP_MAX_RETRIES 5

int main(int argc, char *argv[]) {
    struct addrinfo hints, *servinfo, *servaddr;
    int             sock;
    int             ret;
    ncp_msg         setup_msg;
    ncp_msg         recvd_msg;
    fd_set          mask, read_mask;
    struct timeval  timeout;
    int             attempt;
    int             setup_acked;

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    ret = getaddrinfo(Hostname, Port_Str, &hints, &servinfo);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
        exit(1);
    }

    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (sock < 0) {
            perror("ncp: socket");
            continue;
        }
        break; /* got a valid socket */
    }
    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }
    /* Build one hardcoded DATA packet, just to test that the wire format
     * round-trips correctly. No window, no file reading yet. */
    setup_msg.type        = MSG_SETUP;
    setup_msg.seq         = 0;
    setup_msg.payload_len = (int)strlen(Dst_filename) + 1; /* include '\0' */
    memcpy(setup_msg.payload, Dst_filename, setup_msg.payload_len);

    FD_ZERO(&read_mask);
    FD_SET(sock, &read_mask);

    setup_acked = 0;
    for (attempt = 1; attempt <= SETUP_MAX_RETRIES && !setup_acked; attempt++) {
 
        ret = sendto_dbg(sock, (char *)&setup_msg, sizeof(setup_msg), 0,
                          servaddr->ai_addr, servaddr->ai_addrlen);
        if (ret < 0) {
            perror("ncp: sendto_dbg (setup)");
            exit(1);
        }
        printf("Sent SETUP (attempt %d/%d), waiting for ACK...\n",
               attempt, SETUP_MAX_RETRIES);
 
        mask    = read_mask;
        timeout.tv_sec  = SETUP_TIMEOUT_SEC;
        timeout.tv_usec = 0;
 
        ret = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("ncp: select");
            exit(1);
        } else if (ret == 0) {
            /* Timed out waiting for a response -- loop around and resend. */
            printf("Timed out waiting for SETUP ACK, retrying...\n");
            continue;
        }
 
        if (FD_ISSET(sock, &mask)) {
            ret = recvfrom(sock, &recvd_msg, sizeof(recvd_msg), 0, NULL, NULL);
            if (ret < 0) {
                perror("ncp: recvfrom");
                continue;
            }
 
            if (recvd_msg.type == MSG_ACK) {
                printf("Received SETUP ACK. Ready to send data.\n");
                setup_acked = 1;
            } else if (recvd_msg.type == MSG_BUSY) {
                printf("Receiver is busy with another transfer. Retrying...\n");
            } else {
                printf("Received unexpected type=%d while waiting for SETUP ACK\n",
                       recvd_msg.type);
            }
        }
    }

    if (!setup_acked) {
        fprintf(stderr, "Giving up: no SETUP ACK after %d attempts\n",
                SETUP_MAX_RETRIES);
        exit(1);
    }
 
    {
        ncp_msg data_msg;
        data_msg.type        = MSG_DATA;
        data_msg.seq         = 1;
        data_msg.payload_len = 5;
        memcpy(data_msg.payload, "hello", 5);
 
        ret = sendto_dbg(sock, (char *)&data_msg, sizeof(data_msg), 0,
                          servaddr->ai_addr, servaddr->ai_addrlen);
        if (ret < 0) {
            perror("ncp: sendto_dbg (data)");
            exit(1);
        }
        printf("Sent test DATA packet: seq=%d payload_len=%d\n",
               data_msg.seq, data_msg.payload_len);
    }
 
    freeaddrinfo(servinfo);
    close(sock);
 
    return 0;
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {

    if (argc != 5) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    if (!strncmp(argv[2], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[2], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }

    Src_filename = argv[3];
    Dst_filename = strtok(argv[4], "@");
    Hostname = strtok(NULL, ":");
    if (Hostname == NULL) {
        printf("Error: no hostname provided\n");
        Print_help();
    }
    Port_Str = strtok(NULL, ":");
    if (Port_Str == NULL) {
        printf("Error: no port provided\n");
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: ncp <loss_rate_percent> <env> <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}
