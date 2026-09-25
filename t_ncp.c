#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "net_include.h"

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static int  Send_all(int sock, const char *buf, int len);
static double Elapsed_sec(struct timeval *since);

static char *Port_Str;
static char *Src_filename;
static char *Dst_filename;
static char *Hostname;

#define BUF_SIZE 8192
#define MB 1000000.0

int main(int argc, char *argv[]) {
    struct addrinfo hints, *servinfo, *servaddr;
    int             sock;
    int             ret;
    FILE           *fin;
    char            buf[BUF_SIZE];
    size_t          nread;
    int32_t         name_len;
    int32_t         name_len_net;

    long            total_bytes_sent;
    long            last_report_bytes;
    struct timeval  xfer_start_time;
    struct timeval  last_report_time;

    Usage(argc, argv);
    printf("Successfully initialized with:\n");
    printf("\tSource filename = %s\n", Src_filename);
    printf("\tDestination filename = %s\n", Dst_filename);
    printf("\tHostname = %s\n", Hostname);
    printf("\tPort = %s\n", Port_Str);

    fin = fopen(Src_filename, "rb");
    if (fin == NULL) {
        perror("t_ncp: fopen (source file)");
        exit(1);
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ret = getaddrinfo(Hostname, Port_Str, &hints, &servinfo);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
        exit(1);
    }

    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (sock < 0) {
            perror("t_ncp: socket");
            continue;
        }
        ret = connect(sock, servaddr->ai_addr, servaddr->ai_addrlen);
        if (ret < 0) {
            perror("t_ncp: connect");
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

    printf("Connected. Sending filename '%s'...\n", Dst_filename);

    name_len     = (int32_t)strlen(Dst_filename) + 1;
    name_len_net = htonl(name_len);

    ret = Send_all(sock, (char *)&name_len_net, sizeof(name_len_net));
    if (ret < 0) {
        perror("t_ncp: send (name length)");
        exit(1);
    }
    ret = Send_all(sock, Dst_filename, name_len);
    if (ret < 0) {
        perror("t_ncp: send (filename)");
        exit(1);
    }

    gettimeofday(&xfer_start_time, NULL);
    last_report_time  = xfer_start_time;
    total_bytes_sent  = 0;
    last_report_bytes = 0;

    for (;;) {
        nread = fread(buf, 1, BUF_SIZE, fin);
        if (nread > 0) {
            ret = Send_all(sock, buf, (int)nread);
            if (ret < 0) {
                perror("t_ncp: send (data)");
                exit(1);
            }
            total_bytes_sent += nread;

            if (total_bytes_sent - last_report_bytes >= (long)(10 * MB)) {
                double elapsed = Elapsed_sec(&last_report_time);
                double rate_mbps = elapsed > 0
                    ? ((total_bytes_sent - last_report_bytes) * 8.0 / MB) / elapsed
                    : 0.0;
                printf("[t_ncp] %.2f MB sent so far, last interval rate %.2f Mbps\n",
                       total_bytes_sent / MB, rate_mbps);
                last_report_bytes = total_bytes_sent;
                gettimeofday(&last_report_time, NULL);
            }
        }
        if (nread < BUF_SIZE) {
            if (feof(fin)) {
                break;
            } else {
                perror("t_ncp: fread (source file)");
                break;
            }
        }
    }

    {
        double total_time    = Elapsed_sec(&xfer_start_time);
        double file_size_mb  = total_bytes_sent / MB;
        double avg_rate_mbps = total_time > 0
            ? (total_bytes_sent * 8.0 / MB) / total_time
            : 0.0;

        printf("\n[t_ncp] Transfer complete.\n");
        printf("[t_ncp] File size: %.4f MB\n", file_size_mb);
        printf("[t_ncp] Transfer time: %.4f sec\n", total_time);
        printf("[t_ncp] Average rate: %.4f Mbps\n", avg_rate_mbps);
    }

    fclose(fin);
    close(sock);

    return 0;
}

static int Send_all(int sock, const char *buf, int len) {
    int sent = 0;
    int ret;

    while (sent < len) {
        ret = send(sock, buf + sent, len - sent, 0);
        if (ret < 0) {
            return -1;
        }
        sent += ret;
    }
    return sent;
}

static double Elapsed_sec(struct timeval *since) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - since->tv_sec) +
           (now.tv_usec - since->tv_usec) / 1000000.0;
}

static void Usage(int argc, char *argv[]) {

    if (argc != 3) {
        Print_help();
    }

    Src_filename = argv[1];
    Dst_filename = strtok(argv[2], "@");
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
    printf("Usage: t_ncp <source_file_name> <dest_file_name>@<ip_addr>:<port>\n");
    exit(0);
}