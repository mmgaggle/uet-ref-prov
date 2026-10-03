/*
 * Copyright (c) 2024,2025,2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/* NIC Interface common functions */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>

#include "uet_api_private.h"
#include "uet_nic.h"

/* helper to get IPv4 address of interface */
int uet_nic_get_ipv4_addr(int sock_fd,
			  struct ifreq *ifr,
			  uint32_t *ipv4_addr,
			  char *ipv4_addr_str)
{
	char *ip;

	ifr->ifr_addr.sa_family = AF_INET;
	if ((ioctl(sock_fd, SIOCGIFADDR, ifr)) < 0)
		return -ENOENT;

	ip = &ifr->ifr_addr.sa_data[2];
	*ipv4_addr = ntohl(*((uint32_t *)ip));
	inet_ntop(AF_INET, ip, ipv4_addr_str, INET_ADDRSTRLEN);
	return 0;
}

/* helper to get IPv6 address of interface from /proc/net/if_inet6 */
int uet_nic_get_ipv6_addr(const char *ifname,
			  uint8_t *ipv6_addr,
			  char *ipv6_addr_str)
{
	FILE *f;
	char line[128];
	char addr_hex[33];
	char if_name[IFNAMSIZ];
	int scope, prefix, flags, if_idx;
	unsigned int addr_bytes[16];
	int i;
	bool found_link_local = false;
	uint8_t link_local_addr[16];
	char link_local_str[INET6_ADDRSTRLEN];

	f = fopen("/proc/net/if_inet6", "r");
	if (!f)
		return -ENOENT;

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "%32s %x %x %x %x %s",
			   addr_hex, &if_idx, &prefix, &scope, &flags,
			   if_name) != 6)
			continue;

		if (strcmp(if_name, ifname) != 0)
			continue;

		/* skip loopback (scope 0x10) */
		if (scope == 0x10)
			continue;

		/* parse hex address */
		for (i = 0; i < 16; i++) {
			if (sscanf(&addr_hex[i * 2], "%2x", &addr_bytes[i]) != 1) {
				fclose(f);
				return -EINVAL;
			}
		}

		/* save link-local (scope 0x20) as fallback */
		if (scope == 0x20) {
			if (!found_link_local) {
				for (i = 0; i < 16; i++)
					link_local_addr[i] = (uint8_t)addr_bytes[i];
				inet_ntop(AF_INET6, link_local_addr,
					  link_local_str, INET6_ADDRSTRLEN);
				found_link_local = true;
			}
			continue;
		}

		/* found non-link-local address - use it */
		for (i = 0; i < 16; i++)
			ipv6_addr[i] = (uint8_t)addr_bytes[i];
		inet_ntop(AF_INET6, ipv6_addr, ipv6_addr_str, INET6_ADDRSTRLEN);
		fclose(f);
		return 0;
	}

	fclose(f);

	/* fallback to link-local if no global/site-local found */
	if (found_link_local) {
		memcpy(ipv6_addr, link_local_addr, 16);
		strncpy(ipv6_addr_str, link_local_str, INET6_ADDRSTRLEN);
		return 0;
	}

	return -ENOENT;
}

/*
 * IPv4 next-hop resolution over rtnetlink.
 *
 * The next hop comes from the route the kernel would use out of the shim's
 * interface (RTM_GETROUTE with that output interface, which also finds the
 * route inside a VRF the interface is enslaved to), and its MAC from the
 * interface's neighbor table (RTM_GETNEIGH). An existing entry is used as
 * it is, and never deleted. Without a usable one, a datagram to the next
 * hop out of the interface makes the kernel resolve it by ARP, and the
 * table is polled until the entry is usable or UET_NH_WAIT_MS (default
 * 1000) has passed. With UET_NH_WAIT_MS=0 nothing waits: the call returns
 * -EAGAIN while the resolution runs, and a later call finds the MAC. The
 * core retries resolution on every post until it succeeds, so a caller of
 * uet_av_insert() need not wait for a peer that is slow to answer.
 *
 * This replaces "ip route get", "arp -d" and "ping", which took a process
 * each, deleted a good entry first, and waited up to 10 s for an echo
 * reply from a peer that does not answer ping. Everything here is in local
 * variables, so threads may resolve at once.
 */
#define UET_NH_WAIT_MS_ENV	"UET_NH_WAIT_MS"
#define UET_NH_WAIT_MS_DEF	1000
#define UET_NH_PROBE_MS		200	/* a probe at most this often */
#define UET_NH_POLL_US		1000	/* neighbor table polling interval */
#define UET_NH_PROBE_PORT	9	/* discard */
#define UET_NL_BUF		16384

static uint64_t uet_nh_now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int uet_nh_wait_ms(void)
{
	const char *s = getenv(UET_NH_WAIT_MS_ENV);
	char *end;
	long v;

	if (s == NULL || *s == '\0')
		return UET_NH_WAIT_MS_DEF;
	v = strtol(s, &end, 10);
	if (*end != '\0' || v < 0 || v > 60000)
		return UET_NH_WAIT_MS_DEF;
	return (int)v;
}

/*
 * Send a request on a fresh NETLINK_ROUTE socket and hand every reply
 * message to cb, until NLMSG_DONE for a dump or the one reply otherwise.
 * Returns 0, a negative errno from the kernel, or one of the socket's.
 */
static int uet_nl_request(struct nlmsghdr *req,
			  void (*cb)(const struct nlmsghdr *, void *),
			  void *arg)
{
	struct sockaddr_nl sa;
	char *buf;
	int fd, rc = 0;
	bool dump = (req->nlmsg_flags & NLM_F_DUMP) != 0;
	bool done = false;

	fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
	if (fd < 0)
		return -errno;

	memset(&sa, 0, sizeof(sa));
	sa.nl_family = AF_NETLINK;
	req->nlmsg_seq = 1;
	if (sendto(fd, req, req->nlmsg_len, 0, (struct sockaddr *)&sa,
		   sizeof(sa)) < 0) {
		rc = -errno;
		close(fd);
		return rc;
	}

	buf = malloc(UET_NL_BUF);
	if (buf == NULL) {
		close(fd);
		return -ENOMEM;
	}

	while (!done) {
		ssize_t n = recv(fd, buf, UET_NL_BUF, 0);
		struct nlmsghdr *h;
		int len;

		if (n < 0) {
			if (errno == EINTR)
				continue;
			rc = -errno;
			break;
		}
		len = (int)n;
		for (h = (struct nlmsghdr *)buf; NLMSG_OK(h, len);
		     h = NLMSG_NEXT(h, len)) {
			if (h->nlmsg_seq != 1)
				continue;
			if (h->nlmsg_type == NLMSG_DONE) {
				done = true;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				struct nlmsgerr *e = NLMSG_DATA(h);

				rc = e->error;	/* 0 is an acknowledgement */
				done = true;
				break;
			}
			cb(h, arg);
			if (!dump) {
				done = true;
				break;
			}
		}
	}

	free(buf);
	close(fd);
	return rc;
}

static void uet_nl_add_attr(struct nlmsghdr *h, unsigned short type,
			    const void *data, unsigned short len)
{
	struct rtattr *rta = (struct rtattr *)
		((char *)h + NLMSG_ALIGN(h->nlmsg_len));

	rta->rta_type = type;
	rta->rta_len = RTA_LENGTH(len);
	memcpy(RTA_DATA(rta), data, len);
	h->nlmsg_len = NLMSG_ALIGN(h->nlmsg_len) + RTA_ALIGN(rta->rta_len);
}

struct uet_nl_route {
	bool found;
	uint32_t gw;	/* network order, 0 if on-link */
};

static void uet_nl_route_cb(const struct nlmsghdr *h, void *arg)
{
	struct uet_nl_route *r = arg;
	const struct rtmsg *rtm = NLMSG_DATA(h);
	const struct rtattr *rta;
	int len;

	if (h->nlmsg_type != RTM_NEWROUTE || rtm->rtm_family != AF_INET)
		return;
	r->found = true;
	len = (int)RTM_PAYLOAD(h);
	for (rta = RTM_RTA(rtm); RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
		if (rta->rta_type == RTA_GATEWAY &&
		    RTA_PAYLOAD(rta) == sizeof(uint32_t))
			memcpy(&r->gw, RTA_DATA(rta), sizeof(uint32_t));
	}
}

/* the next hop out of @ifindex to @dst (network order), as the kernel
 * routes it */
static int uet_nl_next_hop(int ifindex, uint32_t dst, uint32_t *nh)
{
	struct {
		struct nlmsghdr h;
		struct rtmsg r;
		char attrs[64];
	} req;
	struct uet_nl_route route = { 0 };
	int rc;

	memset(&req, 0, sizeof(req));
	req.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	req.h.nlmsg_type = RTM_GETROUTE;
	req.h.nlmsg_flags = NLM_F_REQUEST;
	req.r.rtm_family = AF_INET;
	req.r.rtm_dst_len = 32;
	uet_nl_add_attr(&req.h, RTA_DST, &dst, sizeof(dst));
	uet_nl_add_attr(&req.h, RTA_OIF, &ifindex, sizeof(ifindex));

	rc = uet_nl_request(&req.h, uet_nl_route_cb, &route);
	if (rc != 0)
		return rc;
	if (!route.found)
		return -ENETUNREACH;
	*nh = route.gw != 0 ? route.gw : dst;
	return 0;
}

struct uet_nl_neigh {
	int ifindex;
	uint32_t ip;	/* network order */
	bool found;
	uint16_t state;
	uint8_t mac[ETH_ALEN];
	bool has_mac;
};

static void uet_nl_neigh_cb(const struct nlmsghdr *h, void *arg)
{
	struct uet_nl_neigh *q = arg;
	const struct ndmsg *ndm = NLMSG_DATA(h);
	const struct rtattr *rta;
	bool match = false, has_mac = false;
	uint8_t mac[ETH_ALEN];
	int len;

	if (h->nlmsg_type != RTM_NEWNEIGH || ndm->ndm_family != AF_INET ||
	    ndm->ndm_ifindex != q->ifindex)
		return;
	len = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*ndm)));
	for (rta = (const struct rtattr *)((const char *)ndm +
					   NLMSG_ALIGN(sizeof(*ndm)));
	     RTA_OK(rta, len); rta = RTA_NEXT(rta, len)) {
		if (rta->rta_type == NDA_DST &&
		    RTA_PAYLOAD(rta) == sizeof(uint32_t))
			match = memcmp(RTA_DATA(rta), &q->ip,
				       sizeof(uint32_t)) == 0;
		else if (rta->rta_type == NDA_LLADDR &&
			 RTA_PAYLOAD(rta) == ETH_ALEN) {
			memcpy(mac, RTA_DATA(rta), ETH_ALEN);
			has_mac = true;
		}
	}
	if (!match)
		return;
	q->found = true;
	q->state = ndm->ndm_state;
	q->has_mac = has_mac;
	if (has_mac)
		memcpy(q->mac, mac, ETH_ALEN);
}

/* the neighbor entry of @ip (network order) on @ifindex */
static int uet_nl_neighbor(struct uet_nl_neigh *q)
{
	struct {
		struct nlmsghdr h;
		struct ndmsg n;
	} req;

	memset(&req, 0, sizeof(req));
	req.h.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
	req.h.nlmsg_type = RTM_GETNEIGH;
	req.h.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	req.n.ndm_family = AF_INET;
	req.n.ndm_ifindex = q->ifindex;	/* a filter, where the kernel has it */
	q->found = false;
	return uet_nl_request(&req.h, uet_nl_neigh_cb, q);
}

static bool uet_nl_neigh_usable(const struct uet_nl_neigh *q)
{
	static const uint8_t zero[ETH_ALEN];

	return q->found && q->has_mac &&
	       (q->state & (NUD_REACHABLE | NUD_STALE | NUD_DELAY |
			    NUD_PROBE | NUD_PERMANENT | NUD_NOARP)) != 0 &&
	       memcmp(q->mac, zero, ETH_ALEN) != 0;
}

/*
 * Make the kernel resolve @nh (network order) on @ifname: a datagram to it,
 * bound to the interface (a VRF slave too). It goes once the neighbor is
 * known; the kernel asks by ARP first. At most one per UET_NH_PROBE_MS for
 * a destination, across threads.
 */
static void uet_nh_probe(const char *ifname, int ifindex, uint32_t nh)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static struct { uint32_t nh; uint64_t at; } recent[16];
	static unsigned next;
	struct sockaddr_in sin;
	uint64_t now = uet_nh_now_ms();
	bool skip = false;
	unsigned i;
	int fd;

	pthread_mutex_lock(&lock);
	for (i = 0; i < 16; i++) {
		if (recent[i].nh == nh && recent[i].at != 0 &&
		    now - recent[i].at < UET_NH_PROBE_MS) {
			skip = true;
			break;
		}
	}
	if (!skip) {
		for (i = 0; i < 16 && recent[i].nh != nh; i++)
			;
		if (i == 16)
			i = next++ % 16;
		recent[i].nh = nh;
		recent[i].at = now;
	}
	pthread_mutex_unlock(&lock);
	if (skip)
		return;

	fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return;
	/* SO_BINDTODEVICE needs CAP_NET_RAW, which the raw socket needs as
	 * well; IP_UNICAST_IF picks the interface without it */
	if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname,
		       strlen(ifname) + 1) != 0) {
		uint32_t idx = htonl((uint32_t)ifindex);

		(void)setsockopt(fd, IPPROTO_IP, IP_UNICAST_IF, &idx,
				 sizeof(idx));
	}
	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(UET_NH_PROBE_PORT);
	sin.sin_addr.s_addr = nh;
	(void)sendto(fd, NULL, 0, MSG_DONTWAIT, (struct sockaddr *)&sin,
		     sizeof(sin));
	close(fd);
}

/* resolve next-hop info for ipv4 destination address */
int uet_nic_resolve_ipv4_nh(struct uet_nic *nic,
			    int sock_fd,
			    uint32_t dst_ip,
			    uint8_t *mac)
{
	char dst_str[INET_ADDRSTRLEN], nh_str[INET_ADDRSTRLEN];
	struct uet_nl_neigh q;
	uint32_t dst = htonl(dst_ip), nh;
	uint64_t deadline;
	int ifindex, wait_ms, rc;

	(void)sock_fd;

	ifindex = (int)if_nametoindex(nic->ifname);
	if (ifindex == 0) {
		UET_API_ERR("Unknown interface %s", nic->ifname);
		return -ENODEV;
	}

	inet_ntop(AF_INET, &dst, dst_str, sizeof(dst_str));
	rc = uet_nl_next_hop(ifindex, dst, &nh);
	if (rc != 0) {
		UET_API_ERR("No route to %s out of %s: %s", dst_str,
			    nic->ifname, strerror(-rc));
		return -ENETUNREACH;
	}
	inet_ntop(AF_INET, &nh, nh_str, sizeof(nh_str));

	memset(&q, 0, sizeof(q));
	q.ifindex = ifindex;
	q.ip = nh;
	wait_ms = uet_nh_wait_ms();
	deadline = uet_nh_now_ms() + (uint64_t)wait_ms;
	for (;;) {
		rc = uet_nl_neighbor(&q);
		if (rc != 0) {
			UET_API_ERR("Reading the neighbor table: %s",
				    strerror(-rc));
			return -EIO;
		}
		if (uet_nl_neigh_usable(&q))
			break;
		/* a failed entry is asked again: the peer may be back */
		uet_nh_probe(nic->ifname, ifindex, nh);
		if (wait_ms == 0)
			return -EAGAIN;
		if (uet_nh_now_ms() >= deadline) {
			UET_API_ERR("Unable to resolve next-hop %s of %s",
				    nh_str, dst_str);
			return -ENETUNREACH;
		}
		usleep(UET_NH_POLL_US);
	}

	memcpy(mac, q.mac, ETH_ALEN);

	printf("Next-Hop Address Resolution\n");
	printf("  Destination IPv4 Addr: %s\n", dst_str);
	printf("  Next-Hop IPv4 Addr:    %s\n", nh_str);
	printf("  Next-Hop MAC Addr:     ");
	uet_print_mac_addr(mac);

	return 0;
}

/* resolve next-hop info for ipv6 destination address */
int uet_nic_resolve_ipv6_nh(struct uet_nic *nic,
			    const uint8_t *dst_ip6,
			    uint8_t *mac)
{
	char sys_cmd[UET_MAX_SYS_CMD_OCTETS];
	char line[256];
	int i, rc;
	FILE *cmd_stream;
	uint8_t invalid_mac[ETH_ALEN];

	/* convert ipv6 addr to string */
	inet_ntop(AF_INET6, dst_ip6, nic->dst_ip_addr_str, INET6_ADDRSTRLEN);

	/* find next-hop ipv6 address using ip -6 route get */
	snprintf(sys_cmd, sizeof(sys_cmd),
		 "ip -6 route get %s oif %s 2>/dev/null | head -1",
		 nic->dst_ip_addr_str, nic->ifname);
	cmd_stream = popen(sys_cmd, "r");
	if (cmd_stream == NULL) {
		UET_API_PRINT_ERRNO("popen");
		UET_API_ERR("Error getting next-hop IPv6 address");
		return -EIO;
	}

	/* parse: "<dst> from <src> via <nexthop> ..." or "<dst> from <src> dev ..." */
	memset(line, 0, sizeof(line));
	if (fgets(line, sizeof(line), cmd_stream) == NULL) {
		UET_API_ERR("Error reading route output");
		pclose(cmd_stream);
		return -EIO;
	}
	pclose(cmd_stream);

	/* look for "via <nexthop>" in output, otherwise use dst as nexthop */
	char *via = strstr(line, " via ");
	if (via) {
		via += 5; /* skip " via " */
		for (i = 0; i < INET6_ADDRSTRLEN - 1 && via[i] && !isspace(via[i]); i++)
			nic->nh_ip_addr_str[i] = via[i];
		nic->nh_ip_addr_str[i] = '\0';
	} else {
		/* on-link destination, next-hop is the destination itself */
		strncpy(nic->nh_ip_addr_str, nic->dst_ip_addr_str,
			INET6_ADDRSTRLEN);
	}

	/* delete any entry for next-hop already in neighbor cache */
	snprintf(sys_cmd, sizeof(sys_cmd),
		 "ip -6 neigh del %s dev %s 2>/dev/null 1>/dev/null",
		 nic->nh_ip_addr_str, nic->ifname);
	system(sys_cmd);

	/* ping6 next hop to trigger NDP */
	snprintf(sys_cmd, sizeof(sys_cmd),
		 "ping -6 -c 1 -I %s %s 2>/dev/null 1>/dev/null",
		 nic->ifname, nic->nh_ip_addr_str);
	system(sys_cmd);

	/* read next-hop mac address from neighbor cache */
	snprintf(sys_cmd, sizeof(sys_cmd),
		 "ip -6 neigh show %s dev %s 2>/dev/null",
		 nic->nh_ip_addr_str, nic->ifname);
	cmd_stream = popen(sys_cmd, "r");
	if (cmd_stream == NULL) {
		UET_API_PRINT_ERRNO("popen");
		UET_API_ERR("Error reading neighbor cache");
		return -EIO;
	}

	/* parse: "<addr> dev <if> lladdr <mac> ..." */
	memset(line, 0, sizeof(line));
	memset(mac, 0, ETH_ALEN);
	if (fgets(line, sizeof(line), cmd_stream) != NULL) {
		char *lladdr = strstr(line, "lladdr ");
		if (lladdr) {
			lladdr += 7; /* skip "lladdr " */
			/* parse MAC address aa:bb:cc:dd:ee:ff */
			unsigned int m[6];
			if (sscanf(lladdr, "%x:%x:%x:%x:%x:%x",
				   &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6) {
				for (i = 0; i < 6; i++)
					mac[i] = (uint8_t)m[i];
			}
		}
	}
	pclose(cmd_stream);

	rc = 0;
	memset(invalid_mac, 0, ETH_ALEN);
	if (memcmp(mac, invalid_mac, ETH_ALEN) == 0) {
		UET_API_ERR("Unable to resolve next-hop MAC addr for IPv6");
		rc = -ENETUNREACH;
	}

	printf("Next-Hop Address Resolution\n");
	printf("  Destination IPv6 Addr: %s\n", nic->dst_ip_addr_str);
	printf("  Next-Hop IPv6 Addr:    %s\n", nic->nh_ip_addr_str);
	printf("  Next-Hop MAC Addr:     ");
	uet_print_mac_addr(mac);

	return rc;
}

int uet_nic_getinfo(struct uet_nic *nic,
		    struct uet_nic_info *nic_info)
{
	if (!nic || !nic_info)
		assert(0);

	memset(nic_info, 0, sizeof(struct uet_nic_info));

	return nic->nic_getinfo(nic, nic_info);
}

/* Raw Socket NIC protocol callbacks */
extern int nic_rawsock_getinfo(struct uet_nic *nic,
			       struct uet_nic_info *nic_info);
extern int nic_rawsock_tx_pkt_iov(struct uet_nic *nic,
				  const struct iovec *iov,
				  int iovcnt,
				  size_t pkt_size);
extern int nic_rawsock_tx_pkt(struct uet_nic *nic,
			      void *pkt,
			      void *iphdr,
			      size_t pkt_size);
extern int nic_rawsock_rx_pkt(struct uet_nic *nic,
			      void *pkt,
			      size_t pkt_buf_size,
			      size_t *rx_pkt_size);
extern int nic_rawsock_rx_poll(struct uet_nic *nic);
extern void nic_rawsock_finalize(struct uet_nic *nic);
extern int nic_rawsock_initialize(struct uet_nic *nic);

#if ENABLE_XDP
/* XDP NIC protocol callbacks */
extern int nic_xdp_getinfo(struct uet_nic *nic,
			   struct uet_nic_info *nic_info);
extern int nic_xdp_tx_pkt(struct uet_nic *nic,
			  void *pkt,
			  void *iphdr,
			  size_t pkt_size);
extern int nic_xdp_rx_pkt(struct uet_nic *nic,
			  void *pkt,
			  size_t pkt_buf_size,
			  size_t *rx_pkt_size);
extern int nic_xdp_rx_poll(struct uet_nic *nic);
extern void nic_xdp_finalize(struct uet_nic *nic);
extern int nic_xdp_initialize(struct uet_nic *nic);
#endif

/* the application supplied shim, see uet_nic_register_shim() */
static const struct uet_nic_shim_ops *ext_shim_ops;
static void *ext_shim_ctx;

int uet_nic_register_shim(const struct uet_nic_shim_ops *ops, void *ctx)
{
	if (ops == NULL) {
		ext_shim_ops = NULL;
		ext_shim_ctx = NULL;
		return 0;
	}

	if ((ops->name == NULL) || (ops->name[0] == '\0') ||
	    (ops->nic_initialize == NULL) || (ops->nic_finalize == NULL) ||
	    (ops->nic_getinfo == NULL) || (ops->nic_tx_pkt == NULL) ||
	    (ops->nic_rx_pkt == NULL) || (ops->nic_rx_poll == NULL))
		return -EINVAL;

	if ((strcmp(ops->name, "rawsock") == 0) ||
	    (strcmp(ops->name, "xdp") == 0))
		return -EEXIST;

	ext_shim_ops = ops;
	ext_shim_ctx = ctx;
	return 0;
}

/* init nic resources */
int uet_nic_initialize(struct uet_nic *nic)
{
	char *nic_shim;

	/* get interface name from environment variable */
	nic_shim = getenv(UET_NIC_SHIM);

	/* a registered shim is the default, and is also selectable by name */
	if ((ext_shim_ops != NULL) &&
	    ((nic_shim == NULL) || (strcmp(nic_shim, ext_shim_ops->name) == 0))) {
		nic->nic_getinfo     = ext_shim_ops->nic_getinfo;
		nic->nic_tx_pkt      = ext_shim_ops->nic_tx_pkt;
		nic->nic_rx_pkt      = ext_shim_ops->nic_rx_pkt;
		nic->nic_rx_poll     = ext_shim_ops->nic_rx_poll;
		nic->nic_finalize    = ext_shim_ops->nic_finalize;
		nic->nic_initialize  = ext_shim_ops->nic_initialize;
		nic->nic_resolve_nh  = ext_shim_ops->nic_resolve_nh;
		nic->nic_tx_pkt_iov  = ext_shim_ops->nic_tx_pkt_iov;
		nic->shim_ctx        = ext_shim_ctx;
		nic->sock_fd         = -1;

		return nic->nic_initialize(nic);
	}

#if ENABLE_XDP
	/* for an XDP build, make its shim the default */
	if (nic_shim == NULL)
		nic_shim = "xdp";
#endif

	if ((nic_shim == NULL) || (strcmp(nic_shim, "rawsock") == 0)) {
		nic->nic_getinfo     = nic_rawsock_getinfo;
		nic->nic_tx_pkt      = nic_rawsock_tx_pkt;
		nic->nic_tx_pkt_iov  = nic_rawsock_tx_pkt_iov;
		nic->nic_rx_pkt      = nic_rawsock_rx_pkt;
		nic->nic_rx_poll     = nic_rawsock_rx_poll;
		nic->nic_finalize    = nic_rawsock_finalize;
		nic->nic_initialize  = nic_rawsock_initialize;
#if ENABLE_XDP
	} else if (strcmp(nic_shim, "xdp") == 0) {
		nic->nic_getinfo     = nic_xdp_getinfo;
		nic->nic_tx_pkt      = nic_xdp_tx_pkt;
		nic->nic_rx_pkt      = nic_xdp_rx_pkt;
		nic->nic_rx_poll     = nic_xdp_rx_poll;
		nic->nic_finalize    = nic_xdp_finalize;
		nic->nic_initialize  = nic_xdp_initialize;
#endif
	} else {
		UET_API_ERR("invalid UET_NIC_SHIM environment variable");
		return -ENODEV;
	}

	return nic->nic_initialize(nic);
}

