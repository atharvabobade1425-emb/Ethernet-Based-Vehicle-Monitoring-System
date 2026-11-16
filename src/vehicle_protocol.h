/*
 * vehicle_protocol.h
 * ------------------------------------------------------------------
 * Shared definitions for the Ethernet-based Vehicle Monitoring System.
 *
 * Both the Vehicle ECU (sender) and the Dashboard ECU (receiver) include
 * this header so that they agree on the message format.
 *
 * Wire format (ASCII, one message per line, NMEA-style XOR checksum):
 *
 *   VDATA,seq=12,ts=1731772800123456,speed=63.4,rpm=2150,steer=-12.5,gear=3,temp=88.2*5A\n
 *
 *   seq   : message sequence number (detects loss / re-ordering)
 *   ts    : sender timestamp in microseconds (CLOCK_REALTIME, used for latency)
 *   speed : vehicle speed  [km/h]      (mandatory signal)
 *   rpm   : engine speed   [rev/min]   (mandatory signal)
 *   steer : steering angle [deg], +ve = right, -ve = left (mandatory signal)
 *   gear  : selected gear  [0 = neutral, 1..6]          (extra signal)
 *   temp  : engine coolant temperature [deg C]          (extra signal)
 *   *XX   : XOR of every byte between 'V' and '*', in hex (integrity check)
 *
 * A text format was chosen deliberately so that the payload is readable
 * directly in Wireshark's packet-bytes pane.
 * ------------------------------------------------------------------
 */
#ifndef VEHICLE_PROTOCOL_H
#define VEHICLE_PROTOCOL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DEFAULT_PORT        5000
#define DEFAULT_IP          "127.0.0.1"
#define MAX_MSG_LEN         256

typedef struct {
    uint32_t seq;          /* sequence number                 */
    uint64_t ts_us;        /* sender timestamp, microseconds  */
    double   speed_kmph;   /* vehicle speed                   */
    uint32_t rpm;          /* engine RPM                      */
    double   steer_deg;    /* steering wheel angle            */
    uint32_t gear;         /* gear (0 = N)                    */
    double   coolant_c;    /* coolant temperature             */
} vehicle_data_t;

/* Current wall-clock time in microseconds. */
static inline uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* XOR checksum over a buffer (NMEA style). */
static inline uint8_t vp_checksum(const char *buf, size_t len)
{
    uint8_t c = 0;
    for (size_t i = 0; i < len; i++)
        c ^= (uint8_t)buf[i];
    return c;
}

/*
 * Serialise a vehicle_data_t into buf.
 * Returns number of bytes written (excluding the terminating NUL) or -1.
 */
static inline int vp_encode(const vehicle_data_t *d, char *buf, size_t size)
{
    int n = snprintf(buf, size,
                     "VDATA,seq=%u,ts=%llu,speed=%.1f,rpm=%u,steer=%.1f,gear=%u,temp=%.1f",
                     d->seq, (unsigned long long)d->ts_us, d->speed_kmph,
                     d->rpm, d->steer_deg, d->gear, d->coolant_c);
    if (n < 0 || (size_t)n + 5 > size)
        return -1;
    uint8_t cs = vp_checksum(buf, (size_t)n);
    n += snprintf(buf + n, size - (size_t)n, "*%02X\n", cs);
    return n;
}

/*
 * Parse one message (with or without trailing newline) into d.
 * Returns  0 on success,
 *         -1 on malformed message,
 *         -2 on checksum mismatch.
 */
static inline int vp_decode(const char *msg, vehicle_data_t *d)
{
    const char *star = strrchr(msg, '*');
    if (strncmp(msg, "VDATA,", 6) != 0 || star == NULL)
        return -1;

    unsigned int rx_cs;
    if (sscanf(star + 1, "%2X", &rx_cs) != 1)
        return -1;
    if (vp_checksum(msg, (size_t)(star - msg)) != (uint8_t)rx_cs)
        return -2;

    unsigned long long ts;
    int fields = sscanf(msg,
                        "VDATA,seq=%u,ts=%llu,speed=%lf,rpm=%u,steer=%lf,gear=%u,temp=%lf",
                        &d->seq, &ts, &d->speed_kmph, &d->rpm,
                        &d->steer_deg, &d->gear, &d->coolant_c);
    if (fields != 7)
        return -1;
    d->ts_us = (uint64_t)ts;
    return 0;
}

#endif /* VEHICLE_PROTOCOL_H */
