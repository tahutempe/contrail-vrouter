/*
 * vr_dpdk_netlink.c -- message handling from agent
 *
 * Copyright (c) 2014, Juniper Networks Private Inc.,
 * All rights reserved
 */
#include <stdio.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/un.h>
#include <sys/stat.h>

#include <linux/netlink.h>
#include <linux/genetlink.h>

#include "vr_queue.h"
#include "vr_dpdk_usocket.h"
#include "vr_message.h"
#include "vr_dpdk.h"
#include "vr_genetlink.h"
#include "vr_uvhost.h"
#include "vr_uvhost_msg.h"

#define HDR_LEN (NLMSG_HDRLEN + GENL_HDRLEN + sizeof(struct nlattr))

struct nlmsghdr *dpdk_nl_message_hdr(struct vr_message *);
unsigned int dpdk_nl_message_len(struct vr_message *);

int vr_usocket_message_write(struct vr_usocket *, struct vr_message *);
int vr_nl_uvh_sock;

static void
dpdk_nl_process_response(void *usockp, struct nlmsghdr *nlh)
{
    __u32 seq;
    unsigned int multi_flag = 0;
    bool write = true;

    struct vr_message *resp;

    struct nlmsghdr *resp_nlh;
    struct genlmsghdr *genlh, *resp_genlh;
    struct nlattr *resp_nla;

    seq = nlh->nlmsg_seq;
    genlh = (struct genlmsghdr *)((unsigned char *)nlh + NLMSG_HDRLEN);

    /* Process responses */
    while ((resp = (struct vr_message *)vr_message_dequeue_response())) {
        if (!write) {
            vr_message_free(resp);
            continue;
        }

        if (!vr_response_queue_empty()) {
            multi_flag = NLM_F_MULTI;
        } else {
            multi_flag = 0;
        }

        resp->vr_message_len = RTE_ALIGN(resp->vr_message_len, 4);

        /* Update Netlink headers */
        resp_nlh = dpdk_nl_message_hdr(resp);
        resp_nlh->nlmsg_len = dpdk_nl_message_len(resp);
        resp_nlh->nlmsg_type = nlh->nlmsg_type;
        resp_nlh->nlmsg_flags = multi_flag;
        resp_nlh->nlmsg_seq = seq;
        resp_nlh->nlmsg_pid = 0;

        resp_genlh = (struct genlmsghdr *)((unsigned char *)resp_nlh +
                NLMSG_HDRLEN);
        memcpy(resp_genlh, genlh, sizeof(*genlh));

        resp_nla = (struct nlattr *)((unsigned char *)resp_genlh + GENL_HDRLEN);
        resp_nla->nla_len = resp->vr_message_len;
        resp_nla->nla_type = NL_ATTR_VR_MESSAGE_PROTOCOL;

        if (vr_usocket_message_write(usockp, resp) < 0) {
            write = false;
            vr_usocket_close(usockp);
        }
    }

    return;
}

int
dpdk_netlink_receive(void *usockp, char *nl_buf,
        unsigned int nl_len)
{
    struct vr_message request;

    request.vr_message_buf = nl_buf + HDR_LEN;
    request.vr_message_len = nl_len - HDR_LEN;
    vr_message_request(&request);

    dpdk_nl_process_response(usockp, (struct nlmsghdr *)nl_buf);

    return 0;
}

unsigned int
dpdk_nl_message_len(struct vr_message *message)
{
    return message->vr_message_len + HDR_LEN;
}

struct nlmsghdr *
dpdk_nl_message_hdr(struct vr_message *message)
{
    return (struct nlmsghdr *)(message->vr_message_buf - HDR_LEN);
}

static void
dpdk_nl_trans_free(char *buf)
{
    buf -= HDR_LEN;
    vr_free(buf);

    return;
}

static char *
dpdk_nl_trans_alloc(unsigned int size)
{
    char *buf;

    buf = vr_malloc(size + HDR_LEN);
    if (!buf)
        return NULL;

    return buf + HDR_LEN;
}

static struct vr_mtransport dpdk_nl_transport = {
    .mtrans_alloc       =       dpdk_nl_trans_alloc,
    .mtrans_free        =       dpdk_nl_trans_free,
};

/*
 * vr_netlink_uvhost_vif_del - sends a message to the user space vhost
 * thread when a vif is deleted. vif_idx is the index of the vif.
 *
 * Returns 0 on success, -1 otherwise.
 */
int
vr_netlink_uvhost_vif_del(unsigned int vif_idx)
{
    vrnu_msg_t msg;

    msg.vrnum_type = VRNU_MSG_VIF_DEL;
    msg.vrnum_vif_del.vrnu_vif_idx = vif_idx;

    /*
     * This is a blocking send.
     */
    if (send(vr_nl_uvh_sock, (void *) &msg, sizeof(msg), 0) !=
             sizeof(msg)) {
        RTE_LOG(ERR, VROUTER, "\terror deleting vif %u from user space vhost:"
            " %s (%d)\n", vif_idx, strerror(errno), errno);
        return -1;
    }

    return 0;
}

/*
 * vr_netlink_uvhost_vif_add - sends a message to the user space vhost
 * thread when a new vif is created. The name os the vif is specified in
 * the vif_name argument.
 *
 * Returns 0 on success, -1 otherwise.
 */
int
vr_netlink_uvhost_vif_add(char *vif_name, unsigned int vif_idx,
                          unsigned int vif_nrxqs, unsigned int vif_ntxqs)
{
    vrnu_msg_t msg;

    msg.vrnum_type = VRNU_MSG_VIF_ADD;
    strncpy(msg.vrnum_vif_add.vrnu_vif_name, vif_name,
            sizeof(msg.vrnum_vif_add.vrnu_vif_name) - 1);
    msg.vrnum_vif_add.vrnu_vif_idx = vif_idx;
    msg.vrnum_vif_add.vrnu_vif_nrxqs = vif_nrxqs;
    msg.vrnum_vif_add.vrnu_vif_ntxqs = vif_ntxqs;

    /*
     * This is a blocking send.
     */
    if (send(vr_nl_uvh_sock, (void *) &msg, sizeof(msg), 0) !=
             sizeof(msg)) {
        RTE_LOG(ERR, VROUTER, "\terror adding vif %u to user space vhost:"
            " %s (%d)\n", vif_idx, strerror(errno), errno);
        return -1;
    }

    return 0;
}

int
dpdk_netlink_io(void)
{
    RTE_LOG(DEBUG, VROUTER, "%s[%lx]: FD %d\n", __func__, pthread_self(),
                ((struct vr_usocket *)vr_dpdk.netlink_sock)->usock_fd);
    return vr_usocket_io(vr_dpdk.netlink_sock);
}

void
dpdk_netlink_exit(void)
{
    vr_message_transport_unregister(&dpdk_nl_transport);
    vr_usocket_close(vr_dpdk.netlink_sock);

    return;
}

/*
 * vr_nl_uvhost_connect - connect to the user space vhost server on a UNIX
 * domain socket.
 *
 * Returns 0 on success, error otherwise.
 */
static int
vr_nl_uvhost_connect(void)
{
    int s = 0, ret = -1, err;
    struct sockaddr_un nl_sun, uvh_sun;

    s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s == -1) {
        RTE_LOG(ERR, VROUTER, "\terror connecting to uvhost: %s (%d)\n",
                        strerror(errno), errno);
        goto error;
    }
    RTE_LOG(INFO, VROUTER, "\tuvhost socket FD is %d\n", s);

    memset(&nl_sun, 0, sizeof(nl_sun));
    nl_sun.sun_family = AF_UNIX;
    strncpy(nl_sun.sun_path, VR_NL_UVH_SOCK, sizeof(nl_sun.sun_path) - 1);

    mkdir(VR_SOCKET_DIR, VR_SOCKET_DIR_MODE);
    unlink(nl_sun.sun_path);
    ret = bind(s, (struct sockaddr *) &nl_sun, sizeof(nl_sun));
    if (ret == -1) {
        RTE_LOG(ERR, VROUTER, "\terror binding uvhost FD %d to %s: %s (%d)\n",
                        s, nl_sun.sun_path, strerror(errno), errno);
        goto error;
    }

    /*
     * This will block until the user space vhost thread listens on the
     * socket.
     */
    memset(&uvh_sun, 0, sizeof(uvh_sun));
    uvh_sun.sun_family = AF_UNIX;
    strncpy(uvh_sun.sun_path, VR_UVH_NL_SOCK, sizeof(uvh_sun.sun_path) - 1);
    ret = vr_dpdk_retry_connect(s, (struct sockaddr *) &uvh_sun, sizeof(uvh_sun));
    if (ret == -1) {
        RTE_LOG(ERR, VROUTER, "\terror connecting uvhost socket FD %d to %s: %s (%d)\n",
                        s, uvh_sun.sun_path, strerror(errno), errno);
        goto error;
    }

    vr_nl_uvh_sock = s;

    return 0;

error:

    err = errno;
    if (s > 0) {
        close(s);
    }
    errno = err;

    return ret;
}

int
dpdk_netlink_init(void)
{
    int ret;

    RTE_LOG(INFO, VROUTER, "Starting NetLink...\n");
    ret = vr_message_transport_register(&dpdk_nl_transport);
    if (ret)
        return ret;

    vr_dpdk.netlink_sock = vr_usocket(NETLINK, TCP);
    if (!vr_dpdk.netlink_sock) {
        RTE_LOG(ERR, VROUTER, "\terror creating NetLink server socket: %s (%d)\n",
                strerror(errno), errno);
        return -1;
    }

    ret = vr_nl_uvhost_connect();
    if (ret != 0) {
        vr_message_transport_unregister(&dpdk_nl_transport);
        vr_usocket_close(vr_dpdk.netlink_sock);

        RTE_LOG(ERR, VROUTER, "\terror creating uvhost connection\n");
        return -1;
    }

    if (rte_lcore_count() == VR_DPDK_MIN_LCORES) {
        RTE_LOG(INFO, VROUTER, "\tsetting NetLink socket to non-blocking\n");
        vr_usocket_non_blocking(vr_dpdk.netlink_sock);
    }

    return 0;
}
