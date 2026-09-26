#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "sendto_dbg.h"
#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static int  Send_and_wait_ack(int sock, ncp_msg *msg,
                struct sockaddr *addr, socklen_t addrlen,
                ncp_msg *reply_out);
static long   Elapsed_ms(struct timeval *since);
static double Elapsed_sec(struct timeval *since);

static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

static int ACK_TIMEOUT_SEC;
static int ACK_TIMEOUT_USEC;
#define MAX_RETRIES 5
static int WINDOW_SIZE;
#define POLL_INTERVAL_MS 100
#define DATA_RTO_MS 500
#define MB 1000000.0

typedef struct {
    int             in_use;
    ncp_msg         pkt;
    struct timeval  sent_time;
} send_slot_t;

int main(int argc, char *argv[]) {
    Usage(argc, argv);
    
    struct addrinfo hints, *servinfo, *servaddr;
    int             sock;
    int             ret;
    ncp_msg         setup_msg;
    ncp_msg         reply_msg;
    FILE           *fin;

    static send_slot_t window[WINDOW_SIZE_MAX];
    int             window_size;
    int             base_seq;
    int             next_seq;
    int             fin_seq;
    int             file_eof;
    long            total_bytes_acked;
    long            total_pkts_sent_incl_retrans;
    long            total_bytes_sent_incl_retrans;

    struct timeval  xfer_start_time;
    struct timeval  last_report_time;
    long            last_report_bytes;

    int             i;

    sendto_dbg_init(Loss_rate);
    printf("Successfully initialized with:\n");
    printf("\tLoss rate = %d\n", Loss_rate);
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);
    if (Mode == MODE_LAN) {
        printf("\tMode = LAN\n");
        window_size = WINDOW_SIZE_LAN;
    } else {
        printf("\tMode = WAN\n");
        window_size = WINDOW_SIZE_WAN;
    }

    fin = fopen(Src_filename, "rb");
    if (fin == NULL) {
        perror("ncp: fopen (source file)");
        exit(1);
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
        break;
    }
    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }

    setup_msg.type        = MSG_SETUP;
    setup_msg.seq         = 0;
    setup_msg.payload_len = (int)strlen(Dst_filename) + 1;
    memcpy(setup_msg.payload, Dst_filename, setup_msg.payload_len);

    printf("Sending SETUP...\n");
    ret = Send_and_wait_ack(sock, &setup_msg, servaddr->ai_addr,
                             servaddr->ai_addrlen, &reply_msg);
    if (ret == 0) {
        fprintf(stderr, "Giving up: no SETUP ACK after %d attempts\n", MAX_RETRIES);
        exit(1);
    }
    if (reply_msg.type == MSG_BUSY) {
        fprintf(stderr, "Receiver is busy. Try again later.\n");
        exit(1);
    }
    printf("SETUP ACKed. Starting windowed data transfer (window size %d)...\n",
           window_size);

    gettimeofday(&xfer_start_time, NULL);
    last_report_time  = xfer_start_time;
    last_report_bytes = 0;

    for (i = 0; i < window_size; i++) {
        window[i].in_use = 0;
    }
    base_seq                      = 1;
    next_seq                      = 1;
    fin_seq                       = -1;
    file_eof                      = 0;
    total_bytes_acked             = 0;
    total_pkts_sent_incl_retrans   = 0;
    total_bytes_sent_incl_retrans  = 0;

    while (!file_eof || base_seq <= fin_seq) {

        while (!file_eof && next_seq < base_seq + window_size) {
            ncp_msg  *pkt = &window[next_seq % window_size].pkt;
            size_t    nread;

            nread = fread(pkt->payload, 1, MAX_PAYLOAD, fin);

            if (nread > 0) {
                int is_last = feof(fin);

                pkt->type        = is_last ? USER_MSG_FIN : MSG_DATA;
                pkt->seq         = next_seq;
                pkt->payload_len = (int)nread;

                ret = sendto_dbg(sock, (char *)pkt, sizeof(*pkt), 0,
                                  servaddr->ai_addr, servaddr->ai_addrlen);
                if (ret < 0) {
                    perror("ncp: sendto_dbg (data)");
                    exit(1);
                }
                total_pkts_sent_incl_retrans++;
                total_bytes_sent_incl_retrans += sizeof(*pkt);

                window[next_seq % window_size].in_use = 1;
                gettimeofday(&window[next_seq % window_size].sent_time, NULL);

                printf("Sent %s seq=%d payload_len=%d (window has %d..%d in flight)\n",
                       is_last ? "FIN" : "DATA", next_seq, pkt->payload_len,
                       base_seq, next_seq);

                if (is_last) {
                    fin_seq   = next_seq;
                    file_eof  = 1;
                }
                next_seq++;

            } else if (feof(fin)) {
                pkt->type        = USER_MSG_FIN;
                pkt->seq         = next_seq;
                pkt->payload_len = 0;

                ret = sendto_dbg(sock, (char *)pkt, sizeof(*pkt), 0,
                                  servaddr->ai_addr, servaddr->ai_addrlen);
                if (ret < 0) {
                    perror("ncp: sendto_dbg (fin)");
                    exit(1);
                }
                total_pkts_sent_incl_retrans++;
                total_bytes_sent_incl_retrans += sizeof(*pkt);

                window[next_seq % window_size].in_use = 1;
                gettimeofday(&window[next_seq % window_size].sent_time, NULL);

                printf("Sent empty FIN seq=%d\n", next_seq);

                fin_seq  = next_seq;
                file_eof = 1;
                next_seq++;
            } else {
                perror("ncp: fread (source file)");
                file_eof = 1;
            }
        }

        {
            fd_set         mask, read_mask;
            struct timeval timeout;

            FD_ZERO(&read_mask);
            FD_SET(sock, &read_mask);
            mask = read_mask;

            timeout.tv_sec  = 0;
            timeout.tv_usec = POLL_INTERVAL_MS * 1000;

            ret = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
            if (ret < 0) {
                perror("ncp: select");
                exit(1);
            }

            if (ret > 0 && FD_ISSET(sock, &mask)) {
                for (;;) {
                    ncp_msg reply;
                    ret = recvfrom(sock, &reply, sizeof(reply), MSG_DONTWAIT,
                                    NULL, NULL);
                    if (ret < 0) {
                        break;
                    }

                    if (reply.type == MSG_ACK) {
                        if (reply.seq >= base_seq) {
                            int s;
                            for (s = base_seq; s <= reply.seq; s++) {
                                int idx = s % window_size;
                                if (window[idx].in_use && window[idx].pkt.seq == s) {
                                    total_bytes_acked += window[idx].pkt.payload_len;
                                    window[idx].in_use = 0;
                                }
                            }
                            base_seq = reply.seq + 1;
                            printf("  ACK seq=%d -- window base now %d\n",
                                   reply.seq, base_seq);

                            if (total_bytes_acked - last_report_bytes >= (long)(10 * MB)) {
                                double elapsed = Elapsed_sec(&last_report_time);
                                double rate_mbps = elapsed > 0
                                    ? ((total_bytes_acked - last_report_bytes) * 8.0 / MB) / elapsed
                                    : 0.0;
                                printf("[ncp] %.2f MB transferred so far, "
                                       "last interval rate %.2f Mbps\n",
                                       total_bytes_acked / MB, rate_mbps);
                                last_report_bytes = total_bytes_acked;
                                gettimeofday(&last_report_time, NULL);
                            }
                        }

                    } else if (reply.type == MSG_NACK) {
                        int idx = reply.seq % window_size;
                        if (window[idx].in_use && window[idx].pkt.seq == reply.seq) {
                            ret = sendto_dbg(sock, (char *)&window[idx].pkt,
                                              sizeof(window[idx].pkt), 0,
                                              servaddr->ai_addr, servaddr->ai_addrlen);
                            if (ret < 0) {
                                perror("ncp: sendto_dbg (nack resend)");
                            } else {
                                total_pkts_sent_incl_retrans++;
                                total_bytes_sent_incl_retrans += sizeof(window[idx].pkt);
                                gettimeofday(&window[idx].sent_time, NULL);
                                printf("  NACK seq=%d -- resent\n", reply.seq);
                            }
                        }
                    }
                }
            }
        }

        for (i = 0; i < window_size; i++) {
            if (window[i].in_use && Elapsed_ms(&window[i].sent_time) >= DATA_RTO_MS) {
                ret = sendto_dbg(sock, (char *)&window[i].pkt,
                                  sizeof(window[i].pkt), 0,
                                  servaddr->ai_addr, servaddr->ai_addrlen);
                if (ret < 0) {
                    perror("ncp: sendto_dbg (timeout resend)");
                } else {
                    total_pkts_sent_incl_retrans++;
                    total_bytes_sent_incl_retrans += sizeof(window[i].pkt);
                    gettimeofday(&window[i].sent_time, NULL);
                    printf("  (timeout) resent seq=%d\n", window[i].pkt.seq);
                }
            }
        }
    }

    {
        double total_time    = Elapsed_sec(&xfer_start_time);
        double file_size_mb  = total_bytes_acked / MB;
        double avg_rate_mbps = total_time > 0
            ? (total_bytes_acked * 8.0 / MB) / total_time
            : 0.0;
        double sent_incl_retrans_mb = total_bytes_sent_incl_retrans / MB;

        printf("\n[ncp] Transfer complete.\n");
        printf("[ncp] File size: %.4f MB\n", file_size_mb);
        printf("[ncp] Transfer time: %.4f sec\n", total_time);
        printf("[ncp] Average rate: %.4f Mbps\n", avg_rate_mbps);
        printf("[ncp] Total data sent including retransmissions: %.4f MB\n",
               sent_incl_retrans_mb);
    }

    fclose(fin);
    freeaddrinfo(servinfo);
    close(sock);

    return 0;
}

static long Elapsed_ms(struct timeval *since) {
    struct timeval now;
    long           sec_diff, usec_diff;

    gettimeofday(&now, NULL);
    sec_diff  = now.tv_sec  - since->tv_sec;
    usec_diff = now.tv_usec - since->tv_usec;

    return (sec_diff * 1000) + (usec_diff / 1000);
}

static double Elapsed_sec(struct timeval *since) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - since->tv_sec) +
           (now.tv_usec - since->tv_usec) / 1000000.0;
}

static int Send_and_wait_ack(int sock, ncp_msg *msg,
                              struct sockaddr *addr, socklen_t addrlen,
                              ncp_msg *reply_out) {
    fd_set         mask, read_mask;
    struct timeval timeout;
    int            attempt;
    int            ret;

    FD_ZERO(&read_mask);
    FD_SET(sock, &read_mask);

    for (attempt = 1; attempt <= MAX_RETRIES; attempt++) {

        ret = sendto_dbg(sock, (char *)msg, sizeof(*msg), 0, addr, addrlen);
        if (ret < 0) {
            perror("ncp: sendto_dbg");
            return 0;
        }

        mask = read_mask;
        timeout.tv_sec  = ACK_TIMEOUT_SEC;
        timeout.tv_usec = ACK_TIMEOUT_USEC;

        ret = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("ncp: select");
            return 0;
        } else if (ret == 0) {
            printf("  (timeout on seq=%d attempt %d/%d, resending)\n",
                   msg->seq, attempt, MAX_RETRIES);
            continue;
        }

        if (FD_ISSET(sock, &mask)) {
            ret = recvfrom(sock, reply_out, sizeof(*reply_out), 0, NULL, NULL);
            if (ret < 0) {
                perror("ncp: recvfrom");
                continue;
            }

            if (reply_out->type == MSG_BUSY) {
                return 1;
            }
            if (reply_out->type == MSG_ACK && reply_out->seq == msg->seq) {
                return 1;
            }
            printf("  (got type=%d seq=%d while waiting for ack of seq=%d, ignoring)\n",
                   reply_out->type, reply_out->seq, msg->seq);
        }
    }

    return 0;
}

static void Usage(int argc, char *argv[]) {

    if (argc != 5) {
        Print_help();
    }

    if (sscanf(argv[1], "%d", &Loss_rate) != 1) {
        Print_help();
    }

    if (!strncmp(argv[2], "WAN", 4)) {
        Mode = MODE_WAN;
        ACK_TIMEOUT_SEC = 0;
        ACK_TIMEOUT_USEC = 100000; // 100 ms timeout
        WINDOW_SIZE = 4000;
    } else if (!strncmp(argv[2], "LAN", 4)) {
        Mode = MODE_LAN;
        ACK_TIMEOUT_SEC = 0;
        ACK_TIMEOUT_USEC = 200; // 0.2 ms timeout
        WINDOW_SIZE = 8;
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