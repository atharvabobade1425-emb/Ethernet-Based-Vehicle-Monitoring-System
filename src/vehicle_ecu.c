/*
 * vehicle_ecu.c
 * ------------------------------------------------------------------
 * Vehicle ECU - periodically generates vehicle data (speed, engine RPM,
 * steering angle, gear, coolant temperature) and transmits it to the
 * Dashboard ECU over Ethernet using either UDP or TCP sockets.
 *
 * Usage:
 *   ./vehicle_ecu -m udp|tcp [-i dest_ip] [-p port] [-n count]
 *                 [-t interval_ms] [-r retries] [-q]
 *
 *   -m  transport protocol: udp or tcp           (default: udp)
 *   -i  Dashboard ECU IPv4 address               (default: 127.0.0.1)
 *   -p  Dashboard ECU port                       (default: 5000)
 *   -n  number of messages, 0 = run until Ctrl+C (default: 0)
 *   -t  transmit period in milliseconds          (default: 1000)
 *   -r  TCP connection attempts before giving up (default: 5)
 *   -q  quiet: do not print every transmitted frame
 *
 * Examples:
 *   ./vehicle_ecu -m udp                    # UDP, 1 Hz, forever
 *   ./vehicle_ecu -m tcp -i 127.0.0.1 -n 20 # TCP, 20 messages
 * ------------------------------------------------------------------
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "vehicle_protocol.h"

#define CONNECT_TIMEOUT_MS   3000   /* TCP connect() timeout          */
#define RETRY_DELAY_S        2      /* delay between TCP connect tries */

typedef enum { PROTO_UDP, PROTO_TCP } proto_t;

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---------------- statistics ---------------- */
typedef struct {
    unsigned long generated;
    unsigned long sent;
    unsigned long send_errors;
    unsigned long refused;      /* UDP: ICMP port unreachable reported */
    unsigned long reconnects;
    unsigned long bytes;
    uint64_t      start_us;
} tx_stats_t;

/* ---------------- vehicle model ----------------
 * A simple repeating 60-second drive cycle:
 *   0-19 s  accelerate 0 -> ~100 km/h
 *  20-39 s  cruise ~100 km/h
 *  40-59 s  brake back to standstill
 * RPM is derived from speed and gear; steering follows a slow sine
 * (lane changes / curves); coolant warms up from 25 C to ~90 C.
 */
static double noise(double amplitude)
{
    return amplitude * (((double)rand() / RAND_MAX) * 2.0 - 1.0);
}

static void generate_vehicle_data(vehicle_data_t *d, uint32_t seq, double t_s)
{
    static const double rpm_per_kmph[7] = { 0, 110, 62, 44, 34, 27, 23 };
    double cycle = fmod(t_s, 60.0);
    double speed;

    if (cycle < 20.0)
        speed = cycle * 5.0;                 /* accelerate */
    else if (cycle < 40.0)
        speed = 100.0;                       /* cruise     */
    else
        speed = (60.0 - cycle) * 5.0;        /* brake      */
    speed += noise(1.5);
    if (speed < 0.5)
        speed = 0.0;

    uint32_t gear;
    if (speed < 1.0)        gear = 0;
    else if (speed < 20.0)  gear = 1;
    else if (speed < 40.0)  gear = 2;
    else if (speed < 60.0)  gear = 3;
    else if (speed < 80.0)  gear = 4;
    else if (speed < 105.0) gear = 5;
    else                    gear = 6;

    double rpm = 800.0 + speed * rpm_per_kmph[gear] + noise(40.0);
    if (rpm > 7000.0) rpm = 7000.0;

    double steer = 30.0 * sin(2.0 * M_PI * t_s / 30.0) + noise(2.0);
    if (steer > 540.0)  steer = 540.0;
    if (steer < -540.0) steer = -540.0;

    double temp = 25.0 + 65.0 * (1.0 - exp(-t_s / 60.0)) + noise(0.3);

    d->seq        = seq;
    d->ts_us      = now_us();
    d->speed_kmph = speed;
    d->rpm        = (uint32_t)rpm;
    d->steer_deg  = steer;
    d->gear       = gear;
    d->coolant_c  = temp;
}

/* ---------------- socket helpers ---------------- */

/* connect() with a timeout so an unreachable IP does not hang for ~2 min. */
static int connect_with_timeout(int fd, const struct sockaddr_in *addr, int timeout_ms)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (const struct sockaddr *)addr, sizeof(*addr));
    if (rc < 0 && errno != EINPROGRESS)
        return -1;

    if (rc < 0) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        rc = poll(&pfd, 1, timeout_ms);
        if (rc == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (rc < 0)
            return -1;
        int so_err = 0;
        socklen_t len = sizeof(so_err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_err, &len);
        if (so_err != 0) {
            errno = so_err;
            return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);       /* back to blocking mode */
    return 0;
}

/* Create a TCP socket connected to the Dashboard ECU, retrying if needed. */
static int tcp_open(const struct sockaddr_in *addr, int max_attempts)
{
    for (int attempt = 1; attempt <= max_attempts && g_running; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            perror("[ERROR] socket");
            return -1;
        }
        printf("[INFO ] TCP connect attempt %d/%d to %s:%u ...\n", attempt,
               max_attempts, inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
        fflush(stdout);

        if (connect_with_timeout(fd, addr, CONNECT_TIMEOUT_MS) == 0) {
            struct sockaddr_in local;
            socklen_t len = sizeof(local);
            getsockname(fd, (struct sockaddr *)&local, &len);
            printf("[INFO ] Connected (3-way handshake complete). Local port %u\n",
                   ntohs(local.sin_port));
            return fd;
        }

        int err = errno;
        fprintf(stderr, "[WARN ] connect() failed: %s%s\n", strerror(err),
                err == ECONNREFUSED ? " -> Dashboard ECU not running?" :
                err == ETIMEDOUT    ? " -> host unreachable / wrong IP?" : "");
        close(fd);
        if (attempt < max_attempts && g_running)
            sleep(RETRY_DELAY_S);
    }
    return -1;
}

static int udp_open(const struct sockaddr_in *addr)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("[ERROR] socket");
        return -1;
    }
    /*
     * connect() on a UDP socket does NOT send anything on the wire; it only
     * fixes the default destination.  It also lets the kernel report ICMP
     * "port unreachable" errors back to us as ECONNREFUSED, which is how we
     * can notice that the Dashboard ECU is not running.
     */
    if (connect(fd, (const struct sockaddr *)addr, sizeof(*addr)) < 0) {
        fprintf(stderr, "[ERROR] UDP connect(): %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

/* Send the whole buffer (TCP may accept fewer bytes than requested). */
static ssize_t send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)off;
}

/* Graceful TCP close: send FIN, wait for the peer's FIN, then close. */
static void tcp_graceful_close(int fd)
{
    char tmp[64];
    printf("[INFO ] Closing connection: shutdown(SHUT_WR) -> FIN sent\n");
    shutdown(fd, SHUT_WR);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    while (poll(&pfd, 1, 2000) > 0) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0)
            break;
    }
    close(fd);
    printf("[INFO ] Connection closed (FIN/ACK exchange complete)\n");
}

/* Sleep until an absolute monotonic deadline (no drift over long runs). */
static void sleep_until(struct timespec *next, double period_ms)
{
    long long ns = (long long)(period_ms * 1e6);
    next->tv_sec  += ns / 1000000000LL;
    next->tv_nsec += ns % 1000000000LL;
    if (next->tv_nsec >= 1000000000L) {
        next->tv_sec++;
        next->tv_nsec -= 1000000000L;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL) == EINTR && g_running)
        ;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -m udp|tcp [-i dest_ip] [-p port] [-n count] [-t interval_ms] [-r retries] [-q]\n"
            "  -m  protocol (udp|tcp)            default udp\n"
            "  -i  Dashboard ECU IPv4 address    default %s\n"
            "  -p  Dashboard ECU port            default %d\n"
            "  -n  messages to send (0=forever)  default 0\n"
            "  -t  period in ms                  default 1000\n"
            "  -r  TCP connect attempts          default 5\n"
            "  -q  quiet (no per-frame output)\n",
            prog, DEFAULT_IP, DEFAULT_PORT);
}

int main(int argc, char *argv[])
{
    proto_t     proto       = PROTO_UDP;
    const char *ip          = DEFAULT_IP;
    int         port        = DEFAULT_PORT;
    long        count       = 0;
    double      interval_ms = 1000.0;
    int         retries     = 5;
    bool        quiet       = false;
    int         opt;

    while ((opt = getopt(argc, argv, "m:i:p:n:t:r:qh")) != -1) {
        switch (opt) {
        case 'm':
            if (strcmp(optarg, "udp") == 0)      proto = PROTO_UDP;
            else if (strcmp(optarg, "tcp") == 0) proto = PROTO_TCP;
            else { usage(argv[0]); return 1; }
            break;
        case 'i': ip          = optarg;               break;
        case 'p': port        = atoi(optarg);         break;
        case 'n': count       = atol(optarg);         break;
        case 't': interval_ms = atof(optarg);         break;
        case 'r': retries     = atoi(optarg);         break;
        case 'q': quiet       = true;                 break;
        default:  usage(argv[0]);                     return 1;
        }
    }

    if (port <= 0 || port > 65535 || interval_ms < 0 || retries < 1) {
        fprintf(stderr, "[ERROR] Invalid port, interval or retry count\n");
        return 1;
    }

    /* ---- validate the destination IP address (Scenario 4) ---- */
    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &dest.sin_addr) != 1) {
        fprintf(stderr, "[ERROR] Invalid IP address '%s' - expected dotted IPv4, e.g. 127.0.0.1\n", ip);
        return 2;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);    /* handle broken TCP pipes via errno instead */
    srand((unsigned)time(NULL));

    printf("==============================================\n");
    printf(" Vehicle ECU  |  %s  ->  %s:%d\n", proto == PROTO_UDP ? "UDP" : "TCP", ip, port);
    if (count > 0)
        printf(" Period %.1f ms | Messages %ld\n", interval_ms, count);
    else
        printf(" Period %.1f ms | Messages until Ctrl+C\n", interval_ms);
    printf("==============================================\n");

    int fd = (proto == PROTO_UDP) ? udp_open(&dest) : tcp_open(&dest, retries);
    if (fd < 0) {
        fprintf(stderr, "[ERROR] Could not reach Dashboard ECU at %s:%d - exiting\n", ip, port);
        return 3;
    }

    tx_stats_t st = { .start_us = now_us() };
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    char buf[MAX_MSG_LEN];
    uint32_t seq = 0;

    while (g_running && (count == 0 || (long)st.generated < count)) {
        vehicle_data_t d;
        double t_s = (double)seq * interval_ms / 1000.0;
        generate_vehicle_data(&d, seq, t_s);
        int len = vp_encode(&d, buf, sizeof(buf));
        st.generated++;
        seq++;

        ssize_t n = send_all(fd, buf, (size_t)len);
        if (n < 0) {
            int err = errno;
            st.send_errors++;
            if (proto == PROTO_UDP) {
                if (err == ECONNREFUSED) {
                    st.refused++;
                    fprintf(stderr, "[WARN ] seq=%u: ICMP port unreachable - Dashboard ECU not "
                            "listening (datagram lost, continuing)\n", d.seq);
                } else {
                    fprintf(stderr, "[WARN ] seq=%u: send failed: %s\n", d.seq, strerror(err));
                }
            } else {
                fprintf(stderr, "[WARN ] seq=%u: TCP send failed: %s -> reconnecting\n",
                        d.seq, strerror(err));
                close(fd);
                fd = tcp_open(&dest, retries);
                if (fd < 0) {
                    fprintf(stderr, "[ERROR] Dashboard ECU lost and reconnection failed\n");
                    break;
                }
                st.reconnects++;
            }
        } else {
            st.sent++;
            st.bytes += (unsigned long)n;
            if (!quiet)
                printf("[TX %5u] Speed %6.1f km/h | RPM %4u | Steer %+6.1f deg | Gear %u | Temp %5.1f C\n",
                       d.seq, d.speed_kmph, d.rpm, d.steer_deg, d.gear, d.coolant_c);
            else if (st.sent % 1000 == 0)
                printf("[TX] %lu messages sent\n", st.sent);
            fflush(stdout);
        }

        if (interval_ms > 0 && g_running && (count == 0 || (long)st.generated < count))
            sleep_until(&next, interval_ms);
    }

    if (fd >= 0) {
        if (proto == PROTO_TCP)
            tcp_graceful_close(fd);
        else
            close(fd);
    }

    double dur = (double)(now_us() - st.start_us) / 1e6;
    printf("---------------- Vehicle ECU summary ----------------\n");
    printf(" Protocol        : %s\n", proto == PROTO_UDP ? "UDP" : "TCP");
    printf(" Generated       : %lu\n", st.generated);
    printf(" Sent OK         : %lu\n", st.sent);
    printf(" Send errors     : %lu", st.send_errors);
    if (proto == PROTO_UDP) printf("  (ICMP port-unreachable: %lu)", st.refused);
    else                    printf("  (reconnects: %lu)", st.reconnects);
    printf("\n Payload bytes   : %lu\n", st.bytes);
    printf(" Duration        : %.2f s\n", dur);
    printf("-----------------------------------------------------\n");
    return 0;
}
