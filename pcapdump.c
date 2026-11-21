/* pcapdump.c -- tiny dependency-free packet capture for the EntryNav2 head unit.
 * Copyright (c) 2026 nivanikov
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * A static ARM capture tool for when the head unit has no tcpdump. It opens a raw AF_PACKET socket on an interface and
 * writes every frame (optionally only those to/from one IPv4 host) to a standard libpcap file you open in Wireshark.
 * No libpcap, no dynamic deps -- build with: arm-...-gnueabihf-gcc -O2 -static pcapdump.c -o pcapdump
 *
 *   pcapdump <iface> <outfile.pcap> [host_ip] [max_packets]
 *     host_ip      keep only IPv4 frames whose source or destination is this host (one VLAN tag handled); omit = all.
 *     max_packets  stop after this many kept packets (0/omit = until SIGINT/SIGTERM).
 * Run it from an exec path (e.g. /opt/app/cisu) but write the .pcap to a writable one (e.g. /var/local).
 */
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;
static void on_stop(int s) { (void)s; g_stop = 1; }

static void put32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE* f, uint16_t v) { fwrite(&v, 2, 1, f); }

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <iface> <out.pcap> [host_ip] [max_packets]\n", argv[0]);
        return 2;
    }
    const char* iface = argv[1];
    const char* outpath = argv[2];
    uint32_t filter = 0;  /* network-order IPv4; 0 = no filter */
    if (argc >= 4 && argv[3][0]) {
        struct in_addr a;
        if (inet_aton(argv[3], &a)) filter = a.s_addr;
        else { fprintf(stderr, "bad host_ip: %s\n", argv[3]); return 2; }
    }
    long maxpkt = (argc >= 5) ? atol(argv[4]) : 0;

    unsigned ifindex = if_nametoindex(iface);
    if (!ifindex) { fprintf(stderr, "no interface '%s'\n", iface); return 1; }

    int fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
    if (fd < 0) { perror("socket(AF_PACKET) (need root)"); return 1; }
    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ETH_P_ALL);
    sll.sll_ifindex = (int)ifindex;
    if (bind(fd, (struct sockaddr*)&sll, sizeof(sll)) < 0) { perror("bind"); close(fd); return 1; }

    FILE* out = fopen(outpath, "wb");
    if (!out) { perror("fopen out"); close(fd); return 1; }
    /* pcap global header: magic, ver 2.4, zone 0, sigfigs 0, snaplen, LINKTYPE_ETHERNET(1) */
    const uint32_t SNAP = 65535;
    put32(out, 0xa1b2c3d4u); put16(out, 2); put16(out, 4);
    put32(out, 0); put32(out, 0); put32(out, SNAP); put32(out, 1);

    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);

    unsigned char buf[65536];
    long kept = 0;
    unsigned long long bytes = 0;
    while (!g_stop) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) { if (g_stop) break; continue; }
        if (filter) {
            if (n < 14) continue;
            uint16_t et = (uint16_t)((buf[12] << 8) | buf[13]);
            size_t ip = 14;
            if (et == 0x8100 && n >= 18) { et = (uint16_t)((buf[16] << 8) | buf[17]); ip = 18; }  /* one VLAN tag */
            if (et != 0x0800 || (size_t)n < ip + 20) continue;                                    /* IPv4 only */
            uint32_t src, dst;
            memcpy(&src, buf + ip + 12, 4);
            memcpy(&dst, buf + ip + 16, 4);
            if (src != filter && dst != filter) continue;
        }
        struct timeval tv;
        gettimeofday(&tv, 0);
        uint32_t incl = (n > (ssize_t)SNAP) ? SNAP : (uint32_t)n;
        put32(out, (uint32_t)tv.tv_sec); put32(out, (uint32_t)tv.tv_usec);
        put32(out, incl); put32(out, (uint32_t)n);
        fwrite(buf, 1, incl, out);
        kept++;
        bytes += incl;
        if ((kept & 0x3f) == 0) fflush(out);
        if (maxpkt && kept >= maxpkt) break;
    }
    fflush(out);
    fclose(out);
    close(fd);
    fprintf(stderr, "pcapdump: %ld packets, %llu bytes -> %s\n", kept, bytes, outpath);
    return 0;
}
