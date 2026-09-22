#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sendto_dbg.h"
#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;

int main(int argc, char *argv[]) {
    struct addrinfo         hints, *servinfo, *servaddr;
    struct sockaddr_storage from_addr;
    socklen_t               from_len;
    int                     sock;
    fd_set                  mask, read_mask;
    int                     bytes, num, ret;
    ncp_msg                 recvd_msg;
    ncp_msg                 ack_msg;
    char                    hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

    //currently in progress on this receiver
    FILE *fout = NULL;

    /* Initialize */
    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
    } else { /*(Mode == WAN)*/
        printf("\tMode = WAN\n");
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE;
    ret = getaddrinfo(NULL, Port_Str, &hints, &servinfo);

    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (sock < 0) { 
            perror("rcv: socket"); 
            continue; 
        }
        if (bind(sock, servaddr->ai_addr, servaddr->ai_addrlen) < 0) { 
            perror("rcv: bind");
            close(sock);
            continue; 
        }

        break;
    }
    freeaddrinfo(servinfo);

    FD_ZERO(&read_mask);
    FD_SET(sock, &read_mask);
 
    for (;;) {
        mask = read_mask;
 
        num = select(FD_SETSIZE, &mask, NULL, NULL, NULL);
        if (num <= 0) {
            continue;
        }
 
        if (!FD_ISSET(sock, &mask)) {
            continue;
        }

        from_len = sizeof(from_addr);
        bytes = recvfrom(sock, &recvd_msg, sizeof(recvd_msg), 0,
            (struct sockaddr *)&from_addr, &from_len);

        if (bytes < 0) {
            perror("rcv: recvfrom");
            continue;
        }

        ret = getnameinfo((struct sockaddr *)&from_addr, from_len, hbuf,
            sizeof(hbuf), sbuf, sizeof(sbuf),
            NI_NUMERICHOST | NI_NUMERICSERV);

        switch (recvd_msg.type) {
 
            case MSG_SETUP:
                /* recvd_msg.payload holds the destination filename,
                 * null-terminated, with recvd_msg.payload_len bytes
                 * (including the null terminator). */
 
                if (fout == NULL) {
                    /* No transfer in progress yet -- this is a fresh SETUP.
                     * Open (or create/truncate) the destination file for
                     * writing. */
                    fout = fopen(recvd_msg.payload, "wb");
                    if (fout == NULL) {
                        perror("rcv: fopen (destination file)");
                        /* Don't ACK -- sender will retry, and we'll try
                         * again on the next SETUP if the problem was
                         * transient. If it's a permissions issue this will
                         * just keep failing, which is acceptable for now. */
                        continue;
                    }
                    printf("Received SETUP from %s:%s -- saving to '%s'\n",
                           hbuf, sbuf, recvd_msg.payload);
                } else {
                    /* We already opened the file. This SETUP is either a
                     * duplicate (our earlier ACK was lost) or a second
                     * sender trying to start a new transfer while we're
                     * busy. For now we just re-ACK; distinguishing these
                     * two cases (and sending MSG_BUSY for the second one)
                     * is a later step. */
                    printf("Received duplicate/second SETUP from %s:%s\n",
                           hbuf, sbuf);
                }
 
                /* ACK the SETUP so the sender can stop retrying it and
                 * move on to sending data. */
                ack_msg.type        = MSG_ACK;
                ack_msg.seq         = 0;
                ack_msg.payload_len = 0;
                ret = sendto_dbg(sock, (char *)&ack_msg, sizeof(ack_msg), 0,
                                  (struct sockaddr *)&from_addr, from_len);
                if (ret < 0) {
                    perror("rcv: sendto_dbg (setup ack)");
                }
                break;
 
            case MSG_DATA:
                /* File-writing and windowing logic goes here next. For now
                 * just report what arrived, same as before. */
                printf("Received DATA from %s:%s -> seq=%d payload_len=%d\n",
                       hbuf, sbuf, recvd_msg.seq, recvd_msg.payload_len);
                break;
 
            default:
                printf("Received unexpected type=%d from %s:%s\n",
                       recvd_msg.type, hbuf, sbuf);
                break;
        }
    }

    if (fout != NULL) {
        fclose(fout);
    }
    close(sock);
    return 0;
    //if (servaddr == NULL) { fprintf(stderr, "No valid address found...exiting\n"); exit(1); }
}

/* Read commandline arguments */
static void Usage(int argc, char *argv[]) {
    if (argc != 4) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    Port_Str = argv[2];

    if (!strncmp(argv[3], "WAN", 4)) {
        Mode = MODE_WAN;
    } else if (!strncmp(argv[3], "LAN", 4)) {
        Mode = MODE_LAN;
    } else {
        Print_help();
    }
}

static void Print_help(void) {
    printf("Usage: rcv <loss_rate_percent> <port> <env>\n");
    exit(0);
}
