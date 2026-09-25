#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sendto_dbg.h"
#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static double Elapsed_sec(struct timeval *since);

static int Loss_rate;
static int Mode;
static char *Port_Str;

#define WINDOW_SIZE_LAN 4
#define WINDOW_SIZE_WAN 500
#define WINDOW_SIZE_MAX 500
#define MB 1000000.0

typedef struct {
    int      received;
    ncp_msg  pkt;
} recv_slot_t;

static void Send_msg(int sock, ncp_msg *msg, struct sockaddr *addr, socklen_t addrlen,
                      const char *what);

int main(int argc, char *argv[]) {
    struct addrinfo         hints, *servinfo, *servaddr;
    struct sockaddr_storage from_addr;
    socklen_t               from_len;
    int                     sock;
    fd_set                  mask, read_mask;
    int                     bytes, num, ret;
    ncp_msg                 recvd_msg;
    ncp_msg                 ack_msg, nack_msg;
    char                    hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    long                    total_bytes_written;

    FILE        *fout = NULL;
    int          expected_seq = 1;
    static recv_slot_t window[WINDOW_SIZE_MAX];
    int          window_size;
    int          i;
    char         active_hbuf[NI_MAXHOST] = "";
    char         active_sbuf[NI_MAXSERV] = "";

    struct timeval xfer_start_time;
    struct timeval last_report_time;
    long            last_report_bytes;

    Usage(argc, argv);
    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
        window_size = WINDOW_SIZE_LAN;
    } else {
        printf("\tMode = WAN\n");
        window_size = WINDOW_SIZE_WAN;
    }

    for (i = 0; i < window_size; i++) {
        window[i].received = 0;
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
    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }
    freeaddrinfo(servinfo);

    printf("Listening on port %s (window size %d)...\n\n", Port_Str, window_size);

    total_bytes_written = 0;
    last_report_bytes   = 0;

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
        if (ret != 0) {
            fprintf(stderr, "getnameinfo error: %s\n", gai_strerror(ret));
            continue;
        }

        switch (recvd_msg.type) {

            case MSG_SETUP:
                if (fout == NULL) {
                    fout = fopen(recvd_msg.payload, "wb");
                    if (fout == NULL) {
                        perror("rcv: fopen (destination file)");
                        continue;
                    }
                    total_bytes_written = 0;
                    last_report_bytes   = 0;
                    expected_seq = 1;
                    for (i = 0; i < window_size; i++) {
                        window[i].received = 0;
                    }
                    gettimeofday(&xfer_start_time, NULL);
                    last_report_time = xfer_start_time;
                    strncpy(active_hbuf, hbuf, sizeof(active_hbuf) - 1);
                    active_hbuf[sizeof(active_hbuf) - 1] = '\0';
                    strncpy(active_sbuf, sbuf, sizeof(active_sbuf) - 1);
                    active_sbuf[sizeof(active_sbuf) - 1] = '\0';
                    printf("Received SETUP from %s:%s -- saving to '%s'\n",
                           hbuf, sbuf, recvd_msg.payload);

                    ack_msg.type        = MSG_ACK;
                    ack_msg.seq         = 0;
                    ack_msg.payload_len = 0;
                    Send_msg(sock, &ack_msg, (struct sockaddr *)&from_addr, from_len,
                             "setup ack");

                } else if (strcmp(hbuf, active_hbuf) == 0 &&
                           strcmp(sbuf, active_sbuf) == 0) {
                    printf("Received duplicate SETUP from %s:%s (same sender)\n",
                           hbuf, sbuf);

                    ack_msg.type        = MSG_ACK;
                    ack_msg.seq         = 0;
                    ack_msg.payload_len = 0;
                    Send_msg(sock, &ack_msg, (struct sockaddr *)&from_addr, from_len,
                             "setup ack");

                } else {
                    printf("Received SETUP from %s:%s but busy with %s:%s "
                           "-- sending BUSY\n",
                           hbuf, sbuf, active_hbuf, active_sbuf);

                    ack_msg.type        = MSG_BUSY;
                    ack_msg.seq         = 0;
                    ack_msg.payload_len = 0;
                    Send_msg(sock, &ack_msg, (struct sockaddr *)&from_addr, from_len,
                             "busy reply");
                }
                break;

            case MSG_DATA:
            case MSG_FIN:
                if (fout == NULL) {
                    if (recvd_msg.type == MSG_FIN &&
                        recvd_msg.seq == expected_seq - 1) {
                        printf("Received duplicate FIN seq=%d from %s:%s "
                               "for an already-completed transfer, re-ACKing\n",
                               recvd_msg.seq, hbuf, sbuf);
                        ack_msg.type        = MSG_ACK;
                        ack_msg.seq         = recvd_msg.seq;
                        ack_msg.payload_len = 0;
                        Send_msg(sock, &ack_msg, (struct sockaddr *)&from_addr,
                                 from_len, "late fin ack");
                    } else {
                        printf("Received %s seq=%d from %s:%s but no "
                               "transfer is in progress -- dropping\n",
                               recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                               recvd_msg.seq, hbuf, sbuf);
                    }
                    break;
                }

                if (recvd_msg.seq < expected_seq) {
                    printf("Received duplicate %s seq=%d from %s:%s "
                           "(already delivered, re-ACKing)\n",
                           recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                           recvd_msg.seq, hbuf, sbuf);

                } else if (recvd_msg.seq >= expected_seq + window_size) {
                    printf("Received %s seq=%d from %s:%s -- outside "
                           "receive window (expected %d..%d), dropping\n",
                           recvd_msg.type == MSG_FIN ? "FIN" : "DATA",
                           recvd_msg.seq, hbuf, sbuf, expected_seq,
                           expected_seq + window_size - 1);
                    break;

                } else {
                    int slot = recvd_msg.seq % window_size;

                    if (!window[slot].received) {
                        window[slot].received = 1;
                        window[slot].pkt       = recvd_msg;
                    }

                    while (window[expected_seq % window_size].received) {
                        int      idx = expected_seq % window_size;
                        ncp_msg *p   = &window[idx].pkt;

                        if (p->payload_len > 0) {
                            size_t nwritten = fwrite(p->payload, 1,
                                                      p->payload_len, fout);
                            if (nwritten < (size_t)p->payload_len) {
                                perror("rcv: fwrite");
                            } else {
                                total_bytes_written += nwritten;
                            }
                        }

                        printf("Delivered %s seq=%d payload_len=%d "
                               "(total written: %ld bytes)\n",
                               p->type == MSG_FIN ? "FIN" : "DATA",
                               p->seq, p->payload_len, total_bytes_written);

                        if (total_bytes_written - last_report_bytes >= (long)(10 * MB)) {
                            double elapsed = Elapsed_sec(&last_report_time);
                            double rate_mbps = elapsed > 0
                                ? ((total_bytes_written - last_report_bytes) * 8.0 / MB) / elapsed
                                : 0.0;
                            printf("[rcv] %.2f MB received so far, "
                                   "last interval rate %.2f Mbps\n",
                                   total_bytes_written / MB, rate_mbps);
                            last_report_bytes = total_bytes_written;
                            gettimeofday(&last_report_time, NULL);
                        }

                        if (p->type == MSG_FIN) {
                            double total_time    = Elapsed_sec(&xfer_start_time);
                            double file_size_mb  = total_bytes_written / MB;
                            double avg_rate_mbps = total_time > 0
                                ? (total_bytes_written * 8.0 / MB) / total_time
                                : 0.0;

                            fclose(fout);
                            fout = NULL;

                            printf("\n[rcv] Transfer complete.\n");
                            printf("[rcv] File size: %.4f MB\n", file_size_mb);
                            printf("[rcv] Transfer time: %.4f sec\n", total_time);
                            printf("[rcv] Average rate: %.4f Mbps\n\n", avg_rate_mbps);
                        }

                        window[idx].received = 0;
                        expected_seq++;

                        if (fout == NULL) {
                            break;
                        }
                    }

                    {
                        int k;
                        for (k = expected_seq; k < recvd_msg.seq; k++) {
                            if (!window[k % window_size].received) {
                                nack_msg.type        = MSG_NACK;
                                nack_msg.seq         = k;
                                nack_msg.payload_len = 0;
                                Send_msg(sock, &nack_msg,
                                         (struct sockaddr *)&from_addr, from_len,
                                         "nack");
                            }
                        }
                    }
                }

                ack_msg.type        = MSG_ACK;
                ack_msg.seq         = expected_seq - 1;
                ack_msg.payload_len = 0;
                Send_msg(sock, &ack_msg, (struct sockaddr *)&from_addr, from_len,
                         "cumulative ack");
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
}

static double Elapsed_sec(struct timeval *since) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - since->tv_sec) +
           (now.tv_usec - since->tv_usec) / 1000000.0;
}

static void Send_msg(int sock, ncp_msg *msg, struct sockaddr *addr, socklen_t addrlen,
                      const char *what) {
    int ret = sendto_dbg(sock, (char *)msg, sizeof(*msg), 0, addr, addrlen);
    if (ret < 0) {
        char errmsg[64];
        snprintf(errmsg, sizeof(errmsg), "rcv: sendto_dbg (%s)", what);
        perror(errmsg);
    }
}

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