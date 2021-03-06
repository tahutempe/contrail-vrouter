/*
 * Copyright (C) 2014 Semihalf.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * vr_dpdk_host.c -- DPDK vrouter module
 *
 */

#include <stdarg.h>
#include <sys/user.h>
#include <linux/if_ether.h>

#include <netinet/ip.h>
#include <netinet/tcp.h>

#include <urcu-qsbr.h>

#include <rte_config.h>
#include <rte_malloc.h>
#include <rte_jhash.h>
#include <rte_timer.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_jhash.h>

#include "vr_dpdk.h"
#include "vr_sandesh.h"
#include "vr_proto.h"
#include "vr_hash.h"
#include "vr_fragment.h"

/* Max number of CPU */
unsigned int vr_num_cpus = RTE_MAX_LCORE;
/* Global init flag */
static bool vr_host_inited = false;

struct rcu_cb_data {
    struct rcu_head rcd_rcu;
    vr_defer_cb rcd_user_cb;
    struct vrouter *rcd_router;
    unsigned char rcd_user_data[0];
};

static void *
dpdk_page_alloc(unsigned int size)
{
    return rte_malloc(0, size, PAGE_SIZE);
}

static void
dpdk_page_free(void *address, unsigned int size)
{
    rte_free(address);
}

static int
dpdk_printf(const char *format, ...)
{
    va_list args;

    rte_log(RTE_LOG_INFO, RTE_LOGTYPE_VROUTER, "DPCORE: ");
    va_start(args, format);
    rte_vlog(RTE_LOG_INFO, RTE_LOGTYPE_VROUTER, format, args);
    va_end(args);

    return 0;
}

static void *
dpdk_malloc(unsigned int size)
{
    return rte_malloc(NULL, size, 0);
}

static void *
dpdk_zalloc(unsigned int size)
{
    return rte_calloc(NULL, size, 1, 0);
}

static void
dpdk_free(void *mem)
{
    rte_free(mem);
}

static uint64_t
dpdk_vtop(void *address)
{
    /* TODO: not used */
    rte_panic("%s: not used in DPDK mode\n", __func__);

    return (uint64_t)0;
}

static struct vr_packet *
dpdk_palloc(unsigned int size)
{
    struct rte_mbuf *m;

    /* in DPDK we have fixed-sized mbufs only */
    RTE_VERIFY(size < VR_DPDK_MAX_PACKET_SZ);
    m = rte_pktmbuf_alloc(vr_dpdk.rss_mempool);
    if (!m)
        return (NULL);

    return vr_dpdk_packet_get(m, NULL);
}

static struct vr_packet *
dpdk_palloc_head(struct vr_packet *pkt, unsigned int size)
{
    /* TODO: not implemented */
    fprintf(stderr, "%s: not implemented\n", __func__);
    return NULL;
}

static struct vr_packet *
dpdk_pexpand_head(struct vr_packet *pkt, unsigned int hspace)
{
    /* TODO: not implemented */
    return pkt;
}

static void
dpdk_pfree(struct vr_packet *pkt, unsigned short reason)
{
    struct vrouter *router = vrouter_get(0);

    if (!pkt)
        rte_panic("Null packet");

    if (router)
        ((uint64_t *)(router->vr_pdrop_stats[pkt->vp_cpu]))[reason]++;

    rte_pktmbuf_free(vr_dpdk_pkt_to_mbuf(pkt));

    return;
}

void
vr_dpdk_pfree(struct rte_mbuf *mbuf, unsigned short reason)
{
    dpdk_pfree(vr_dpdk_mbuf_to_pkt(mbuf), reason);
}


static void
dpdk_preset(struct vr_packet *pkt)
{
    struct rte_mbuf *m;

    if (!pkt)
        rte_panic("%s: NULL pkt", __func__);

    m = vr_dpdk_pkt_to_mbuf(pkt);

    /* Reset packet data */
    pkt->vp_data = rte_pktmbuf_headroom(m);
    pkt->vp_tail = rte_pktmbuf_headroom(m) + rte_pktmbuf_data_len(m);
    pkt->vp_len = rte_pktmbuf_data_len(m);

    return;
}

/**
 * Copy packet mbuf data to another packet mbuf.
*
 * @param dst
 *   The destination packet mbuf.
 * @param src
 *   The source packet mbuf.
 */

static inline void
dpdk_pktmbuf_copy_data(struct rte_mbuf *dst, struct rte_mbuf *src)
{
    dst->ol_flags = src->ol_flags;

    dst->pkt.next = NULL;
    dst->pkt.data_len = src->pkt.data_len;
    dst->pkt.nb_segs = 1;
    dst->pkt.in_port = src->pkt.in_port;
    dst->pkt.pkt_len = src->pkt.data_len;
    dst->pkt.vlan_macip = src->pkt.vlan_macip;
    dst->pkt.hash = src->pkt.hash;

    __rte_mbuf_sanity_check(dst, RTE_MBUF_PKT, 1);
    __rte_mbuf_sanity_check(src, RTE_MBUF_PKT, 0);

    /* copy data */
    rte_memcpy(dst->pkt.data, src->pkt.data, src->pkt.data_len);
}

/**
 * Creates a copy of the given packet mbuf.
 *
 * Walks through all segments of the given packet mbuf, and for each of them:
 *  - Creates a new packet mbuf from the given pool.
 *  - Copies data to the newly created mbuf.
 * Then updates pkt_len and nb_segs of the "copy" packet mbuf to match values
 * from the original packet mbuf.
 *
 * @param md
 *   The packet mbuf to be copied.
 * @param mp
 *   The mempool from which the "copy" mbufs are allocated.
 * @return
 *   - The pointer to the new "copy" mbuf on success.
 *   - NULL if allocation fails.
 */
static inline struct rte_mbuf *
dpdk_pktmbuf_copy(struct rte_mbuf *md,
        struct rte_mempool *mp)
{
    struct rte_mbuf *mc, *mi, **prev;
    uint32_t pktlen;
    uint8_t nseg;

    if (unlikely ((mc = rte_pktmbuf_alloc(mp)) == NULL))
        return (NULL);

    mi = mc;
    prev = &mi->pkt.next;
    pktlen = md->pkt.pkt_len;
    nseg = 0;

    do {
        nseg++;
        dpdk_pktmbuf_copy_data(mi, md);
        *prev = mi;
        prev = &mi->pkt.next;
    } while ((md = md->pkt.next) != NULL &&
        (mi = rte_pktmbuf_alloc(mp)) != NULL);

    *prev = NULL;
    mc->pkt.nb_segs = nseg;
    mc->pkt.pkt_len = pktlen;

    /* Allocation of new indirect segment failed */
    if (unlikely (mi == NULL)) {
        rte_pktmbuf_free(mc);
        return (NULL);
    }

    __rte_mbuf_sanity_check(mc, RTE_MBUF_PKT, 1);
    return (mc);
}

/* VRouter callback */
static struct vr_packet *
dpdk_pclone(struct vr_packet *pkt)
{
    /* TODO: pktmbuf_clone version should be faster, but at the moment
     *       vr_dpdk_mbuf_to_pkt returns original vr_packet structure
     */
#if 0
    struct rte_mbuf *m, *m_clone;
    struct vr_packet *pkt_clone;

    m = vr_dpdk_pkt_to_mbuf(pkt);

    m_clone = rte_pktmbuf_clone(m, vr_dpdk.rss_mempool);
    if (!m_clone)
        return NULL;

    /* clone vr_packet data */
    pkt_clone = vr_dpdk_mbuf_to_pkt(m_clone);
    /* TODO: vr_dpdk_mbuf_to_pkt never returns pointer to the vr_packet copy */
    *pkt_clone = *pkt;

    return pkt_clone;
#else
    /* if no scatter/gather enabled -> just copy the mbuf */
    struct rte_mbuf *m, *m_copy;
    struct vr_packet *pkt_copy;

    m = vr_dpdk_pkt_to_mbuf(pkt);

    m_copy = dpdk_pktmbuf_copy(m, vr_dpdk.rss_mempool);
    if (!m_copy)
        return NULL;

    /* copy vr_packet data */
    pkt_copy = vr_dpdk_mbuf_to_pkt(m_copy);
    *pkt_copy = *pkt;
    /* set head pointer to a copy */
    pkt_copy->vp_head = m_copy->buf_addr;

    return pkt_copy;
#endif
}

/* Copy the specified number of bytes from the source mbuf to the
 * destination buffer.
 */
static int
dpdk_pktmbuf_copy_bits(const struct rte_mbuf *mbuf, int offset,
    void *to, int len)
{
    /* how many bytes to copy in loop */
    int copy = 0;
    /* loop pointer to a source data */
    void *from;

    /* check total packet length */
    if (unlikely(offset > (int)rte_pktmbuf_pkt_len(mbuf) - len))
        goto fault;

    do {
        if (offset < rte_pktmbuf_data_len(mbuf)) {
            /* copy a piece of data */
            from = (void *)(rte_pktmbuf_mtod(mbuf, uintptr_t) + offset);
            copy = rte_pktmbuf_data_len(mbuf) - offset;
            if (copy > len)
                copy = len;
            rte_memcpy(to, from, copy);
            offset = 0;
        } else {
            offset -= rte_pktmbuf_data_len(mbuf);
        }
        /* get next mbuf */
        to += copy;
        len -= copy;
        mbuf = mbuf->pkt.next;
    } while (unlikely(len > 0 && NULL != mbuf));

    if (likely(0 == len))
        return 0;

fault:
    return -EFAULT;
}

/* VRouter callback */
static int
dpdk_pcopy(unsigned char *dst, struct vr_packet *p_src,
    unsigned int offset, unsigned int len)
{
    int ret;
    struct rte_mbuf *src;

    src = vr_dpdk_pkt_to_mbuf(p_src);
    ret = dpdk_pktmbuf_copy_bits(src, offset, dst, len);
    if (ret)
        return ret;

    return len;
}


static unsigned short
dpdk_pfrag_len(struct vr_packet *pkt)
{
    struct rte_mbuf *m;

    m = vr_dpdk_pkt_to_mbuf(pkt);

    return rte_pktmbuf_pkt_len(m) - rte_pktmbuf_data_len(m);
}

static unsigned short
dpdk_phead_len(struct vr_packet *pkt)
{
    struct rte_mbuf *m;

    m = vr_dpdk_pkt_to_mbuf(pkt);

    return rte_pktmbuf_data_len(m);
}

static void
dpdk_pset_data(struct vr_packet *pkt, unsigned short offset)
{
    struct rte_mbuf *m;

    m = vr_dpdk_pkt_to_mbuf(pkt);
    m->pkt.data = pkt->vp_head + offset;

    return;
}

static unsigned int
dpdk_get_cpu(void)
{
    return rte_lcore_id();
}

/* DPDK timer callback */
static void
dpdk_timer(struct rte_timer *tim, void *arg)
{
    struct vr_timer *vtimer = (struct vr_timer*)arg;

    vtimer->vt_timer(vtimer->vt_vr_arg);
}

static int
dpdk_create_timer(struct vr_timer *vtimer)
{
    struct rte_timer *timer;
    uint64_t hz, ticks;

    timer = rte_zmalloc("vr_dpdk_timer", sizeof(struct rte_timer), 0);

    if (!timer) {
        RTE_LOG(ERR, VROUTER, "Error allocating RTE timer\n");
        return -1;
    }

    /* init timer */
    rte_timer_init(timer);
    vtimer->vt_os_arg = (void *)timer;

    /* reset timer */
    hz = rte_get_timer_hz();
    ticks = hz * vtimer->vt_msecs / 1000;
    if (rte_timer_reset(timer, ticks, PERIODICAL, rte_get_master_lcore(),
        dpdk_timer, vtimer) == -1) {
        RTE_LOG(ERR, VROUTER, "Error resetting timer\n");
        rte_free(timer);

        return -1;
    }

    return 0;
}

static void
dpdk_delete_timer(struct vr_timer *vtimer)
{
    struct rte_timer *timer = (struct rte_timer*)vtimer->vt_os_arg;

    if (timer) {
        rte_timer_stop_sync(timer);
        rte_free(timer);
    } else {
        RTE_LOG(ERR, VROUTER, "No timer to delete\n");
    }
}

static void
dpdk_get_time(unsigned int *sec, unsigned int *nsec)
{
    struct timespec ts;

    *sec = *nsec = 0;
    if (-1 == clock_gettime(CLOCK_REALTIME, &ts))
        return;

    *sec = ts.tv_sec;
    *nsec = ts.tv_nsec;

    return;
}

static void
dpdk_get_mono_time(unsigned int *sec, unsigned int *nsec)
{
    struct timespec ts;

    *sec = *nsec = 0;
    if (-1 == clock_gettime(CLOCK_MONOTONIC, &ts))
        return;

    *sec = ts.tv_sec;
    *nsec = ts.tv_nsec;

    return;
}

static void
dpdk_work_timer(struct rte_timer *timer, void *arg)
{
    struct vr_timer *vtimer = (struct vr_timer *)arg;

    dpdk_timer(timer, arg);

    dpdk_delete_timer(vtimer);
    dpdk_free(vtimer);

    return;
}

static void
dpdk_schedule_work(unsigned int cpu, void (*fn)(void *), void *arg)
{
    struct rte_timer *timer;
    struct vr_timer *vtimer;

    timer = dpdk_malloc(sizeof(struct rte_timer));
    if (!timer) {
        RTE_LOG(ERR, VROUTER, "Error allocating RTE timer\n");
        return;
    }

    vtimer = dpdk_malloc(sizeof(*vtimer));
    if (!vtimer) {
        dpdk_free(timer);
        RTE_LOG(ERR, VROUTER, "Error allocating VR timer for work\n");
        return;
    }
    vtimer->vt_timer = fn;
    vtimer->vt_vr_arg = arg;
    vtimer->vt_os_arg = timer;
    vtimer->vt_msecs = 1;

    rte_timer_init(timer);

    RTE_LOG(DEBUG, VROUTER, "%s[%u]: reset timer %p REINJECTING: lcore_id %u\n",
            __func__, rte_lcore_id(), timer, vr_dpdk.packet_lcore_id);
    /* schedule task to pkt0 lcore */
    if (rte_timer_reset(timer, 0, SINGLE, vr_dpdk.packet_lcore_id,
        dpdk_work_timer, vtimer) == -1) {
        RTE_LOG(ERR, VROUTER, "Error resetting timer\n");
        rte_free(timer);
        rte_free(vtimer);

        return;
    }

    /* wake up pkt0 lcore */
    vr_dpdk_packet_wakeup(vr_dpdk.lcores[vr_dpdk.packet_lcore_id]);
    return;
}

static void
dpdk_delay_op(void)
{
    synchronize_rcu();

    return;
}

static void
rcu_cb(struct rcu_head *rh)
{
    struct rcu_cb_data *cb_data = (struct rcu_cb_data *)rh;

    /* Call the user call back */
    cb_data->rcd_user_cb(cb_data->rcd_router, cb_data->rcd_user_data);
    dpdk_free(cb_data);

    return;
}

static void
dpdk_defer(struct vrouter *router, vr_defer_cb user_cb, void *data)
{
    struct rcu_cb_data *cb_data;

    cb_data = CONTAINER_OF(rcd_user_data, struct rcu_cb_data, data);
    cb_data->rcd_user_cb = user_cb;
    cb_data->rcd_router = router;
    call_rcu(&cb_data->rcd_rcu, rcu_cb);

    return;
}

static void *
dpdk_get_defer_data(unsigned int len)
{
    struct rcu_cb_data *cb_data;

    if (!len)
        return NULL;

    cb_data = dpdk_malloc(sizeof(*cb_data) + len);
    if (!cb_data) {
        return NULL;
    }

    return cb_data->rcd_user_data;
}

static void
dpdk_put_defer_data(void *data)
{
    struct rcu_cb_data *cb_data;

    if (!data)
        return;

    cb_data = CONTAINER_OF(rcd_user_data, struct rcu_cb_data, data);
    dpdk_free(cb_data);

    return;
}

static void *
dpdk_network_header(struct vr_packet *pkt)
{
    if (pkt->vp_network_h < pkt->vp_end)
        return pkt->vp_head + pkt->vp_network_h;

    /* TODO: for buffer chain? */
    rte_panic("%s: buffer chain not supported\n", __func__);

    return NULL;
}

static void *
dpdk_inner_network_header(struct vr_packet *pkt)
{
    /* TODO: not used? */
    rte_panic("%s: not implemented\n", __func__);

    return NULL;
}

static void *
dpdk_data_at_offset(struct vr_packet *pkt, unsigned short off)
{
    if (off < pkt->vp_end)
        return pkt->vp_head + off;

    /* TODO: for buffer chain? */
    rte_panic("%s: buffer chain not supported\n", __func__);

    return NULL;
}

/*
 * dpdk_pheader_pointer
 * return pointer to data at pkt->vp_data offset if hdr_len bytes
 * in continuous memory, otherwise copy data to buf
 */
static void *
dpdk_pheader_pointer(struct vr_packet *pkt, unsigned short hdr_len, void *buf)
{
    struct rte_mbuf *m;
    int offset;

    m = vr_dpdk_pkt_to_mbuf(pkt);

    /*
     * vp_data is offset from start of buffer,
     * so first calculate offset from start of mbuf payload
     */
    offset = pkt->vp_data - rte_pktmbuf_headroom(m);
    if ((offset + hdr_len) < rte_pktmbuf_data_len(m))
        return (void *)((uintptr_t)m->buf_addr + pkt->vp_data);
    else {
        int len = rte_pktmbuf_data_len(m) - offset;
        void *tmp_buf = buf;

        rte_memcpy(tmp_buf, rte_pktmbuf_mtod(m, char *) + offset, len);
        hdr_len -= len;
        tmp_buf = (void *)((uintptr_t)tmp_buf + len);

        /* iterate thru buffers chain */
        while (hdr_len) {
            m = m->pkt.next;
            if (!m)
                return (NULL);
            if (hdr_len > rte_pktmbuf_data_len(m))
                len = rte_pktmbuf_data_len(m);
            else
                len = hdr_len;

            rte_memcpy(tmp_buf, rte_pktmbuf_mtod(m, void *), len);

            tmp_buf = (void *)((uintptr_t)tmp_buf + len);
            hdr_len -= len;
        }

        return (buf);
    }
}

/* VRouter callback */
static int
dpdk_pcow(struct vr_packet *pkt, unsigned short head_room)
{
    struct rte_mbuf *mbuf = vr_dpdk_pkt_to_mbuf(pkt);

    /* Store the right values to mbuf */
    mbuf->pkt.data = pkt_data(pkt);
    mbuf->pkt.pkt_len = pkt_len(pkt);
    mbuf->pkt.data_len = pkt_head_len(pkt);

    if (head_room > rte_pktmbuf_headroom(mbuf)) {
        return -ENOMEM;
    }

    return 0;
}

/*
 * dpdk_get_udp_src_port - return a source port for the outer UDP header.
 * The source port is based on a hash of the inner IP source/dest addresses,
 * vrf (and inner TCP/UDP ports in the future). The label from fmd
 * will be used in the future to detect whether it is a L2/L3 packet.
 * Returns 0 on error, valid source port otherwise.
 *
 * Based on linux/vrouter_mod.c:lh_get_udp_src_port
 * Copyright (c) 2013, 2014 Juniper Networks, Inc.
 */
static uint16_t
dpdk_get_udp_src_port(struct vr_packet *pkt, struct vr_forwarding_md *fmd,
    unsigned short vrf)
{
    struct rte_mbuf *mbuf = vr_dpdk_pkt_to_mbuf(pkt);
    unsigned int pull_len;
    uint32_t ip_src, ip_dst, hashval, port_range;
    struct vr_ip *iph;
    uint16_t port;
    uint16_t sport = 0, dport = 0;
    struct vr_fragment *frag;
    struct vrouter *router = vrouter_get(0);
    uint32_t hash_key[5];
    uint16_t *l4_hdr;
    struct vr_flow_entry *fentry;

    if (hashrnd_inited == 0) {
        vr_hashrnd = random();
        hashrnd_inited = 1;
    }

    if (pkt->vp_type == VP_TYPE_IP) {
        /* Ideally the below code is only for VP_TYPE_IP and not
         * for IP6. But having explicit check for IP only break IP6
         */
        pull_len = sizeof(struct iphdr);
        pull_len += pkt_get_network_header_off(pkt);
        pull_len -= rte_pktmbuf_headroom(mbuf);

        /* It's safe to assume the ip hdr is within this mbuf, so we skip
         * all the header checks.
         */

        iph = (struct vr_ip *)(mbuf->buf_addr + pkt_get_network_header_off(pkt));
        if (vr_ip_transport_header_valid(iph)) {
            if ((iph->ip_proto == VR_IP_PROTO_TCP) ||
                        (iph->ip_proto == VR_IP_PROTO_UDP)) {
                l4_hdr = (__u16 *) (((char *) iph) + (iph->ip_hl * 4));
                sport = *l4_hdr;
                dport = *(l4_hdr+1);
            }
        } else {
            /*
             * If this fragment required flow lookup, get the source and
             * dst port from the frag entry. Otherwise, use 0 as the source
             * dst port (which could result in fragments getting a different
             * outer UDP source port than non-fragments in the same flow).
             */
            frag = vr_fragment_get(router, vrf, iph);
            if (frag) {
                sport = frag->f_sport;
                dport = frag->f_dport;
            }
        }

        if (fmd && fmd->fmd_flow_index >= 0) {
            fentry = vr_get_flow_entry(router, fmd->fmd_flow_index);
            if (fentry) {
                vr_dpdk_mbuf_reset(pkt);
                return fentry->fe_udp_src_port;
            }
        }

        ip_src = iph->ip_saddr;
        ip_dst = iph->ip_daddr;

        hash_key[0] = ip_src;
        hash_key[1] = ip_dst;
        hash_key[2] = vrf;
        hash_key[3] = sport;
        hash_key[4] = dport;

        hashval = rte_jhash(hash_key, 20, vr_hashrnd);
        vr_dpdk_mbuf_reset(pkt);
    } else {

        /* We treat all non-ip packets as L2 here. For V6 we can extract
         * the required fieleds explicity and manipulate the src port
         */

        if (pkt_head_len(pkt) < ETH_HLEN)
            goto error;

        hashval = vr_hash(pkt_data(pkt), ETH_HLEN, vr_hashrnd);
        /* Include the VRF to calculate the hash */
        hashval = vr_hash_2words(hashval, vrf, vr_hashrnd);
    }


    /*
     * Convert the hash value to a value in the port range that we want
     * for dynamic UDP ports
     */
    port_range = VR_MUDP_PORT_RANGE_END - VR_MUDP_PORT_RANGE_START;
    port = (uint16_t) (((uint64_t) hashval * port_range) >> 32);

    if (port > port_range) {
        /*
         * Shouldn't happen...
         */
        port = 0;
    }

    return (port + VR_MUDP_PORT_RANGE_START);

error:
    vr_dpdk_mbuf_reset(pkt);
    return 0;
}

static void
dpdk_adjust_tcp_mss(struct tcphdr *tcph, struct rte_mbuf *m, unsigned short overlay_len)
{
    int opt_off = sizeof(struct tcphdr);
    u_int8_t *opt_ptr = (u_int8_t *) tcph;
    u_int16_t pkt_mss, max_mss, mtu;
    unsigned int csum;
    uint8_t port_id;
    struct vrouter *router = vrouter_get(0);

    if ((tcph == NULL) || !(tcph->syn) || (router == NULL))
        return;

    if (router->vr_eth_if == NULL)
        return;

    while (opt_off < (tcph->doff * 4)) {
        switch (opt_ptr[opt_off]) {
        case TCPOPT_EOL:
            return;

        case TCPOPT_NOP:
            opt_off++;
            continue;

        case TCPOPT_MAXSEG:
            if ((opt_off + TCPOLEN_MAXSEG) > (tcph->doff*4))
                return;

            if (opt_ptr[opt_off+1] != TCPOLEN_MAXSEG)
                return;

            pkt_mss = (opt_ptr[opt_off+2] << 8) | opt_ptr[opt_off+3];
            if (router->vr_eth_if == NULL)
                return;

            port_id = (((struct vr_dpdk_ethdev *)(router->vr_eth_if->vif_os))->
                    ethdev_port_id);
            rte_eth_dev_get_mtu(port_id, &mtu);

            max_mss = mtu - (overlay_len + sizeof(struct vr_ip) +
                sizeof(struct tcphdr));

            if (pkt_mss > max_mss) {
                opt_ptr[opt_off+2] = (max_mss & 0xff00) >> 8;
                opt_ptr[opt_off+3] = max_mss & 0xff;

                /* Recalculate checksum */
                csum = (unsigned short)(~ntohs(tcph->check));
                csum = csum + (unsigned short)~pkt_mss;
                csum = (csum & 0xffff) + (csum >> 16);
                csum += max_mss;
                csum = (csum & 0xffff) + (csum >> 16);
                tcph->check = htons(~((unsigned short)csum));
            }
            return;

        default:
            if ((opt_off + 1) == (tcph->doff*4))
            return;

            if (opt_ptr[opt_off+1])
                opt_off += opt_ptr[opt_off+1];
            else
                opt_off++;

            continue;
        } /* switch */
    } /* while */

    return;
}

/*
 * dpdk_pkt_from_vm_tcp_mss_adj - perform TCP MSS adjust, if required, for packets
 * that are sent by a VM. Returns 0 on success, non-zero otherwise.
 */
static int
dpdk_pkt_from_vm_tcp_mss_adj(struct vr_packet *pkt, unsigned short overlay_len)
{
    struct rte_mbuf *m;
    struct vr_ip *iph;
    struct tcphdr *tcph;
    int offset;

    m = vr_dpdk_pkt_to_mbuf(pkt);

    /* check if whole ip header is in the packet */
    offset = sizeof(struct vr_ip);
    if (pkt->vp_data + offset < pkt->vp_end)
        iph = (struct vr_ip *) ((uintptr_t)m->buf_addr + pkt->vp_data);
    else
        rte_panic("%s: ip header not in first buffer\n", __func__);

    if (iph->ip_proto != VR_IP_PROTO_TCP)
        goto out;

    /*
     * If this is a fragment and not the first one, it can be ignored
     */
    if (iph->ip_frag_off & htons(IP_OFFMASK))
        goto out;


    /*
     * Now we know exact ip header length,
     * check if whole tcp header is also in the packet
     */
    offset = (iph->ip_hl * 4) + sizeof(struct tcphdr);

    if (pkt->vp_data + offset < pkt->vp_end)
        tcph = (struct tcphdr *) ((char *) iph + (iph->ip_hl * 4));
    else
        rte_panic("%s: tcp header not in first buffer\n", __func__);

    if ((tcph->doff << 2) <= (sizeof(struct tcphdr))) {
        /*Nothing to do if there are no TCP options */
        goto out;
    }


    offset += (tcph->doff << 2) - sizeof(struct tcphdr);
    if (pkt->vp_data + offset > pkt->vp_end)
        rte_panic("%s: tcp header outside first buffer\n", __func__);


    dpdk_adjust_tcp_mss(tcph, m, overlay_len);

out:
    return 0;
}

static unsigned int
dpdk_pgso_size(struct vr_packet *pkt)
{
    /* TODO: not implemented */
    return 0;
}

static void
dpdk_add_mpls(struct vrouter *router, unsigned mpls_label)
{
    int ret, i;
    struct vr_interface *eth_vif;

    for (i = 0; i < router->vr_max_interfaces; i++) {
        eth_vif = __vrouter_get_interface(router, i);
        if (eth_vif && (eth_vif->vif_type == VIF_TYPE_PHYSICAL)
            && (eth_vif->vif_flags & VIF_FLAG_FILTERING_OFFLOAD)) {
            RTE_LOG(INFO, VROUTER, "Enabling hardware acceleration on vif %u for MPLS %u\n",
                (unsigned)eth_vif->vif_idx, mpls_label);
            if (!eth_vif->vif_ip) {
                RTE_LOG(ERR, VROUTER, "\terror accelerating MPLS %u: no IP address set\n",
                    mpls_label);
                continue;
            }
            ret = vr_dpdk_lcore_mpls_schedule(eth_vif, eth_vif->vif_ip, mpls_label);
            if (ret != 0)
                RTE_LOG(INFO, VROUTER, "\terror accelerating MPLS %u: %s (%d)\n",
                    mpls_label, rte_strerror(-ret), -ret);
        }
    }

}

static void
dpdk_del_mpls(struct vrouter *router, unsigned mpls_label)
{
    /* TODO: not implemented */
}

static int
dpdk_pkt_may_pull(struct vr_packet *pkt, unsigned int len)
{
    struct rte_mbuf *mbuf = vr_dpdk_pkt_to_mbuf(pkt);

    if (len > rte_pktmbuf_data_len(mbuf))
        return -1;

    vr_dpdk_mbuf_reset(pkt);
    return 0;
}


struct host_os dpdk_host = {
    .hos_printf                     =    dpdk_printf,
    .hos_malloc                     =    dpdk_malloc,
    .hos_zalloc                     =    dpdk_zalloc,
    .hos_free                       =    dpdk_free,
    .hos_vtop                       =    dpdk_vtop, /* not used */
    .hos_page_alloc                 =    dpdk_page_alloc,
    .hos_page_free                  =    dpdk_page_free,

    .hos_palloc                     =    dpdk_palloc,
    .hos_palloc_head                =    dpdk_palloc_head, /* not implemented */
    .hos_pexpand_head               =    dpdk_pexpand_head, /* not implemented */
    .hos_pfree                      =    dpdk_pfree,
    .hos_preset                     =    dpdk_preset,
    .hos_pclone                     =    dpdk_pclone,
    .hos_pcopy                      =    dpdk_pcopy,
    .hos_pfrag_len                  =    dpdk_pfrag_len,
    .hos_phead_len                  =    dpdk_phead_len,
    .hos_pset_data                  =    dpdk_pset_data,
    .hos_pgso_size                  =    dpdk_pgso_size, /* not implemented, returns 0 */

    .hos_get_cpu                    =    dpdk_get_cpu,
    .hos_schedule_work              =    dpdk_schedule_work,
    .hos_delay_op                   =    dpdk_delay_op, /* do nothing */
    .hos_defer                      =    dpdk_defer, /* for mirroring? */
    .hos_get_defer_data             =    dpdk_get_defer_data, /* for mirroring? */
    .hos_put_defer_data             =    dpdk_put_defer_data, /* for mirroring? */
    .hos_get_time                   =    dpdk_get_time,
    .hos_get_mono_time              =    dpdk_get_mono_time,
    .hos_create_timer               =    dpdk_create_timer,
    .hos_delete_timer               =    dpdk_delete_timer,

    .hos_network_header             =    dpdk_network_header, /* for chains? */
    .hos_inner_network_header       =    dpdk_inner_network_header, /* not used? */
    .hos_data_at_offset             =    dpdk_data_at_offset, /* for chains? */
    .hos_pheader_pointer            =    dpdk_pheader_pointer,
    .hos_pull_inner_headers         =    NULL,  /* not necessary */
    .hos_pcow                       =    dpdk_pcow,
    .hos_pull_inner_headers_fast    =    NULL,  /* not necessary */
#if VR_DPDK_USE_MPLS_UDP_ECMP
    .hos_get_udp_src_port           =    dpdk_get_udp_src_port,
#endif
    .hos_pkt_from_vm_tcp_mss_adj    =    dpdk_pkt_from_vm_tcp_mss_adj,
    .hos_pkt_may_pull               =    dpdk_pkt_may_pull,

    .hos_add_mpls                   =    dpdk_add_mpls,
    .hos_del_mpls                   =    dpdk_del_mpls, /* not implemented */
};

struct host_os *
vrouter_get_host(void)
{
    return &dpdk_host;
}

/* Remove xconnect callback */
void
vhost_remove_xconnect(void)
{
    int i;
    struct vr_interface *vif;

    for (i = 0; i < VR_MAX_INTERFACES; i++) {
        vif = vr_dpdk.vhosts[i];
        if (vif != NULL) {
            vif_remove_xconnect(vif);
            if (vif->vif_bridge != NULL)
                vif_remove_xconnect(vif->vif_bridge);
        }
    }
}

/* Implementation of the Linux kernel function */
void
get_random_bytes(void *buf, int nbytes)
{
    int i;

    if (nbytes == sizeof(uint32_t)) {
        *(uint32_t *)buf = (uint32_t)rte_rand();
    } else if (nbytes == sizeof(uint64_t)) {
        *(uint64_t *)buf = rte_rand();
    } else {
        for (i = 0; i < nbytes; i++) {
            *((uint8_t *)buf + i) = (uint8_t)rte_rand();
        }
    }
}

uint32_t
jhash(void *key, uint32_t length, uint32_t initval)
{
    return rte_jhash(key, length, initval);
}



/* Convert internal packet fields
 * Based on linux_get_packet()
 */
struct vr_packet *
vr_dpdk_packet_get(struct rte_mbuf *m, struct vr_interface *vif)
{
    struct vr_packet *pkt = vr_dpdk_mbuf_to_pkt(m);
    pkt->vp_cpu = vr_get_cpu();
    /* vp_head is set in vr_dpdk_pktmbuf_init() */

    pkt->vp_tail = rte_pktmbuf_headroom(m) + rte_pktmbuf_data_len(m);
    pkt->vp_data = rte_pktmbuf_headroom(m);
    /* vp_end is set in vr_dpdk_pktmbuf_init() */

    pkt->vp_len = rte_pktmbuf_data_len(m);
    pkt->vp_if = vif;
    pkt->vp_network_h = pkt->vp_inner_network_h = 0;
    pkt->vp_nh = NULL;
    pkt->vp_flags = 0;
    if (likely(m->ol_flags & PKT_TX_IP_CKSUM))
        pkt->vp_flags |= VP_FLAG_CSUM_PARTIAL;

    pkt->vp_ttl = 64;
    pkt->vp_type = VP_TYPE_NULL;

    return pkt;
}

/* Exit vRouter */
void
vr_dpdk_host_exit(void)
{
    vr_sandesh_exit();
    vrouter_exit(false);

    return;
}

/* Init vRouter */
int
vr_dpdk_host_init(void)
{
    int ret;

    if (vr_host_inited)
        return 0;

    if (!vrouter_host) {
        vrouter_host = vrouter_get_host();
        if (vr_dpdk_flow_init()) {
            return -1;
        }
    }

    /*
     * Turn off GRO/GSO as they are not implemented with DPDK.
     */
    vr_perfr = vr_perfs = 0;

    ret = vrouter_init();
    if (ret)
        return ret;

    ret = vr_sandesh_init();
    if (ret)
        goto init_fail;

    vr_host_inited = true;

    return 0;

init_fail:
    vr_dpdk_host_exit();
    return ret;
}

/* Retry socket connection */
int
vr_dpdk_retry_connect(int sockfd, const struct sockaddr *addr,
                        socklen_t alen)
{
    int nsec;

    for (nsec = 1; nsec < VR_DPDK_RETRY_CONNECT_SECS; nsec <<= 1) {
        if (connect(sockfd, addr, alen) == 0)
            return 0;

        if (nsec < VR_DPDK_RETRY_CONNECT_SECS/2) {
            sleep(nsec);
            RTE_LOG(INFO, VROUTER, "Retrying connection for socket %d...\n",
                    sockfd);
        }
    }

    return -1;
}

/* Returns a string hash */
static inline uint32_t
dpdk_strhash(const char *k, uint32_t initval)
{
    uint32_t a, b, c;

    a = b = RTE_JHASH_GOLDEN_RATIO;
    c = initval;

    do {
        if (*k) {
            a += k[0];
            k++;
        }
        if (*k) {
            b += k[0];
            k++;
        }
        if (*k) {
            c += k[0];
            k++;
        }
        __rte_jhash_mix(a, b, c);
    } while (*k);

    return c;
}

/* Generates unique log message */
int vr_dpdk_ulog(uint32_t level, uint32_t logtype, uint32_t *last_hash,
                    const char *format, ...)
{
    va_list ap;
    int ret = 0;
    uint32_t hash;
    char buf[256];

    /* fallback to rte_log */
    if (last_hash == NULL) {
        va_start(ap, format);
        ret = rte_log(level, logtype, "%s", buf);
        va_end(ap);
    } else {
        /* calculate message hash */
        va_start(ap, format);
        vsnprintf(buf, sizeof(buf) - 1, format, ap);
        va_end(ap);
        buf[sizeof(buf) - 1] = '\0';
        hash = dpdk_strhash(buf, level + logtype);

        if (hash != *last_hash) {
            *last_hash = hash;
            ret = rte_log(level, logtype, "%s", buf);
        }
    }

    return ret;
}
