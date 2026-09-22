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

/* Global configuration parameters (from command line) */
static int Loss_rate;
static int Mode;
static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

#define ACK_TIMEOUT_SEC 1
#define MAX_RETRIES 5

int main(int argc, char *argv[]) {
    struct addrinfo hints, *servinfo, *servaddr;
    int             sock;
    int             ret;
    ncp_msg         setup_msg;
    ncp_msg         reply_msg;
    FILE           *fin;
    ncp_msg         data_msg;
    size_t          nread;
    int             seq;
    long            total_bytes_sent;

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

    ret = Send_and_wait_ack(sock, &setup_msg, servaddr->ai_addr,
                            servaddr->ai_addrlen, &reply_msg);

    if (ret == 0) {
        fprintf(stderr, "Giving up: no SETUP ACK after %d attempts\n", MAX_RETRIES);
        exit(1);
    }
    if (reply_msg.type == MSG_BUSY) {
        /* For now, treat "busy" the same as "give up". A smarter version
         * would back off and retry later instead of failing outright. */
        fprintf(stderr, "Receiver is busy. Try again later.\n");
        exit(1);
    }

    seq = 1;
    total_bytes_sent = 0;

    for (;;) {
        nread = fread(data_msg.payload, 1, MAX_PAYLOAD, fin);
 
        if (nread > 0) {
            int is_last = feof(fin); /* fread hit EOF exactly on this read */
 
            data_msg.type        = is_last ? MSG_FIN : MSG_DATA;
            data_msg.seq         = seq;
            data_msg.payload_len = (int)nread;
 
            ret = Send_and_wait_ack(sock, &data_msg, servaddr->ai_addr,
                                    servaddr->ai_addrlen, &reply_msg);
            if (ret == 0) {
                fprintf(stderr, "Giving up on seq=%d after %d attempts\n",
                        seq, MAX_RETRIES);
                exit(1);
            }

            total_bytes_sent += nread;
            printf("Sent %s packet: seq=%d payload_len=%d, ACKed (total sent: %ld bytes)\n",
                is_last ? "FIN" : "DATA", seq, data_msg.payload_len,
                total_bytes_sent);
 
            seq++;
 
            if (is_last) {
                break;
            }
        } else {
            if (feof(fin)) {
                data_msg.type        = MSG_FIN;
                data_msg.seq         = seq;
                data_msg.payload_len = 0;
 
                ret = sendto_dbg(sock, (char *)&data_msg, sizeof(data_msg), 0,
                                  servaddr->ai_addr, servaddr->ai_addrlen);
                if (ret < 0) {
                    perror("ncp: sendto_dbg (fin)");
                    exit(1);
                }
                printf("Sent empty FIN packet: seq=%d (total sent: %ld bytes)\n",
                       seq, total_bytes_sent);
            } else {
                perror("ncp: fread (source file)");
            }
            break;
        }
    }
 
    fclose(fin);
    freeaddrinfo(servinfo);
    close(sock);
 
    return 0;
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
        timeout.tv_usec = 0;
 
        ret = select(FD_SETSIZE, &mask, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("ncp: select");
            return 0;
        } else if (ret == 0) {
            /* Timed out -- loop around and resend the same packet. */
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
                return 1; /* caller decides what to do with BUSY */
            }
            if (reply_out->type == MSG_ACK && reply_out->seq == msg->seq) {
                return 1; /* matching ACK -- success */
            }
            /* Anything else (stale ACK from an earlier retry, garbage,
             * etc.) -- ignore it and keep waiting/retrying. */
            printf("  (got type=%d seq=%d while waiting for ack of seq=%d, ignoring)\n",
                   reply_out->type, reply_out->seq, msg->seq);
        }
    }
 
    return 0; /* exhausted retries with no matching reply */
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
