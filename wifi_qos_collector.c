/*
 * wifi_qos_collector.c  â  continuous WiâFi QoS publisher
 *
 * Build:
 *   gcc -Wall -O2 wifi_qos_collector.c \
 *       $(pkg-config --cflags --libs libnl-3.0 libnl-genl-3.0) \
 *       -o /usr/local/bin/wifi_qos_collector
 *
 * Usage:
 *   sudo wifi_qos_collector [-i <ms>] <iface> [peer-mac]
 *
 *   -i <ms>   heartbeat interval (default 1000â¯ms). 0 = disable timer.
 *
 * Destination socket:
 *   $QOS_SOCK overrides, otherwise /run/user/<uid>/wifi_qos.sock
 *
 * Â©â¯2025 0âBSD / public domain.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define _LINUX_IF_H             /* prevent duplicate enums */
#include <linux/wireless.h>
#include <linux/nl80211.h>

#include <libnl3/netlink/genl/ctrl.h>
#include <libnl3/netlink/genl/genl.h>
#include <libnl3/netlink/netlink.h>

/* -------- wire format (24â¯bytes) -------------------------------------- */
struct wifi_qos_msg {
    uint64_t ts_ns;     /* CLOCK_REALTIME in ns */
    int32_t  rssi_dbm;  /* signed */
    uint32_t tx_ok;
    uint32_t tx_retry;
    uint32_t tx_fail;
} __attribute__((packed));

/* --------------------------------------------------------------------- */
static void fatal(const char *why) { perror(why); exit(EXIT_FAILURE); }

/* subscribe to multicast group by name â silently ignore if missing */
static void join_grp(struct nl_sock *s, const char *name)
{
    int id = genl_ctrl_resolve_grp(s, "nl80211", name);
    if (id >= 0) nl_socket_add_membership(s, id);
}

/* fetch BSSID via WirelessâExtensions ioctl (STA mode) */
static int get_bssid(const char *ifname, unsigned char mac[6])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return -1;
    struct iwreq req = {0};
    strncpy(req.ifr_name, ifname, IFNAMSIZ);
    int rc = ioctl(s, SIOCGIWAP, &req);
    close(s);
    if (rc == -1) return -1;
    memcpy(mac, req.u.ap_addr.sa_data, 6);
    return 0;
}

/* send GET_STATION request (heartbeat) */
static void request_stats(struct nl_sock *s, int nl80211_id,
                          int ifindex, const unsigned char mac[6])
{
    struct nl_msg *m = nlmsg_alloc();
    genlmsg_put(m, 0, 0, nl80211_id, 0, 0,
                NL80211_CMD_GET_STATION, 0);
    nla_put_u32(m, NL80211_ATTR_IFINDEX, ifindex);
    nla_put(m, NL80211_ATTR_MAC, 6, mac);
    nl_send_auto(s, m);
    nlmsg_free(m);
}

/* globals for Unix socket dest */
static int uds_fd = -1;
static struct sockaddr_un uds_addr;

/* -------- netlink parse callback ------------------------------------- */
static int parse_cb(struct nl_msg *msg, void *arg)
{
    struct wifi_qos_msg m = {0};

    struct nlattr *attrs[NL80211_ATTR_MAX + 1];
    struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];
    struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

    nla_parse(attrs, NL80211_ATTR_MAX,
              genlmsg_attrdata(gnlh, 0),
              genlmsg_attrlen(gnlh, 0), NULL);

    if (!attrs[NL80211_ATTR_STA_INFO]) return NL_SKIP;
    nla_parse_nested(sinfo, NL80211_STA_INFO_MAX,
                     attrs[NL80211_ATTR_STA_INFO], NULL);

    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    m.ts_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    if (sinfo[NL80211_STA_INFO_SIGNAL])
        m.rssi_dbm = (int8_t) nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL]);
    if (sinfo[NL80211_STA_INFO_TX_PACKETS])
        m.tx_ok = nla_get_u32(sinfo[NL80211_STA_INFO_TX_PACKETS]);
    if (sinfo[NL80211_STA_INFO_TX_RETRIES])
        m.tx_retry = nla_get_u32(sinfo[NL80211_STA_INFO_TX_RETRIES]);
    if (sinfo[NL80211_STA_INFO_TX_FAILED])
        m.tx_fail = nla_get_u32(sinfo[NL80211_STA_INFO_TX_FAILED]);

    sendto(uds_fd, &m, sizeof(m), 0,
           (struct sockaddr*)&uds_addr, sizeof(uds_addr));
    return NL_OK;
}

/* --------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    int interval_ms = 1000;          /* default heartbeat 1â¯s */
    int opt;
    while ((opt = getopt(argc, argv, "i:")) != -1) {
        if (opt == 'i') interval_ms = atoi(optarg);
    }
    argc -= optind; argv += optind;
    if (argc < 1 || argc > 2) {
        fprintf(stderr,
          "Usage: %s [-i <ms>] <interface> [peerâmac]\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *ifname = argv[0];

    /* ---------- Unix datagram destination ---------------------------- */
    const char *path = getenv("QOS_SOCK");
    if (!path) {
        static char defpath[128];
        snprintf(defpath, sizeof(defpath),
                 "/run/user/%u/wifi_qos.sock", getuid());
        path = defpath;
    }
    uds_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (uds_fd == -1) fatal("socket(AF_UNIX)");

    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;
    strncpy(uds_addr.sun_path, path, sizeof(uds_addr.sun_path) - 1);
    /* (sender side need not bind) */

    /* ---------- netlink setup ---------------------------------------- */
    struct nl_sock *sock = nl_socket_alloc();
    if (!sock) fatal("nl_socket_alloc");
    if (genl_connect(sock)) fatal("genl_connect");

    int nl80211_id = genl_ctrl_resolve(sock, "nl80211");
    if (nl80211_id < 0) fatal("genl_ctrl_resolve");

    nl_socket_modify_cb(sock, NL_CB_VALID, NL_CB_CUSTOM, parse_cb, NULL);

    join_grp(sock, "mlme");
    join_grp(sock, "station");
    join_grp(sock, "stats");

    int ifindex = if_nametoindex(ifname);
    if (!ifindex) fatal("if_nametoindex");

    unsigned char mac[6];
    if (argc == 2) {
        if (sscanf(argv[1], "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                   &mac[0], &mac[1], &mac[2],
                   &mac[3], &mac[4], &mac[5]) != 6) {
            fatal("bad MAC");
        }
    } else if (get_bssid(ifname, mac) == -1) {
        fprintf(stderr, "Need peer MAC in AP mode\n");
        return EXIT_FAILURE;
    }

    /* ---------- heartbeat timer ------------------------------------- */
    int tfd = -1;
    if (interval_ms > 0) {
        tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        if (tfd == -1) fatal("timerfd_create");
        struct itimerspec its = {
            .it_interval = { .tv_sec = interval_ms / 1000,
                             .tv_nsec = (interval_ms % 1000) * 1000000 },
            .it_value    = { .tv_sec = 0, .tv_nsec = 1 }   /* kick now */
        };
        timerfd_settime(tfd, 0, &its, NULL);
    }

    /* initial immediate stats */
    request_stats(sock, nl80211_id, ifindex, mac);

    /* ---------- main poll loop -------------------------------------- */
    struct pollfd fds[2] = {
        { .fd = nl_socket_get_fd(sock), .events = POLLIN },
        { .fd = tfd,                    .events = POLLIN }
    };
    for (;;) {
        int nfds = (tfd >= 0) ? 2 : 1;
        if (poll(fds, nfds, -1) == -1) {
            if (errno == EINTR) continue;
            fatal("poll");
        }
        if (fds[0].revents & POLLIN)
            nl_recvmsgs_default(sock);
        if (tfd >= 0 && (fds[1].revents & POLLIN)) {
            uint64_t exp; read(tfd, &exp, sizeof(exp)); /* ack */
            request_stats(sock, nl80211_id, ifindex, mac);
        }
    }
}

