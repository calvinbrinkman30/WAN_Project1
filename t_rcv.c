#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "net_include.h"

#define BACKLOG 4
#define BUF_SIZE 8192
#define MB 1000000.0

static void Usage(int argc, char *argv[]);
static void Print_help(void);
static int  Recv_all(int sock, char *buf, int len);
static double Elapsed_sec(struct timeval *since);

static char *Port_Str;

int main(int argc, char *argv[]) {
    struct addrinfo    hints, *servinfo, *servaddr;
    int                listen_sock;
    int                recv_sock;
    int                ret;
    long               on = 1;
    struct sockaddr_storage from_addr;
    socklen_t               from_len;
    char                    hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];
    char                    buf[BUF_SIZE];
    int32_t                 name_len_net, name_len;
    char                    filename[MAX_MESS_LEN];
    FILE                   *fout;

    Usage(argc, argv);
    printf("Listening for connections on port %s\n", Port_Str);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    ret = getaddrinfo(NULL, Port_Str, &hints, &servinfo);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(ret));
        exit(1);
    }

    for (servaddr = servinfo; servaddr != NULL; servaddr = servaddr->ai_next) {
        listen_sock = socket(servaddr->ai_family, servaddr->ai_socktype, servaddr->ai_protocol);
        if (listen_sock < 0) {
            perror("t_rcv: socket");
            continue;
        }
        if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on)) < 0) {
            perror("t_rcv: setsockopt");
            close(listen_sock);
            continue;
        }
        if (bind(listen_sock, servaddr->ai_addr, servaddr->ai_addrlen) < 0) {
            perror("t_rcv: bind");
            close(listen_sock);
            continue;
        }
        if (listen(listen_sock, BACKLOG) < 0) {
            perror("t_rcv: listen");
            close(listen_sock);
            continue;
        }
        break;
    }
    if (servaddr == NULL) {
        fprintf(stderr, "No valid address found...exiting\n");
        exit(1);
    }
    freeaddrinfo(servinfo);

    for (;;) {
        from_len = sizeof(from_addr);
        recv_sock = accept(listen_sock, (struct sockaddr *)&from_addr, &from_len);
        if (recv_sock < 0) {
            perror("t_rcv: accept");
            continue;
        }

        ret = getnameinfo((struct sockaddr *)&from_addr, from_len, hbuf,
                           sizeof(hbuf), sbuf, sizeof(sbuf),
                           NI_NUMERICHOST | NI_NUMERICSERV);
        if (ret != 0) {
            fprintf(stderr, "getnameinfo error: %s\n", gai_strerror(ret));
        } else {
            printf("Accepted connection from %s:%s\n", hbuf, sbuf);
        }

        ret = Recv_all(recv_sock, (char *)&name_len_net, sizeof(name_len_net));
        if (ret <= 0) {
            fprintf(stderr, "t_rcv: connection closed before filename length\n");
            close(recv_sock);
            continue;
        }
        name_len = ntohl(name_len_net);

        if (name_len <= 0 || name_len >= (int32_t)sizeof(filename)) {
            fprintf(stderr, "t_rcv: invalid filename length %d\n", name_len);
            close(recv_sock);
            continue;
        }

        ret = Recv_all(recv_sock, filename, name_len);
        if (ret <= 0) {
            fprintf(stderr, "t_rcv: connection closed before filename\n");
            close(recv_sock);
            continue;
        }

        fout = fopen(filename, "wb");
        if (fout == NULL) {
            perror("t_rcv: fopen (destination file)");
            close(recv_sock);
            continue;
        }
        printf("Saving to '%s'\n", filename);

        {
            long           total_bytes_written = 0;
            long           last_report_bytes    = 0;
            struct timeval xfer_start_time, last_report_time;

            gettimeofday(&xfer_start_time, NULL);
            last_report_time = xfer_start_time;

            for (;;) {
                ret = recv(recv_sock, buf, sizeof(buf), 0);
                if (ret < 0) {
                    perror("t_rcv: recv");
                    break;
                }
                if (ret == 0) {
                    break;
                }

                {
                    size_t nwritten = fwrite(buf, 1, ret, fout);
                    if (nwritten < (size_t)ret) {
                        perror("t_rcv: fwrite");
                        break;
                    }
                }
                total_bytes_written += ret;

                if (total_bytes_written - last_report_bytes >= (long)(10 * MB)) {
                    double elapsed = Elapsed_sec(&last_report_time);
                    double rate_mbps = elapsed > 0
                        ? ((total_bytes_written - last_report_bytes) * 8.0 / MB) / elapsed
                        : 0.0;
                    printf("[t_rcv] %.2f MB received so far, "
                           "last interval rate %.2f Mbps\n",
                           total_bytes_written / MB, rate_mbps);
                    last_report_bytes = total_bytes_written;
                    gettimeofday(&last_report_time, NULL);
                }
            }

            fclose(fout);

            {
                double total_time    = Elapsed_sec(&xfer_start_time);
                double file_size_mb  = total_bytes_written / MB;
                double avg_rate_mbps = total_time > 0
                    ? (total_bytes_written * 8.0 / MB) / total_time
                    : 0.0;

                printf("\n[t_rcv] Transfer complete.\n");
                printf("[t_rcv] File size: %.4f MB\n", file_size_mb);
                printf("[t_rcv] Transfer time: %.4f sec\n", total_time);
                printf("[t_rcv] Average rate: %.4f Mbps\n\n", avg_rate_mbps);
            }
        }

        close(recv_sock);
    }

    close(listen_sock);
    return 0;
}

static int Recv_all(int sock, char *buf, int len) {
    int received = 0;
    int ret;

    while (received < len) {
        ret = recv(sock, buf + received, len - received, 0);
        if (ret < 0) {
            return -1;
        }
        if (ret == 0) {
            return received;
        }
        received += ret;
    }
    return received;
}

static double Elapsed_sec(struct timeval *since) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (now.tv_sec - since->tv_sec) +
           (now.tv_usec - since->tv_usec) / 1000000.0;
}

static void Usage(int argc, char *argv[]) {
    if (argc != 2) {
        Print_help();
    }

    Port_Str = argv[1];
}

static void Print_help(void) {
    printf("Usage: t_rcv <port>\n");
    exit(0);
}