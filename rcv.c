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
    long                    total_bytes_written;

    //currently in progress on this receiver
    FILE *fout = NULL;
    int expected_seq = 1;

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
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
        exit(1);
    }

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

    total_bytes_written = 0;

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
                if (fout == NULL) {
                    fout = fopen(recvd_msg.payload, "wb");
                    if (fout == NULL) {
                        perror("rcv: fopen (destination file)");
                        continue;
                    }
                    total_bytes_written = 0;
                    expected_seq = 1;
                    printf("Received SETUP from %s:%s -- saving to '%s'\n",
                           hbuf, sbuf, recvd_msg.payload);
                } else {
                    printf("Received duplicate/second SETUP from %s:%s\n",
                           hbuf, sbuf);
                }

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
            case MSG_FIN:
                if (fout == NULL) {
                    printf("Received %s from %s:%s but no transfer is in "
                           "progress (no prior SETUP) -- dropping\n",
                           recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                           hbuf, sbuf);
                    break;
                }
 
                if (recvd_msg.seq == expected_seq) {
                    if (recvd_msg.payload_len > 0) {
                        size_t nwritten = fwrite(recvd_msg.payload, 1,
                                                  recvd_msg.payload_len, fout);
                        if (nwritten < (size_t)recvd_msg.payload_len) {
                            perror("rcv: fwrite");
                        } else {
                            total_bytes_written += nwritten;
                        }
                    }
 
                    printf("Received %s from %s:%s -> seq=%d payload_len=%d "
                           "(total written: %ld bytes)\n",
                           recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                           hbuf, sbuf, recvd_msg.seq, recvd_msg.payload_len,
                           total_bytes_written);
 
                    expected_seq++;
 
                } else if (recvd_msg.seq < expected_seq) {
                    printf("Received duplicate %s seq=%d from %s:%s "
                           "(already have it, re-ACKing)\n",
                           recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                           recvd_msg.seq, hbuf, sbuf);
                } else {
                    printf("Received out-of-order %s seq=%d from %s:%s "
                           "(expected %d) -- dropping\n",
                           recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                           recvd_msg.seq, hbuf, sbuf, expected_seq);
                    break; /* don't ACK -- we didn't accept this one */
                }

                                ack_msg.type        = MSG_ACK;
                ack_msg.seq         = recvd_msg.seq;
                ack_msg.payload_len = 0;
                ret = sendto_dbg(sock, (char *)&ack_msg, sizeof(ack_msg), 0,
                                  (struct sockaddr *)&from_addr, from_len);
                if (ret < 0) {
                    perror("rcv: sendto_dbg (data ack)");
                }
 
                if (recvd_msg.type == MSG_FIN && recvd_msg.seq == expected_seq - 1) {
                    /* We just accepted the FIN (not a duplicate re-ACK of
                     * an old FIN) -- close out the transfer. */
                    fclose(fout);
                    fout = NULL;
                    printf("Transfer complete. Wrote %ld bytes.\n\n",
                           total_bytes_written);
                }
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
