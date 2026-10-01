/*
 * Interfaces for the reference core (CORE=ref): the core runs its raw
 * socket NIC shim on a netdev, so a domain is a netdev, and its address
 * is the netdev's primary IPv4 address.
 */

#include <errno.h>
#include <net/if_arp.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <arpa/inet.h>

#include "uet_fi.h"

const struct uetfi_core_desc uetfi_core_desc = {
	.name = "ref",
	.if_env = "UET_IFNAME",
	.if_usable = "needs to be up, Ethernet, with an IPv4 address",
	.init_hint = "the raw socket NIC shim needs CAP_NET_RAW",
	.cq_data_size = UETFI_CQ_DATA_SIZE,
	/*
	 * The core has no flow control for RUDI and sends a message's
	 * packets at once; a burst larger than the receiver's socket
	 * buffer is lost and recovered only by timeouts.
	 */
	.segment_size = 16384,
	.max_segments = 2,
};

/*
 * Look up an interface the way the rawsock NIC shim does (SIOCGIFADDR
 * gives the primary IPv4 address the core will use). Needs no privilege.
 */
int uetfi_if_query(const char *name, struct uetfi_ifinfo *ifinfo)
{
	struct ifreq ifr;
	int fd, ret = -FI_ENODATA;

	if (!name || !*name || strlen(name) >= IFNAMSIZ)
		return -FI_EINVAL;

	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
		return -errno;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
	if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0)
		goto out;
	if (!(ifr.ifr_flags & IFF_UP) || (ifr.ifr_flags & IFF_LOOPBACK))
		goto out;
	if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0 ||
	    ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER)
		goto out;
	if (ioctl(fd, SIOCGIFMTU, &ifr) < 0)
		goto out;
	ifinfo->mtu = ifr.ifr_mtu;
	ifr.ifr_addr.sa_family = AF_INET;
	if (ioctl(fd, SIOCGIFADDR, &ifr) < 0)
		goto out;
	ifinfo->ipv4 = ntohl(((struct sockaddr_in *) &ifr.ifr_addr)->
			     sin_addr.s_addr);
	memset(ifinfo->name, 0, sizeof(ifinfo->name));
	strncpy(ifinfo->name, name, UETFI_NAME_MAX - 1);
	ret = 0;
out:
	close(fd);
	return ret;
}

int uetfi_if_foreach(int (*fn)(const struct uetfi_ifinfo *, void *),
		     void *arg)
{
	struct if_nameindex *ifs, *ifp;
	struct uetfi_ifinfo ifinfo;
	int ret = 0;

	ifs = if_nameindex();
	if (!ifs)
		return 0;
	for (ifp = ifs; ifp->if_index && !ret; ifp++) {
		if (uetfi_if_query(ifp->if_name, &ifinfo))
			continue;
		ret = fn(&ifinfo, arg);
	}
	if_freenameindex(ifs);
	return ret;
}
