# Makefile - Ethernet-Based Vehicle Monitoring System
#
#   make            build vehicle_ecu and dashboard_ecu
#   make clean      remove binaries
#   make run-udp-dashboard / run-udp-vehicle    quick UDP demo (two terminals)
#   make run-tcp-dashboard / run-tcp-vehicle    quick TCP demo (two terminals)

CC      := gcc
CFLAGS  := -Wall -Wextra -Wpedantic -O2 -std=c11
LDLIBS  := -lm
HEADERS := vehicle_protocol.h
TARGETS := vehicle_ecu dashboard_ecu

.PHONY: all clean run-udp-dashboard run-udp-vehicle run-tcp-dashboard run-tcp-vehicle

all: $(TARGETS)

vehicle_ecu: vehicle_ecu.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

dashboard_ecu: dashboard_ecu.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

run-udp-dashboard: dashboard_ecu
	./dashboard_ecu -m udp -p 5000

run-udp-vehicle: vehicle_ecu
	./vehicle_ecu -m udp -i 127.0.0.1 -p 5000

run-tcp-dashboard: dashboard_ecu
	./dashboard_ecu -m tcp -p 5001

run-tcp-vehicle: vehicle_ecu
	./vehicle_ecu -m tcp -i 127.0.0.1 -p 5001

clean:
	rm -f $(TARGETS) *.o
