/*
 * Interfaces for the ernic core (CORE=ernic): libuet_ernic drives the UET
 * engine inside a rocm-ernic device, so a domain is an ionic RDMA device,
 * and its address is the one the engine reports. The engine has its own
 * IPv4 address on the wire, which is not an address of any guest netdev.
 *
 * Finding that address takes a command on the device's service queue
 * pair. A device without an engine never answers, so naming the device
 * ($UET_ERNIC_DEVICE or domain_attr->name) avoids a long timeout when the
 * guest has other ionic devices.
 */

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <infiniband/verbs.h>

#include "uet_fi.h"
#include "uet_ernic.h"	/* uet_ernic_device_match() */

const struct uetfi_core_desc uetfi_core_desc = {
	.name = "ernic",
	.if_env = "UET_ERNIC_DEVICE",
	.if_usable = "needs to be the ionic RDMA device of an ernic "
		     "instance started with --uet",
	.init_hint = "the device needs to be an ernic instance started "
		     "with --uet",
	/* the engine raises no events at the target */
	.cq_data_size = 0,
	/*
	 * Every segment is a command to the device and a reply back, and
	 * the engine paces the wire itself, so segments can be large.
	 */
	.segment_pkts = 0,
	.segment_size = 1 << 20,
	.max_segments = 4,
};

/*
 * Open the device through the core API alone, the way a domain does, and
 * ask an endpoint for its address.
 */
int uetfi_if_query(const char *name, struct uetfi_ifinfo *ifinfo)
{
	uet_handle_t h;
	uet_domain_handle_t dom;
	uet_ep_handle_t ep;
	struct uet_addr ua;
	char *saved = NULL, *cur;
	int ret;

	if (!name || !*name || strlen(name) >= UETFI_NAME_MAX)
		return -FI_EINVAL;

	pthread_mutex_lock(&uetfi_env_lock);
	cur = getenv(uetfi_core_desc.if_env);
	if (cur)
		saved = strdup(cur);
	setenv(uetfi_core_desc.if_env, name, 1);
	ret = uet_initialize(&h);
	if (saved) {
		setenv(uetfi_core_desc.if_env, saved, 1);
		free(saved);
	} else {
		unsetenv(uetfi_core_desc.if_env);
	}
	pthread_mutex_unlock(&uetfi_env_lock);
	if (ret)
		return ret;

	ret = uet_domain(h, NULL, NULL, NULL, NULL, NULL, NULL, &dom);
	if (ret)
		goto out_fini;
	ret = uet_endpoint(dom, NULL, NULL, NULL, &ep);
	if (ret)
		goto out_dom;
	ret = uet_getname(ep, &ua);
	if (!ret && uet_addr_is_ipv6(&ua))
		ret = -FI_EADDRNOTAVAIL;
	if (!ret) {
		memset(ifinfo, 0, sizeof(*ifinfo));
		strncpy(ifinfo->name, name, UETFI_NAME_MAX - 1);
		ifinfo->ipv4 = ua.fa.v4;
	}
	uet_ep_close(ep);
out_dom:
	uet_domain_close(dom);
out_fini:
	uet_finalize(h);
	return ret;
}

int uetfi_if_foreach(int (*fn)(const struct uetfi_ifinfo *, void *),
		     void *arg)
{
	struct ibv_device **list;
	struct uetfi_ifinfo ifinfo;
	int i, n = 0, ret = 0;

	list = ibv_get_device_list(&n);
	if (!list)
		return 0;
	for (i = 0; i < n && !ret; i++) {
		const char *name = ibv_get_device_name(list[i]);

		/* the rule libuet_ernic uses when no device is named */
		if (!uet_ernic_device_match(name))
			continue;
		ret = uetfi_if_query(name, &ifinfo);
		if (ret) {
			UETFI_INFO(FI_LOG_CORE, "%s: no UET engine: %s\n",
				   name, fi_strerror(-ret));
			ret = 0;
			continue;
		}
		ret = fn(&ifinfo, arg);
	}
	ibv_free_device_list(list);
	return ret;
}
