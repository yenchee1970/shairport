/*
 * Apple RTP protocol handler. This file is part of Shairport.
 * Copyright (c) James Laird 2013
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <memory.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include "config.h"
#include "common.h"
#include "player.h"
#ifdef MACH_TIME
#include <mach/mach.h>
#include <mach/clock.h>
#endif

#define NTPCACHESIZE 7

// only one RTP session can be active at a time.
static int running = 0;
static int please_shutdown;

static SOCKADDR rtp_client;
static SOCKADDR rtp_timing;
static socklen_t addrlen;
static int server_sock;
static int timing_sock;
static pthread_t rtp_thread;
static pthread_t ntp_receive_thread;
static pthread_t ntp_send_thread;
long long ntp_cache[NTPCACHESIZE + 1];
static int strict_rtp;

void rtp_record(int rtp_mode){
    debug(2, "Setting strict_rtp to %d\n", rtp_mode);
    strict_rtp = rtp_mode;
}

static void get_current_time(struct timespec *tsp) {
#ifdef MACH_TIME
    kern_return_t retval = KERN_SUCCESS;
    clock_serv_t cclock;
    mach_timespec_t mts;

    host_get_clock_service(mach_host_self(), SYSTEM_CLOCK, &cclock);
    retval = clock_get_time(cclock, &mts);
    mach_port_deallocate(mach_task_self(), cclock);

    tsp->tv_sec = mts.tv_sec;
    tsp->tv_nsec = mts.tv_nsec;
#else
    clock_gettime(CLOCK_MONOTONIC, tsp);
#endif
}

static void reset_ntp_cache() {
    int i;
    for (i = 0; i < NTPCACHESIZE; i++) {
        ntp_cache[i] = LLONG_MIN;
    }
    ntp_cache[NTPCACHESIZE] = 0;
}

long long get_ntp_offset() {
    return ntp_cache[NTPCACHESIZE];
}

static void update_ntp_cache(long long offset, long long arrival_time) {
    // average the offsets, filter out outliers

    int i, d, minindex, maxindex;
    long long total;

    for (i = 0; i < (NTPCACHESIZE - 1);  i++) {
        ntp_cache[i] = ntp_cache[i+1];
    }
    ntp_cache[NTPCACHESIZE - 1] = offset;

    d = 0;
    minindex = 0;
    maxindex = 0;
    for (i = 0; i < NTPCACHESIZE; i++) {
        if (ntp_cache[i] != LLONG_MIN) {
            d++;
            minindex = (ntp_cache[i] < ntp_cache[minindex] ? i : minindex);
            maxindex = (ntp_cache[i] > ntp_cache[maxindex] ? i : maxindex);
        }
    }
    debug(2, "ntp: valid entries: %d\n", d);
    if (d < 5)
        minindex = maxindex = -1;
    d = 0;
    total = 0;
    for (i = 0; i < NTPCACHESIZE; i++) {
        debug(3, "ntp[%d]: %lld, d: %d\n", i, ntp_cache[i] , d);
        if ((ntp_cache[i] != LLONG_MIN) && (i != minindex) && (i != maxindex)) {
            d++;
            total += ntp_cache[i];
        }
    }
    ntp_cache[NTPCACHESIZE] = total / d;
    debug(2, "ntp: offset: %lld, d: %d\n", ntp_cache[NTPCACHESIZE], d);
}

static long long tspk_to_us(struct timespec tspk) {
    long long usecs;

    usecs = tspk.tv_sec * 1000000LL;

    return usecs + (tspk.tv_nsec / 1000);
}

long long tstp_us() {
    struct timespec tv;
    get_current_time(&tv);
    return tspk_to_us(tv);
}

static long long ntp_tsp_to_us(uint32_t timestamp_hi, uint32_t timestamp_lo) {
    long long timetemp;

    timetemp = (long long)timestamp_hi * 1000000LL;
    timetemp += ((long long)timestamp_lo * 1000000LL) >> 32;

    return timetemp;
}

static void *rtp_receiver(void *arg) {
    // we inherit the signal mask (SIGUSR1)
    uint8_t packet[2048], *pktp;
    sync_cfg sync_tag, no_tag;
    sync_cfg * pkt_tag;
    int sync_fresh = 0;
    ssize_t nread;

    no_tag.rtp_tsp = 0;
    no_tag.ntp_tsp = 0;
    no_tag.sync_mode = NOSYNC;

    while (1) {
        if (please_shutdown)
            break;
        nread = recv(server_sock, packet, sizeof(packet), 0);
        if (nread < 0)
            break;

        ssize_t plen = nread;
        uint8_t type = packet[1] & ~0x80;
        if (type==0x54) {  // sync
            if (plen != 20) {
                warn("Sync packet with wrong length %d received\n", plen);
                continue;
            }

            sync_tag.rtp_tsp = ntohl(*(uint32_t *)(packet+16));
            debug(3, "Sync packet rtp_tsp %lu\n", sync_tag.rtp_tsp);
            sync_tag.ntp_tsp = ntp_tsp_to_us(ntohl(*(uint32_t *)(packet+8)), ntohl(*(uint32_t *)(packet+12)));
            debug(3, "Sync packet ntp_tsp %lld\n", sync_tag.ntp_tsp);
            // check if extension bit is set; this will be the case for the first sync
            sync_tag.sync_mode = ((packet[0] & 0x10) ? E_NTPSYNC : NTPSYNC);
            sync_fresh = 1;
            continue;
        }
        if (type == 0x60 || type == 0x56) {   // audio data / resend
            pktp = packet;
            if (type==0x56) {
                pktp += 4;
                plen -= 4;
            }

            seq_t seqno = ntohs(*(uint16_t *)(pktp+2));
            unsigned long rtp_tsp = ntohl(*(uint32_t *)(pktp+4));

            pktp += 12;
            plen -= 12;

            // check if packet contains enough content to be reasonable
            if (plen >= 16) {
                // strict -> find a rtp match, this might happen on resend packets, or,
                // in weird network circumstances, even more than once.
                // non-strickt -> just stick it to the first audio packet, _once_
                if ((strict_rtp && (rtp_tsp == sync_tag.rtp_tsp))
                        || (!strict_rtp && sync_fresh && (type == 0x60))) {
                    debug(2, "Packet for with sync data was sent has arrived (%04X)\n", seqno);
                    pkt_tag = &sync_tag;
                    sync_fresh = 0;
                } else
                    pkt_tag = &no_tag;

                player_put_packet(seqno, *pkt_tag, pktp, plen);
                continue;
            }
            if (type == 0x56 && seqno == 0) {
                debug(2, "resend-related request packet received, ignoring.\n");
                continue;
            }
            debug(1, "Unknown RTP packet of type 0x%02X length %d seqno %d\n", type, nread, seqno);
            continue;
        }
        warn("Unknown RTP packet of type 0x%02X length %d", type, nread);
    }

    debug(1, "RTP thread interrupted. terminating.\n");

    return NULL;
}

static void *ntp_receiver(void *arg) {
    // we inherit the signal mask (SIGUSR1)
    uint8_t packet[2048];
    struct timespec tv;

    ssize_t nread;
    while (1) {
        if (please_shutdown)
            break;
        nread = recv(timing_sock, packet, sizeof(packet), 0);
        if (nread < 0)
            break;
        get_current_time(&tv);

        ssize_t plen = nread;
        uint8_t type = packet[1] & ~0x80;
        if (type == 0x53) {
            if (plen != 32) {
                warn("Timing packet with wrong length %d received\n", plen);
                continue;
            }
            long long ntp_ref_tsp = ntp_tsp_to_us(ntohl(*(uint32_t *)(packet+8)), ntohl(*(uint32_t *)(packet+12)));
            debug(2, "Timing packet ntp_ref_tsp %lld\n", ntp_ref_tsp);
            long long ntp_rec_tsp = ntp_tsp_to_us(ntohl(*(uint32_t *)(packet+16)), ntohl(*(uint32_t *)(packet+20)));
            debug(2, "Timing packet ntp_rec_tsp %lld\n", ntp_rec_tsp);
            long long ntp_sen_tsp = ntp_tsp_to_us(ntohl(*(uint32_t *)(packet+24)), ntohl(*(uint32_t *)(packet+28)));
            debug(2, "Timing packet ntp_sen_tsp %lld\n", ntp_sen_tsp);
            long long ntp_loc_tsp = tspk_to_us(tv);
            debug(2, "Timing packet ntp_loc_tsp %lld\n", ntp_loc_tsp);

            // from the ntp spec:
            //    d = (t4 - t1) - (t3 - t2)  and  c = (t2 - t1 + t3 - t4)/2
            long long d = (ntp_loc_tsp - ntp_ref_tsp) - (ntp_sen_tsp - ntp_rec_tsp);
            long long c = ((ntp_rec_tsp - ntp_ref_tsp) + (ntp_sen_tsp - ntp_loc_tsp)) / 2;

            debug(1, "Round-trip delay %lld us\n", d);
            debug(1, "Clock offset %lld us\n", c);
            update_ntp_cache(c, ntp_loc_tsp);
            continue;
        }
        warn("Unknown Timing packet of type 0x%02X length %d", type, nread);
    }

    debug(1, "Time receive thread interrupted. terminating.\n");

    return NULL;
}

static void *ntp_sender(void *arg) {
    // we inherit the signal mask (SIGUSR1)
    int i = 0;
    int cc;
    struct timespec tv;
    char req[32];
    memset(req, 0, sizeof(req));

    while (1) {
        // at startup, we send more timing request to fill up the cache
        if (please_shutdown)
            break;

        req[0] = 0x80;
        req[1] = 0x52|0x80;  // Apple 'ntp request'
        *(uint16_t *)(req+2) = htons(7);  // seq no, needs to be 7 or iTunes won't respond

        get_current_time(&tv);
        *(uint32_t *)(req+24) = htonl((uint32_t)tv.tv_sec);
        *(uint32_t *)(req+28) = htonl((uint32_t)tv.tv_nsec * 0x100000000 / (1000 * 1000 * 1000));

        cc = sendto(timing_sock, req, sizeof(req), 0, (struct sockaddr*)&rtp_timing, addrlen);
        if (cc < 0){
            debug(1, "send packet failed in send_timing_packet\n");
            die("error(%d)\n", errno);
        }
        debug(2, "Current time s:%lu us:%lu\n", (unsigned int) tv.tv_sec, (unsigned int) tv.tv_nsec / 1000);
        // todo: randomize time at which to send timing packets to avoid timing floods at the client
        if (i<2){
            i++;
            usleep(50000);
        } else
            sleep(3);
    }

    debug(1, "Time send thread interrupted. terminating.\n");

    return NULL;
}
static struct addrinfo *get_address_info(SOCKADDR *remote) {
    struct addrinfo hints, *info;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = remote->SAFAMILY;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;

    int ret = getaddrinfo(NULL, "0", &hints, &info);

    if (ret < 0)
        die("failed to get usable addrinfo?! %s", gai_strerror(ret));

    return info;
}

static int bind_port(struct addrinfo *info, int *sock) {
    int ret;

    if (sock == NULL)
        die("socket is NULL");
    *sock = socket(info->ai_family, SOCK_DGRAM, IPPROTO_UDP);
    ret = bind(*sock, info->ai_addr, info->ai_addrlen);

    if (ret < 0)
        die("could not bind a UDP port!");

    int sport;
    SOCKADDR local;
    socklen_t local_len = sizeof(local);
    getsockname(*sock, (struct sockaddr*)&local, &local_len);
#ifdef AF_INET6
    if (local.SAFAMILY == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)&local;
        sport = htons(sa6->sin6_port);
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)&local;
        sport = htons(sa->sin_port);
    }

    return sport;
}

int rtp_setup(SOCKADDR *remote, int *cport, int *tport) {
    // we take the client's cport and tport as input and overwrite them with our own
    // we only create two sockets instead of three, combining control and data
    // allows for one, simpler rtp receive thread
    int server_port;
    struct addrinfo *info;

    if (running)
        die("rtp_setup called with active stream!");

    memcpy(&rtp_client, remote, sizeof(rtp_client));
    memcpy(&rtp_timing, remote, sizeof(rtp_timing));
#ifdef AF_INET6
    if (rtp_client.SAFAMILY == AF_INET6) {
        struct sockaddr_in6 *sa6 = (struct sockaddr_in6*)&rtp_client;
        sa6->sin6_port = htons(*cport);
        struct sockaddr_in6 *sa6_t = (struct sockaddr_in6*)&rtp_timing;
        sa6_t->sin6_port = htons(*tport);
    } else
#endif
    {
        struct sockaddr_in *sa = (struct sockaddr_in*)&rtp_client;
        sa->sin_port = htons(*cport);
        struct sockaddr_in *sa_t = (struct sockaddr_in*)&rtp_timing;
        sa_t->sin_port = htons(*tport);
    }

    // since we create sockets all alike the remote's, the address length
    // is equal for all
    info = get_address_info(remote);
    addrlen = info->ai_addrlen;

    *cport = bind_port(info, &server_sock);
    server_port = *cport;
    *tport = bind_port(info, &timing_sock);
    freeaddrinfo(info);
    debug(1, "Rtp listening on dataport %d, controlport %d. Timing port is %d.\n", server_port, *cport, *tport);

    reset_ntp_cache();

    please_shutdown = 0;
    pthread_create(&rtp_thread, NULL, &rtp_receiver, NULL);
    pthread_create(&ntp_receive_thread, NULL, &ntp_receiver, NULL);
    pthread_create(&ntp_send_thread, NULL, &ntp_sender, NULL);

    running = 1;
    return server_port;
}

void rtp_shutdown(void) {
    if (!running)
        die("rtp_shutdown called without active stream!");

    debug(2, "shutting down RTP thread\n");
    please_shutdown = 1;
    pthread_kill(rtp_thread, SIGUSR1);
    pthread_kill(ntp_receive_thread, SIGUSR1);
    pthread_kill(ntp_send_thread, SIGUSR1);
    void *retval;
    pthread_join(rtp_thread, &retval);
    pthread_join(ntp_receive_thread, &retval);
    pthread_join(ntp_send_thread, &retval);
    close(server_sock);
    close(timing_sock);
    running = 0;
}

void rtp_request_resend(seq_t first, seq_t last) {
    int cc;
    if (!running)
        die("rtp_request_resend called without active stream!");

    debug(1, "requesting resend on %d packets (%04X:%04X)\n",
         seq_diff(first,last) + 1, first, last);

    char req[8];    // *not* a standard RTCP NACK
    req[0] = 0x80;
    req[1] = 0x55|0x80;  // Apple 'resend'
    *(unsigned short *)(req+2) = htons(1);  // our seqnum
    *(unsigned short *)(req+4) = htons(first);  // missed seqnum
    *(unsigned short *)(req+6) = htons(last-first+1);  // count

    cc = sendto(server_sock, req, sizeof(req), 0, (struct sockaddr*)&rtp_client, addrlen);
    if (cc < 0){
        debug(1, "send packet failed in rtp_request_resend\n");
        die("error(%d)\n", errno);
    }
}
