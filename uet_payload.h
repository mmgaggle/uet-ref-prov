/*
 * Payload MTU: how many bytes of SES payload a full UET packet carries.
 *
 * UEC 1.0.1, 3.4.1.11: the Payload MTU is configured to 1024, 2048, 4096
 * or 8192 bytes, must be the same on every FEP that talks to another, and
 * must leave the whole packet within the Ethernet MTU of the path, since
 * UET packets are never fragmented (DF is set).
 *
 * This header has no dependencies, so that code which drives the core
 * without its headers (the libfabric glue, a device model) can size things
 * to the payload the core will use.
 */

#ifndef _UET_PAYLOAD_H_
#define _UET_PAYLOAD_H_

#include <stdbool.h>

#define UET_PAYLOAD_MTU_MIN	1024u
#define UET_PAYLOAD_MTU_MAX	8192u

/*
 * The most header a packet carries around a full payload, inside the IP
 * MTU: an IPv6 header (40), UDP (8), a TSS header with an SSI (16), a RUD
 * or ROD request header (12), the standard SES request header (44) with
 * its largest extension, rendezvous (24), and the TSS ICV (16). Every
 * other combination is smaller (the CRC trailer is 4, the entropy header
 * 4, IPv4 20).
 */
#define UET_PAYLOAD_HDR_MAX	160u

static inline bool uet_payload_mtu_valid(unsigned int payload)
{
	return (payload == 1024u) || (payload == 2048u) ||
	       (payload == 4096u) || (payload == 8192u);
}

/*
 * The largest Payload MTU whose packets fit an IP MTU of ip_mtu bytes, or
 * UET_PAYLOAD_MTU_MIN when even that does not fit: 1024 for an MTU of
 * 1500, 8192 for 9000.
 */
static inline unsigned int uet_payload_mtu_for_ip_mtu(unsigned int ip_mtu)
{
	unsigned int payload = UET_PAYLOAD_MTU_MAX;

	while ((payload > UET_PAYLOAD_MTU_MIN) &&
	       ((payload + UET_PAYLOAD_HDR_MAX) > ip_mtu))
		payload >>= 1;
	return payload;
}

#endif /* _UET_PAYLOAD_H_ */
