/*
 * Copyright (c) 2024,2025,2026 Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

/* Definitions for NIC Interface to UET API's */

#ifndef _UET_NIC_H_
#define _UET_NIC_H_

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/uio.h>
#include <linux/if_ether.h>

#include "uet_addr.h"

//#define UET_NIC_DEBUG_HEXDUMP

/* environment variables to control the NIC interface */
#define UET_NIC_SHIM  "UET_NIC_SHIM"
#define UET_IFNAME    "UET_IFNAME"

#define UET_MAX_SYS_CMD_OCTETS  256
#define UET_NET_TYPE_SIZE 32

#define UET_NIC(uet) (&(uet)->nic)

struct uet_mr_buf_desc;
struct uet_instance;

enum uet_nic_link_state {
	UET_NIC_LINK_STATE_UNKNOWN = 0,
	UET_NIC_LINK_STATE_DOWN    = 1,
	UET_NIC_LINK_STATE_UP      = 2
};

struct uet_nic_info {
	char *ifname;
	char *network_type;
	char *mac_addr_str;
	size_t mtu;
	enum uet_nic_link_state link_state;
};

/* nic control block structure - field of struct uet_instance */
struct uet_nic {
	char ifname[IFNAMSIZ];
	char network_type[UET_NET_TYPE_SIZE];

	uint8_t mac_addr[ETH_ALEN];
	char mac_addr_str[ETH_ALEN*3];

	/*
	 * Dual-stack: Both the IPv4 and IPv6 local addresses are populated at
	 * init (whichever are present). The address family is selected per
	 * destination/packet at runtime, so there is no single active family.
	 */
	uint32_t ipv4_addr;                            /* host order */
	char ipv4_addr_str[INET_ADDRSTRLEN];
	bool has_ipv4;
	uint8_t ipv6_addr[16];
	char ipv6_addr_str[INET6_ADDRSTRLEN];
	bool has_ipv6;

	char dst_ip_addr_str[INET6_ADDRSTRLEN];  /* destination addr */
	char nh_ip_addr_str[INET6_ADDRSTRLEN];      /* next hop addr */

	size_t mtu;                                 /* interface mtu */
	size_t l2_hdr_size;            /* size of l2 header in bytes */
	size_t min_pkt_size;             /* min packet size in bytes */
	size_t min_ip_pkt_size;       /* min ip packet size in bytes */
	size_t max_pkt_size;             /* max packet size in bytes */

	uint8_t uet_ipproto;           /* ip protocol number for uet */

	/*
	 * Offloads a shim may claim in nic_initialize(): it fills in the
	 * IPv4 header checksum of every frame it transmits, so the library
	 * leaves it 0 (tx_ipv4_csum), or it hands over only frames whose
	 * IPv4 header checksum it has checked, so the library does not
	 * check it again (rx_ipv4_csum).
	 */
	bool tx_ipv4_csum;
	bool rx_ipv4_csum;

	int sock_fd;                    /* socket fd for ioctl calls */
	void *nic_priv_data;
	void *shim_ctx;     /* context given to uet_nic_register_shim() */

	/* function pointers supporting different NIC interfaces */
	int (*nic_getinfo)(struct uet_nic *nic,
			   struct uet_nic_info *nic_info);
	int (*nic_tx_pkt)(struct uet_nic *nic,
			  void *pkt,
			  void *iphdr,
			  size_t pkt_size);
	int (*nic_rx_pkt)(struct uet_nic *nic,
			  void *pkt,
			  size_t pkt_buf_size,
			  size_t *rx_pkt_size);
	int (*nic_rx_poll)(struct uet_nic *nic);
	void (*nic_finalize)(struct uet_nic *nic);
	int (*nic_initialize)(struct uet_nic *nic);
	/* optional, see struct uet_nic_shim_ops */
	int (*nic_resolve_nh)(struct uet_nic *nic,
			      const struct uet_fa *fa,
			      bool is_ipv6,
			      uint8_t *mac);
	/* optional, see struct uet_nic_shim_ops */
	int (*nic_tx_pkt_iov)(struct uet_nic *nic,
			      const struct iovec *iov,
			      int iovcnt,
			      size_t pkt_size);
};

/*
 * A NIC shim supplied by the application rather than built into the library.
 *
 * This is for a device model that carries UET frames over a transport of its
 * own, for example an emulated NIC that owns a TAP interface and must keep
 * every packet on the thread that services its guest. The library cannot
 * know about such a transport, so the device model hands over the callbacks
 * the built-in shims provide.
 *
 * The callbacks have the same contracts as the struct uet_nic members of the
 * same name. nic_initialize() must fill in the addressing and size fields of
 * struct uet_nic (ifname, network_type, mac_addr, mac_addr_str, ipv4_addr,
 * has_ipv4, ipv6_addr, has_ipv6, mtu, l2_hdr_size, min_pkt_size,
 * min_ip_pkt_size, max_pkt_size) exactly as the built-in shims do. The
 * context given at registration is available to every callback as
 * nic->shim_ctx.
 */
struct uet_nic_shim_ops {
	const char *name;                     /* matched against UET_NIC_SHIM */
	int (*nic_initialize)(struct uet_nic *nic);
	void (*nic_finalize)(struct uet_nic *nic);
	int (*nic_getinfo)(struct uet_nic *nic,
			   struct uet_nic_info *nic_info);
	int (*nic_tx_pkt)(struct uet_nic *nic,
			  void *pkt,
			  void *iphdr,
			  size_t pkt_size);
	int (*nic_rx_pkt)(struct uet_nic *nic,
			  void *pkt,
			  size_t pkt_buf_size,
			  size_t *rx_pkt_size);
	int (*nic_rx_poll)(struct uet_nic *nic);

	/*
	 * Optional next-hop resolver. When it is NULL the library resolves
	 * the next hop itself by running "ip route", pinging the next hop
	 * and reading the kernel ARP or neighbor cache, which blocks for as
	 * long as those take and needs the interface to be a kernel netdev.
	 *
	 * A resolver must not block. It returns 0 with the next-hop MAC
	 * address in mac, -EAGAIN when it has started a resolution that has
	 * not finished (the operation that needed it fails with -FI_EAGAIN
	 * and the caller retries it later), or another negative errno when
	 * the destination is unreachable.
	 */
	int (*nic_resolve_nh)(struct uet_nic *nic,
			      const struct uet_fa *fa,
			      bool is_ipv6,
			      uint8_t *mac);

	/*
	 * Optional transmit of one frame in pieces, so the payload need not
	 * be copied into the frame first. The pieces, in order, add up to
	 * pkt_size bytes: iov[0] is the headers (Ethernet through SES) and
	 * iov[iovcnt - 1] the 4-byte CRC trailer, both in the library's
	 * memory and valid only for the call; the pieces between are the
	 * payload in the region the operation names, valid as long as the
	 * operation is (the region may not be written meanwhile), so a shim
	 * may send from them after it returns. The IPv4 header in iov[0] is
	 * complete, checksum included unless the shim claims tx_ipv4_csum.
	 * Returns 0, or a negative errno; -ENOTSUP makes the library fall
	 * back to nic_tx_pkt for that frame. When the callback is NULL every
	 * frame goes through nic_tx_pkt.
	 */
	int (*nic_tx_pkt_iov)(struct uet_nic *nic,
			      const struct iovec *iov,
			      int iovcnt,
			      size_t pkt_size);
};

/*
 * register an application supplied NIC shim
 *
 * Must be called before uet_initialize(). The shim is used when the
 * UET_NIC_SHIM environment variable names it, or when UET_NIC_SHIM is not
 * set, so registering a shim makes it the default. One external shim can be
 * registered at a time, and the registration lasts until it is replaced or
 * cleared. The ops structure is referenced, not copied, and must outlive
 * every instance that uses it.
 *
 * parms:
 *      ops - shim callbacks, or NULL to clear the registration
 *      ctx - opaque context made available to the callbacks as
 *            nic->shim_ctx
 *
 * returns:
 *      0 on success
 *      -EINVAL if a required callback is missing
 *      -EEXIST if the name is that of a built-in shim
 */
int uet_nic_register_shim(const struct uet_nic_shim_ops *ops, void *ctx);

/*********************************************************************
 * NIC APIs
 *********************************************************************/

/*
 * helper to get IPv4 address of interface
 *
 * parms:
 *      sock_fd       - socket file descriptor for ioctl
 *      ifr           - ptr to ifreq struct (ifr_name must be set)
 *      ipv4_addr     - ptr to location where IPv4 addr is returned
 *      ipv4_addr_str - ptr to buffer for IPv4 addr string
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
int uet_nic_get_ipv4_addr(int sock_fd,
			  struct ifreq *ifr,
			  uint32_t *ipv4_addr,
			  char *ipv4_addr_str);

/*
 * helper to get IPv6 address of interface from /proc/net/if_inet6
 *
 * parms:
 *      ifname        - interface name
 *      ipv6_addr     - ptr to 16-byte buffer for IPv6 addr
 *      ipv6_addr_str - ptr to buffer for IPv6 addr string
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
int uet_nic_get_ipv6_addr(const char *ifname,
			  uint8_t *ipv6_addr,
			  char *ipv6_addr_str);

/*
 * helper to resolve IPv4 next-hop MAC address
 *
 * parms:
 *      nic     - ptr to uet nic struct
 *      sock_fd - socket file descriptor for ioctl
 *      dst_ip  - destination IPv4 address
 *      mac     - ptr to location where MAC address is returned
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
int uet_nic_resolve_ipv4_nh(struct uet_nic *nic,
			    int sock_fd,
			    uint32_t dst_ip,
			    uint8_t *mac);

/*
 * the same, waiting at most wait_ms for ARP; with 0 it returns -EAGAIN
 * while ARP runs and -ENETUNREACH for a next hop whose resolution failed
 */
int uet_nic_resolve_ipv4_nh_wait(struct uet_nic *nic,
				 uint32_t dst_ip,
				 uint8_t *mac,
				 int wait_ms);

/*
 * helper to resolve IPv6 next-hop MAC address
 *
 * parms:
 *      nic      - ptr to uet nic struct
 *      dst_ip6  - destination IPv6 address
 *      mac      - ptr to location where MAC address is returned
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
int uet_nic_resolve_ipv6_nh(struct uet_nic *nic,
			    const uint8_t *dst_ip6,
			    uint8_t *mac);

/*
 * initialize nic resources
 *
 * parms:
 *      uet - ptr to uet nic struct
 *
 * Discovers both IPv4 and IPv6 local addresses (whichever are present);
 * the address family is selected per destination/packet at runtime.
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
int uet_nic_initialize(struct uet_nic *nic);

/*
 * free nic resources
 *
 * parms:
 *      uet - ptr to uet nic struct
 */
static inline void uet_nic_finalize(struct uet_nic *nic)
{
	if (!nic)
		assert(0);

	return nic->nic_finalize(nic);
}

/*
 * get nic info
 *
 * parms:
 *      nic - ptr to uet nic struct
 *      nic_info - ptr to uet nic info struct to be filled in
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
int uet_nic_getinfo(struct uet_nic *nic,
		    struct uet_nic_info *nic_info);

/*
 * get next-hop info for ipv4 destination address
 *
 * parms:
 *      uet    - ptr to uet nic struct
 *      dst_ip - destination IPv4 address for which info is requested
 *      mac    - ptr to location where mac address is to be returned
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
static inline int uet_nic_get_ipv4_nh(struct uet_nic *nic,
				      uint32_t dst_ip,
				      uint8_t *mac)
{
	if (!nic || !mac)
		assert(0);

	if (nic->nic_resolve_nh) {
		struct uet_fa fa = { .v4 = dst_ip };

		return nic->nic_resolve_nh(nic, &fa, false, mac);
	}

	return uet_nic_resolve_ipv4_nh(nic, nic->sock_fd, dst_ip, mac);
}

/*
 * get next-hop info for ipv6 destination address
 *
 * parms:
 *      nic     - ptr to uet nic struct
 *      dst_ip6 - destination IPv6 address for which info is requested
 *      mac     - ptr to location where mac address is to be returned
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
static inline int uet_nic_get_ipv6_nh(struct uet_nic *nic,
				      const uint8_t *dst_ip6,
				      uint8_t *mac)
{
	if (!nic || !dst_ip6 || !mac)
		assert(0);

	if (nic->nic_resolve_nh) {
		struct uet_fa fa;

		memcpy(fa.v6, dst_ip6, UET_IPV6_ADDR_OCTETS);
		return nic->nic_resolve_nh(nic, &fa, true, mac);
	}

	return uet_nic_resolve_ipv6_nh(nic, dst_ip6, mac);
}

/*
 * get next-hop info for destination address (v4 or v6)
 *
 * parms:
 *      nic     - ptr to uet nic struct
 *      fa      - ptr to fabric address (contains v4 or v6 address)
 *      is_ipv6 - true if fa contains an IPv6 address
 *      mac     - ptr to location where mac address is to be returned
 *
 * returns:
 *      0 on success
 *      negative value corresponding to errno on error
 */
static inline int uet_nic_get_nh(struct uet_nic *nic,
				 const struct uet_fa *fa,
				 bool is_ipv6,
				 uint8_t *mac)
{
	if (!nic || !fa || !mac)
		assert(0);

	if (is_ipv6)
		return uet_nic_get_ipv6_nh(nic, fa->v6, mac);
	else
		return uet_nic_get_ipv4_nh(nic, fa->v4, mac);
}

/*
 * the same without waiting, for a post that finds its peer unresolved:
 * -EAGAIN while ARP runs, -ENETUNREACH for a next hop that failed. A shim
 * with a resolver of its own already never waits; IPv6 still does.
 */
static inline int uet_nic_get_nh_nowait(struct uet_nic *nic,
					const struct uet_fa *fa,
					bool is_ipv6,
					uint8_t *mac)
{
	if (!nic || !fa || !mac)
		assert(0);

	if (is_ipv6 || nic->nic_resolve_nh)
		return uet_nic_get_nh(nic, fa, is_ipv6, mac);
	return uet_nic_resolve_ipv4_nh_wait(nic, fa->v4, mac, 0);
}

/*
 * transmit a packet
 *
 * parms:
 *      uet      - ptr to uet nic struct
 *      pkt      - ptr to packet to send
 *      iphdr    - ptr to ip header in packet to send
 *      pkt_size - size of packet to send in bytes
 *
 * returns:
 *      0 on success,
 *      negative value corresponding to errno on error
 */
static inline int uet_nic_tx_pkt(struct uet_nic *nic,
				 void *pkt,
				 void *iphdr,
				 size_t pkt_size)
{
	if (!nic || !pkt || !pkt_size)
		assert(0);

	return nic->nic_tx_pkt(nic, pkt, iphdr, pkt_size);
}

/*
 * transmit a packet given as pieces, see struct uet_nic_shim_ops
 *
 * returns:
 *	0 on success, -ENOTSUP when the shim cannot, or a negative errno
 */
static inline int uet_nic_tx_pkt_iov(struct uet_nic *nic,
				     const struct iovec *iov,
				     int iovcnt,
				     size_t pkt_size)
{
	if (nic->nic_tx_pkt_iov == NULL)
		return -ENOTSUP;
	return nic->nic_tx_pkt_iov(nic, iov, iovcnt, pkt_size);
}

/*
 * receive a packet
 *
 * parms:
 *      uet - ptr to uet nic struct
 *      pkt - ptr to receive packet buffer
 *      pkt_buf_size - size of receive packet buffer in bytes
 *      rx_pkt_size  - ptr to location where size of received packet
 *                     in bytes is to be returned
 *
 * returns:
 *      0, no valid packet available
 *      1, read a packet
 *      negative value corresponding to errno, err reading packet
 */
static inline int uet_nic_rx_pkt(struct uet_nic *nic,
				 void *pkt,
				 size_t pkt_buf_size,
				 size_t *rx_pkt_size)
{
	if (!nic || !pkt || !pkt_buf_size || !rx_pkt_size)
		assert(0);

	return nic->nic_rx_pkt(nic, pkt, pkt_buf_size, rx_pkt_size);
}

/*
 * poll to determine if rx packet is available
 *
 * parms:
 *      ctx - ptr to uet nic struct
 *
 * returns:
 *      0, no packet is available
 *      1, packet is available
 *      negative value corresponding to errno, poll err
 */
static inline int uet_nic_rx_poll(struct uet_nic *nic)
{
	if (!nic)
		assert(0);

	return nic->nic_rx_poll(nic);
}

#endif /* _UET_NIC_H_ */
