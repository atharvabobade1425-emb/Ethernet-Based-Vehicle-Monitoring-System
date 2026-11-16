/*
 * dashboard_ecu.c
 * ------------------------------------------------------------------
 * Dashboard ECU - receives vehicle data from the Vehicle ECU over
 * Ethernet (UDP or TCP), validates it, and displays it to the user.
 *
 * Usage:
 *   ./dashboard_ecu -m udp|tcp [-b bind_ip] [-p port] [-c count]
 *                   [-T idle_timeout_s] [-q]
 *
 *   -m  transport protocol: udp or tcp                (default: udp)
 *   -b  local IPv4 address to listen on               (default: 0.0.0.0)
 *   -p  local port                                    (default: 5000)
 *   -c  exit after receiving this many messages (0=never)
 *   -T  exit after this many seconds without data (0=never)
 *   -q  quiet: do not print every received frame
 *
 * Received data is checked for:
 *   - checksum errors   (corrupted payload)
 *   - sequence gaps     (lost messages)
 *   - out-of-order / duplicate messages
 * and the end-to-end latency (sender timestamp -> receive time) is measured.
 * ------------------------------------------------------------------
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vehicle_protocol.h"

typedef enum { PROTO_UDP, PROTO_TCP } proto_t;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---------------- receive statistics ---------------- */
typedef struct {
    unsigned long received;      /* messages received (valid or not) */
    unsigned long valid;
    unsigned long bad_checksum;
    unsigned long malformed;
    unsigned long lost;          /* inferred from sequence gaps      */
    unsigned long out_of_order;  /* late or duplicate                */
    unsigned long connections;   /* TCP sessions accepted            */
    unsigned long bytes;
    bool          have_seq;
    uint32_t      next_seq;
    double        lat_min_ms, lat_max_ms, lat_sum_ms;
    uint64_t      first_us, last_us;
} rx_stats_t;

static rx_stats_t st;
static bool g_quiet = false;

static const char *gear_str(uint32_t g)
{
    static const char *names[] = { "N", "1", "2", "3", "4", "5", "6" };
    return g < 7 ? names[g] : "?";
}

/* Validate, track and display one received message. */
static void process_message(const char *msg, const char *peer)
{
    vehicle_data_t d;
    uint64_t rx_us = now_us();
    st.received++;
    if (st.first_us == 0)
        st.first_us = rx_us;
    st.last_us = rx_us;

    int rc = vp_decode(msg, &d);
    if (rc == -2) {
        st.bad_checksum++;
        fprintf(stderr, "[WARN ] Checksum error from %s - frame discarded\n", peer);
        return;
    }
    if (rc != 0) {
        st.malformed++;
        fprintf(stderr, "[WARN ] Malformed frame from %s - discarded\n", peer);
        return;
    }
    st.valid++;

    /* sequence tracking */
    const char *flag = "";
    if (!st.have_seq) {
        st.have_seq = true;
        st.next_seq = d.seq + 1;
    } else if (d.seq == st.next_seq) {
        st.next_seq++;
    } else if (d.seq > st.next_seq) {
        unsigned long gap = d.seq - st.next_seq;
        st.lost += gap;
        st.next_seq = d.seq + 1;
        flag = "  <-- GAP";
        if (!g_quiet)
            fprintf(stderr, "[WARN ] %lu message(s) missing before seq=%u\n", gap, d.seq);
    } else {
        st.out_of_order++;
        flag = "  <-- LATE/DUP";
    }

    /* latency (both ECUs run on the same machine -> same clock) */
    double lat_ms = (double)((int64_t)(rx_us - d.ts_us)) / 1000.0;
    if (st.valid == 1 || lat_ms < st.lat_min_ms) st.lat_min_ms = lat_ms;
    if (st.valid == 1 || lat_ms > st.lat_max_ms) st.lat_max_ms = lat_ms;
    st.lat_sum_ms += lat_ms;

    if (!g_quiet) {
        printf("[RX %5u] Speed %6.1f km/h | RPM %4u | Steer %+6.1f deg %-5s | Gear %s | Temp %5.1f C | %.3f ms%s\n",
               d.seq, d.speed_kmph, d.rpm, d.steer_deg,
               d.steer_deg > 1.0 ? "RIGHT" : d.steer_deg < -1.0 ? "LEFT" : "",
               gear_str(d.gear), d.coolant_c, lat_ms, flag);
        if (d.rpm > 6000)
            printf("            !! WARNING: engine over-rev (%u RPM)\n", d.rpm);
        if (d.coolant_c > 105.0)
            printf("            !! WARNING: engine over-temperature (%.1f C)\n", d.coolant_c);
    } else if (st.valid % 1000 == 0) {
        printf("[RX] %lu valid messages, %lu lost so far\n", st.valid, st.lost);
    }
    fflush(stdout);
}

static void print_summary(proto_t proto)
{
    double dur = st.last_us > st.first_us ? (double)(st.last_us - st.first_us) / 1e6 : 0.0;
    unsigned long expected = st.valid + st.lost;
    printf("\n--------------- Dashboard ECU summary ---------------\n");
    printf(" Protocol          : %s\n", proto == PROTO_UDP ? "UDP" : "TCP");
    if (proto == PROTO_TCP)
        printf(" TCP sessions      : %lu\n", st.connections);
    printf(" Frames received   : %lu  (%lu bytes)\n", st.received, st.bytes);
    printf(" Valid             : %lu\n", st.valid);
    printf(" Checksum errors   : %lu\n", st.bad_checksum);
    printf(" Malformed         : %lu\n", st.malformed);
    printf(" Lost (seq gaps)   : %lu  (%.3f %%)\n", st.lost,
           expected ? 100.0 * (double)st.lost / (double)expected : 0.0);
    printf(" Out-of-order/dup  : %lu\n", st.out_of_order);
    if (st.valid)
        printf(" Latency ms        : min %.3f / avg %.3f / max %.3f\n",
               st.lat_min_ms, st.lat_sum_ms / (double)st.valid, st.lat_max_ms);
    printf(" Active duration   : %.2f s\n", dur);
    printf("-----------------------------------------------------\n");
}

static bool count_reached(long count)
{
    return count > 0 && (long)st.received >= count;
}

/* Wait for the socket to become readable; returns >0 ready, 0 idle timeout, <0 stop. */
static int wait_readable(int fd, int idle_timeout_s)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int waited_ms = 0;
    while (g_running) {
        int rc = poll(&pfd, 1, 500);
        if (rc > 0)
            return rc;
        if (rc < 0 && errno != EINTR) {
            perror("[ERROR] poll");
            return -1;
        }
        waited_ms += 500;
        if (idle_timeout_s > 0 && waited_ms >= idle_timeout_s * 1000) {
            printf("[INFO ] No data for %d s - stopping\n", idle_timeout_s);
            return 0;
        }
    }
    return -1;
}

/* ---------------- UDP receiver ---------------- */
static int run_udp(int fd, long count, int idle_timeout_s)
{
    char buf[MAX_MSG_LEN + 1];
    while (g_running && !count_reached(count)) {
        if (wait_readable(fd, idle_timeout_s) <= 0)
            break;
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(fd, buf, MAX_MSG_LEN, 0, (struct sockaddr *)&src, &slen);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            perror("[ERROR] recvfrom");
            return -1;
        }
        buf[n] = '\0';
        st.bytes += (unsigned long)n;
        char peer[64];
        snprintf(peer, sizeof(peer), "%s:%u", inet_ntoa(src.sin_addr), ntohs(src.sin_port));
        if (st.received == 0)
            printf("[INFO ] First datagram from Vehicle ECU %s\n", peer);
        process_message(buf, peer);
    }
    return 0;
}

/* ---------------- TCP receiver ---------------- */
static void serve_tcp_client(int cfd, const char *peer, long count, int idle_timeout_s)
{
    /* TCP is a byte stream: messages are framed by '\n'. */
    char stream[4 * MAX_MSG_LEN];
    size_t used = 0;

    while (g_running && !count_reached(count)) {
        int w = wait_readable(cfd, idle_timeout_s);
        if (w <= 0)
            break;
        ssize_t n = recv(cfd, stream + used, sizeof(stream) - used - 1, 0);
        if (n == 0) {
            printf("[INFO ] Vehicle ECU %s closed the connection (FIN received)\n", peer);
            break;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "[WARN ] recv from %s: %s\n", peer, strerror(errno));
            break;
        }
        st.bytes += (unsigned long)n;
        used += (size_t)n;
        stream[used] = '\0';

        char *start = stream, *nl;
        while ((nl = strchr(start, '\n')) != NULL) {
            *nl = '\0';
            process_message(start, peer);
            start = nl + 1;
        }
        used = (size_t)(stream + used - start);
        memmove(stream, start, used);
        if (used >= sizeof(stream) - 1) {         /* garbage without newline */
            st.malformed++;
            used = 0;
        }
    }
    close(cfd);
    printf("[INFO ] Connection with %s closed\n", peer);
}

static int run_tcp(int lfd, long count, int idle_timeout_s)
{
    while (g_running && !count_reached(count)) {
        printf("[INFO ] Waiting for Vehicle ECU connection ...\n");
        fflush(stdout);
        if (wait_readable(lfd, idle_timeout_s) <= 0)
            break;
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);
        int cfd = accept(lfd, (struct sockaddr *)&cli, &clen);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            perror("[ERROR] accept");
            return -1;
        }
        st.connections++;
        char peer[64];
        snprintf(peer, sizeof(peer), "%s:%u", inet_ntoa(cli.sin_addr), ntohs(cli.sin_port));
        printf("[INFO ] Vehicle ECU connected from %s (session %lu)\n", peer, st.connections);
        serve_tcp_client(cfd, peer, count, idle_timeout_s);
    }
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -m udp|tcp [-b bind_ip] [-p port] [-c count] [-T idle_timeout_s] [-q]\n"
            "  -m  protocol (udp|tcp)          default udp\n"
            "  -b  listen address              default 0.0.0.0\n"
            "  -p  listen port                 default %d\n"
            "  -c  stop after N messages       default 0 (never)\n"
            "  -T  stop after N idle seconds   default 0 (never)\n"
            "  -q  quiet (summary only)\n",
            prog, DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
    proto_t     proto   = PROTO_UDP;
    const char *bind_ip = "0.0.0.0";
    int         port    = DEFAULT_PORT;
    long        count   = 0;
    int         idle_s  = 0;
    int         opt;

    while ((opt = getopt(argc, argv, "m:b:p:c:T:qh")) != -1) {
        switch (opt) {
        case 'm':
            if (strcmp(optarg, "udp") == 0)      proto = PROTO_UDP;
            else if (strcmp(optarg, "tcp") == 0) proto = PROTO_TCP;
            else { usage(argv[0]); return 1; }
            break;
        case 'b': bind_ip = optarg;        break;
        case 'p': port    = atoi(optarg);  break;
        case 'c': count   = atol(optarg);  break;
        case 'T': idle_s  = atoi(optarg);  break;
        case 'q': g_quiet = true;          break;
        default:  usage(argv[0]);          return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "[ERROR] Invalid port %d\n", port);
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, bind_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "[ERROR] Invalid bind IP address '%s'\n", bind_ip);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;          /* no SA_RESTART: poll()/recv() return EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    int fd = socket(AF_INET, proto == PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) {
        perror("[ERROR] socket");
        return 3;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[ERROR] bind %s:%d: %s\n", bind_ip, port, strerror(errno));
        close(fd);
        return 3;
    }
    if (proto == PROTO_TCP && listen(fd, 1) < 0) {
        perror("[ERROR] listen");
        close(fd);
        return 3;
    }

    printf("==============================================\n");
    printf(" Dashboard ECU  |  %s  listening on %s:%d\n",
           proto == PROTO_UDP ? "UDP" : "TCP", bind_ip, port);
    printf(" Press Ctrl+C to stop\n");
    printf("==============================================\n");
    fflush(stdout);

    int rc = (proto == PROTO_UDP) ? run_udp(fd, count, idle_s) : run_tcp(fd, count, idle_s);
    close(fd);
    print_summary(proto);
    return rc == 0 ? 0 : 4;
}
