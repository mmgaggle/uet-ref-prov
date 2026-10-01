/*
 * Provider entry point, fi_getinfo, fabric object, and the process-wide
 * instance of the reference provider.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "uet_fi.h"

struct uetfi_params uetfi_params = {
	.ifname = NULL,
	.rudi = 1,
	.tx_timeout_ms = -1,
	.progress_burst = 64,
	.segment_size = 0,	/* the core's default */
	.max_segments = 0,
	.tx_retries = -1,
};

ssize_t uetfi_nosys(void)
{
	return -FI_ENOSYS;
}

/*******************************************************************
 * addresses
 *******************************************************************/

static void put_be16(uint8_t *p, uint16_t v)
{
	p[0] = v >> 8;
	p[1] = v;
}

static void put_be32(uint8_t *p, uint32_t v)
{
	p[0] = v >> 24;
	p[1] = v >> 16;
	p[2] = v >> 8;
	p[3] = v;
}

static uint16_t get_be16(const uint8_t *p)
{
	return (uint16_t) ((p[0] << 8) | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
	return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) |
	       ((uint32_t) p[2] << 8) | p[3];
}

void uetfi_addr_encode(const struct uet_addr *addr, uint8_t *wire)
{
	memset(wire, 0, UETFI_ADDR_LEN);
	wire[0] = UETFI_ADDR_MAGIC;
	wire[1] = UETFI_ADDR_VERSION;
	wire[2] = addr->flags;
	put_be16(&wire[4], addr->fep_cap);
	put_be16(&wire[6], addr->pid_on_fep);
	put_be16(&wire[8], addr->start_index);
	put_be16(&wire[10], addr->num_indices);
	put_be32(&wire[12], addr->initiator_id);
	if (uet_addr_is_ipv6(addr))
		memcpy(&wire[16], addr->fa.v6, UET_IPV6_ADDR_OCTETS);
	else
		put_be32(&wire[16], addr->fa.v4);
}

int uetfi_addr_decode(const void *buf, size_t len, struct uet_addr *addr)
{
	const uint8_t *wire = buf;

	if (!buf || len < UETFI_ADDR_LEN || wire[0] != UETFI_ADDR_MAGIC ||
	    wire[1] != UETFI_ADDR_VERSION)
		return -FI_EINVAL;

	memset(addr, 0, sizeof(*addr));
	addr->ver = UET_ADDR_VERSION;
	addr->flags = wire[2];
	addr->fep_cap = get_be16(&wire[4]);
	addr->pid_on_fep = get_be16(&wire[6]);
	addr->start_index = get_be16(&wire[8]);
	addr->num_indices = get_be16(&wire[10]);
	addr->initiator_id = get_be32(&wire[12]);
	if (uet_addr_is_ipv6(addr))
		memcpy(addr->fa.v6, &wire[16], UET_IPV6_ADDR_OCTETS);
	else
		addr->fa.v4 = get_be32(&wire[16]);

	if (!(addr->flags & UET_ADDR_FA_V))
		return -FI_EINVAL;
	return 0;
}

/*
 * The address an endpoint on 'ipv4' gets from the reference provider:
 * relative addressing, the HPC profile advertised (so writes to it may use
 * RUDI) and the provider's default PIDonFEP, index and initiator.
 */
void uetfi_addr_default(struct uet_addr *addr, uint32_t ipv4)
{
	memset(addr, 0, sizeof(*addr));
	addr->ver = UET_ADDR_VERSION;
	addr->flags = (UET_ADDR_FEP_CAP_V | UET_ADDR_FA_V |
		       UET_ADDR_PID_ON_FEP_V | UET_ADDR_INDEX_V |
		       UET_ADDR_INITIATOR_V | UET_ADDR_RELATIVE_MODE |
		       UET_ADDR_IPV4 | UET_ADDR_BIG_MSG_SIZE);
	addr->fep_cap = (UET_FEP_CAP_AI_FULL | UET_FEP_CAP_HPC);
	addr->fa.v4 = ipv4;
	addr->pid_on_fep = UET_ADDR_DEF_PID_ON_FEP;
	addr->num_indices = 1;
	addr->start_index = UET_ADDR_DEF_INDEX;
	addr->initiator_id = UET_ADDR_DEF_INITIATOR_ID;
}

const char *uetfi_addr_str(const void *wire, size_t len, char *buf,
			   size_t *buflen)
{
	struct uet_addr addr;
	char ip[INET6_ADDRSTRLEN];
	int n;

	if (uetfi_addr_decode(wire, len, &addr)) {
		n = snprintf(buf, *buflen, "fi_addr_uet://invalid");
	} else {
		if (uet_addr_is_ipv6(&addr)) {
			inet_ntop(AF_INET6, addr.fa.v6, ip, sizeof(ip));
		} else {
			uint32_t v4 = htonl(addr.fa.v4);

			inet_ntop(AF_INET, &v4, ip, sizeof(ip));
		}
		n = snprintf(buf, *buflen, "fi_addr_uet://%s/%u/%u/%u", ip,
			     addr.pid_on_fep, addr.start_index,
			     addr.initiator_id);
	}
	if (*buflen)
		buf[*buflen - 1] = '\0';
	*buflen = n + 1;
	return buf;
}

/* the interface to use when the application does not name one */
static const char *uetfi_default_ifname(void)
{
	if (uetfi_params.ifname && *uetfi_params.ifname)
		return uetfi_params.ifname;
	return getenv(uetfi_core_desc.if_env);
}

/*******************************************************************
 * the reference provider instance
 *******************************************************************/

/*
 * The reference provider keeps its packet delivery state in globals, so
 * a process has one instance, bound to one netdev, shared by every
 * domain on that netdev. Endpoints all get the default resource index,
 * so a process has one endpoint at a time.
 */
pthread_mutex_t uetfi_env_lock = PTHREAD_MUTEX_INITIALIZER;

static struct {
	pthread_mutex_t lock;
	uet_handle_t h;
	char ifname[UETFI_NAME_MAX];
	int refs;
	bool ep_open;
} uetfi_core = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

/*
 * The core reads its configuration from the environment. Apply the
 * provider's defaults where the user has not set the variables.
 */
static void uetfi_core_env(void)
{
	char val[32];

	setenv("UET_PDS", "pds", 0);
	if (uetfi_params.rudi)
		setenv("UET_FORCE_RUDI", "1", 0);
	else
		unsetenv("UET_FORCE_RUDI");
	if (uetfi_params.tx_timeout_ms >= 0) {
		snprintf(val, sizeof(val), "%d", uetfi_params.tx_timeout_ms);
		setenv("UET_PDS_TX_TIMEOUT", val, 1);
	} else {
		setenv("UET_PDS_TX_TIMEOUT", "200", 0);
	}
	if (uetfi_params.tx_retries >= 0) {
		snprintf(val, sizeof(val), "%d", uetfi_params.tx_retries);
		setenv("UET_PDS_MAX_TX_RETRIES", val, 1);
	} else {
		setenv("UET_PDS_MAX_TX_RETRIES", "25", 0);
	}
}

int uetfi_core_get(const char *ifname, uet_handle_t *h)
{
	char *saved = NULL, *cur;
	int ret = 0;

	pthread_mutex_lock(&uetfi_core.lock);
	if (uetfi_core.refs) {
		if (strcmp(uetfi_core.ifname, ifname)) {
			UETFI_WARN(FI_LOG_DOMAIN,
				   "netdev %s requested, but this process "
				   "already uses %s (one netdev per process)\n",
				   ifname, uetfi_core.ifname);
			ret = -FI_EBUSY;
			goto out;
		}
		uetfi_core.refs++;
		*h = uetfi_core.h;
		goto out;
	}

	pthread_mutex_lock(&uetfi_env_lock);
	uetfi_core_env();
	cur = getenv(uetfi_core_desc.if_env);
	if (cur)
		saved = strdup(cur);
	setenv(uetfi_core_desc.if_env, ifname, 1);
	ret = uet_initialize(&uetfi_core.h);
	if (saved) {
		setenv(uetfi_core_desc.if_env, saved, 1);
		free(saved);
	} else {
		unsetenv(uetfi_core_desc.if_env);
	}
	pthread_mutex_unlock(&uetfi_env_lock);
	if (ret) {
		UETFI_WARN(FI_LOG_DOMAIN,
			   "uet_initialize on %s failed: %s (%s)\n", ifname,
			   fi_strerror(-ret), uetfi_core_desc.init_hint);
		goto out;
	}
	memset(uetfi_core.ifname, 0, sizeof(uetfi_core.ifname));
	strncpy(uetfi_core.ifname, ifname, UETFI_NAME_MAX - 1);
	uetfi_core.refs = 1;
	*h = uetfi_core.h;
out:
	pthread_mutex_unlock(&uetfi_core.lock);
	return ret;
}

void uetfi_core_put(void)
{
	pthread_mutex_lock(&uetfi_core.lock);
	if (uetfi_core.refs && --uetfi_core.refs == 0) {
		uet_finalize(uetfi_core.h);
		uetfi_core.h = NULL;
	}
	pthread_mutex_unlock(&uetfi_core.lock);
}

int uetfi_core_claim_ep(void)
{
	int ret = 0;

	pthread_mutex_lock(&uetfi_core.lock);
	if (uetfi_core.ep_open)
		ret = -FI_EBUSY;
	else
		uetfi_core.ep_open = true;
	pthread_mutex_unlock(&uetfi_core.lock);
	return ret;
}

void uetfi_core_release_ep(void)
{
	pthread_mutex_lock(&uetfi_core.lock);
	uetfi_core.ep_open = false;
	pthread_mutex_unlock(&uetfi_core.lock);
}

/*******************************************************************
 * fi_getinfo
 *******************************************************************/

static int uetfi_check_hints(uint32_t version, const struct fi_info *hints)
{
	const struct fi_domain_attr *da;
	const struct fi_ep_attr *ea;
	const struct fi_tx_attr *ta;
	const struct fi_rx_attr *ra;

	if (!hints)
		return 0;

#define REJECT(...) do {						\
		UETFI_INFO(FI_LOG_CORE, __VA_ARGS__);			\
		return -FI_ENODATA;					\
	} while (0)

	if (hints->caps & ~UETFI_CAPS)
		REJECT("unsupported caps %s\n",
		       fi_tostr(&(uint64_t) {hints->caps & ~UETFI_CAPS},
				FI_TYPE_CAPS));
	if (hints->addr_format != FI_FORMAT_UNSPEC)
		REJECT("address format must be FI_FORMAT_UNSPEC\n");

	if (hints->fabric_attr && hints->fabric_attr->name &&
	    strcmp(hints->fabric_attr->name, UETFI_FABRIC_NAME))
		REJECT("fabric %s is not %s\n", hints->fabric_attr->name,
		       UETFI_FABRIC_NAME);

	ea = hints->ep_attr;
	if (ea) {
		if (ea->type != FI_EP_UNSPEC && ea->type != FI_EP_RDM)
			REJECT("only FI_EP_RDM is supported\n");
		if (ea->protocol != FI_PROTO_UNSPEC)
			REJECT("protocol must be FI_PROTO_UNSPEC\n");
		if (ea->max_msg_size > UETFI_MAX_MSG_SIZE)
			REJECT("max_msg_size too large\n");
		if (ea->tx_ctx_cnt > 1 && ea->tx_ctx_cnt != FI_SHARED_CONTEXT)
			REJECT("one tx context per endpoint\n");
		if (ea->rx_ctx_cnt > 1 && ea->rx_ctx_cnt != FI_SHARED_CONTEXT)
			REJECT("one rx context per endpoint\n");
		if (ea->tx_ctx_cnt == FI_SHARED_CONTEXT ||
		    ea->rx_ctx_cnt == FI_SHARED_CONTEXT)
			REJECT("shared contexts are not supported\n");
		if (ea->auth_key_size)
			REJECT("authorization keys are not supported\n");
	}

	da = hints->domain_attr;
	if (da) {
		switch (da->threading) {
		case FI_THREAD_UNSPEC:
		case FI_THREAD_DOMAIN:
			break;
		default:
			REJECT("only FI_THREAD_DOMAIN is supported\n");
		}
		if (da->control_progress == FI_PROGRESS_AUTO ||
		    da->data_progress == FI_PROGRESS_AUTO)
			REJECT("only manual progress is supported\n");
		if (da->resource_mgmt == FI_RM_DISABLED)
			REJECT("resource management cannot be disabled\n");
		switch (da->av_type) {
		case FI_AV_UNSPEC:
		case FI_AV_TABLE:
		case FI_AV_MAP:
			break;
		default:
			REJECT("unsupported AV type\n");
		}
		/* FI_MR_BASIC/FI_MR_SCALABLE, or a pre-1.5 application */
		if ((da->mr_mode & 3) ||
		    (da->mr_mode == 0 && FI_VERSION_LT(version,
						       FI_VERSION(1, 5))))
			REJECT("legacy memory registration modes are not "
			       "supported\n");
		if ((da->mr_mode & UETFI_MR_MODE) != UETFI_MR_MODE)
			REJECT("the application must support FI_MR_PROV_KEY\n");
		if (da->cq_data_size > uetfi_core_desc.cq_data_size)
			REJECT("cq_data_size too large\n");
		if (da->mr_cnt > UETFI_MAX_MR_CNT)
			REJECT("mr_cnt too large\n");
		if (da->caps & ~UETFI_CAPS)
			REJECT("unsupported domain caps\n");
		if (da->auth_key_size)
			REJECT("authorization keys are not supported\n");
	}

	ta = hints->tx_attr;
	if (ta) {
		if (ta->caps & ~UETFI_CAPS)
			REJECT("unsupported tx caps\n");
		if (ta->op_flags & ~UETFI_TX_OP_FLAGS)
			REJECT("unsupported tx op_flags\n");
		if (ta->msg_order || ta->comp_order)
			REJECT("ordering is not supported\n");
		if (ta->inject_size)
			REJECT("inject is not supported\n");
		if (ta->size > UETFI_MAX_QUEUE_SIZE)
			REJECT("tx size too large\n");
		if (ta->iov_limit > 1 || ta->rma_iov_limit > 1)
			REJECT("one iov per operation\n");
	}

	ra = hints->rx_attr;
	if (ra) {
		if (ra->caps & ~UETFI_CAPS)
			REJECT("unsupported rx caps\n");
		if (ra->op_flags)
			REJECT("unsupported rx op_flags\n");
		if (ra->msg_order || ra->comp_order)
			REJECT("ordering is not supported\n");
		if (ra->total_buffered_recv)
			REJECT("buffered receive is not supported\n");
		if (ra->size > UETFI_MAX_QUEUE_SIZE)
			REJECT("rx size too large\n");
		if (ra->iov_limit > 1)
			REJECT("one iov per operation\n");
	}
#undef REJECT
	return 0;
}

static int uetfi_parse_ipv4(const char *node, uint32_t *ipv4)
{
	struct addrinfo hints, *res;
	struct in_addr in;

	if (inet_pton(AF_INET, node, &in) == 1) {
		*ipv4 = ntohl(in.s_addr);
		return 0;
	}
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	if (getaddrinfo(node, NULL, &hints, &res))
		return -FI_ENODATA;
	*ipv4 = ntohl(((struct sockaddr_in *) res->ai_addr)->sin_addr.s_addr);
	freeaddrinfo(res);
	return 0;
}

static struct fi_info *uetfi_info_alloc(uint32_t version,
					const struct fi_info *hints,
					const struct uetfi_ifinfo *ifinfo,
					const uint8_t *dest)
{
	struct fi_info *info;
	struct uet_addr src;
	uint64_t caps;

	info = fi_allocinfo();
	if (!info)
		return NULL;

	caps = UETFI_CAPS;
	if (hints && hints->caps) {
		caps = hints->caps | FI_REMOTE_COMM;
		/* FI_RMA alone implies read and write; only write is here */
		if ((caps & FI_RMA) &&
		    !(caps & (FI_READ | FI_WRITE | FI_REMOTE_READ |
			      FI_REMOTE_WRITE)))
			caps |= FI_WRITE | FI_REMOTE_WRITE;
	}
	info->caps = caps;
	info->mode = 0;
	info->addr_format = FI_FORMAT_UNSPEC;

	info->src_addr = malloc(UETFI_ADDR_LEN);
	if (!info->src_addr)
		goto err;
	uetfi_addr_default(&src, ifinfo->ipv4);
	uetfi_addr_encode(&src, info->src_addr);
	info->src_addrlen = UETFI_ADDR_LEN;
	if (dest) {
		info->dest_addr = malloc(UETFI_ADDR_LEN);
		if (!info->dest_addr)
			goto err;
		memcpy(info->dest_addr, dest, UETFI_ADDR_LEN);
		info->dest_addrlen = UETFI_ADDR_LEN;
	}

	info->tx_attr->caps = caps & (UETFI_TX_CAPS | FI_REMOTE_COMM);
	info->tx_attr->mode = 0;
	info->tx_attr->op_flags = FI_DELIVERY_COMPLETE;
	if (hints && hints->tx_attr && hints->tx_attr->op_flags)
		info->tx_attr->op_flags = hints->tx_attr->op_flags;
	info->tx_attr->msg_order = 0;
	info->tx_attr->comp_order = 0;
	info->tx_attr->inject_size = 0;
	info->tx_attr->size = (hints && hints->tx_attr &&
			       hints->tx_attr->size) ?
			      hints->tx_attr->size : UETFI_DEF_TX_SIZE;
	info->tx_attr->iov_limit = 1;
	info->tx_attr->rma_iov_limit = 1;
	info->tx_attr->tclass = FI_TC_UNSPEC;

	info->rx_attr->caps = caps & (UETFI_RX_CAPS | FI_REMOTE_COMM);
	info->rx_attr->mode = 0;
	info->rx_attr->op_flags = 0;
	info->rx_attr->msg_order = 0;
	info->rx_attr->comp_order = 0;
	info->rx_attr->total_buffered_recv = 0;
	info->rx_attr->size = (hints && hints->rx_attr &&
			       hints->rx_attr->size) ?
			      hints->rx_attr->size : UETFI_DEF_RX_SIZE;
	info->rx_attr->iov_limit = 1;

	info->ep_attr->type = FI_EP_RDM;
	info->ep_attr->protocol = FI_PROTO_UNSPEC;
	info->ep_attr->protocol_version = 1;
	info->ep_attr->max_msg_size = UETFI_MAX_MSG_SIZE;
	info->ep_attr->msg_prefix_size = 0;
	info->ep_attr->max_order_raw_size = 0;
	info->ep_attr->max_order_war_size = 0;
	info->ep_attr->max_order_waw_size = 0;
	info->ep_attr->mem_tag_format = 0;
	info->ep_attr->tx_ctx_cnt = 1;
	info->ep_attr->rx_ctx_cnt = 1;

	info->domain_attr->name = strdup(ifinfo->name);
	info->domain_attr->threading = FI_THREAD_DOMAIN;
	info->domain_attr->control_progress = FI_PROGRESS_MANUAL;
	info->domain_attr->data_progress = FI_PROGRESS_MANUAL;
	info->domain_attr->resource_mgmt = FI_RM_ENABLED;
	info->domain_attr->av_type = (hints && hints->domain_attr) ?
				     hints->domain_attr->av_type :
				     FI_AV_UNSPEC;
	info->domain_attr->mr_mode = UETFI_MR_MODE;
	info->domain_attr->mr_key_size = UETFI_MR_KEY_SIZE;
	info->domain_attr->cq_data_size = uetfi_core_desc.cq_data_size;
	info->domain_attr->cq_cnt = 64;
	info->domain_attr->ep_cnt = 1;
	info->domain_attr->tx_ctx_cnt = 1;
	info->domain_attr->rx_ctx_cnt = 1;
	info->domain_attr->max_ep_tx_ctx = 1;
	info->domain_attr->max_ep_rx_ctx = 1;
	info->domain_attr->max_ep_stx_ctx = 0;
	info->domain_attr->max_ep_srx_ctx = 0;
	info->domain_attr->cntr_cnt = 0;
	info->domain_attr->mr_iov_limit = 1;
	info->domain_attr->caps = FI_REMOTE_COMM;
	info->domain_attr->mode = 0;
	info->domain_attr->max_err_data = 0;
	info->domain_attr->mr_cnt = (hints && hints->domain_attr &&
				     hints->domain_attr->mr_cnt) ?
				    hints->domain_attr->mr_cnt :
				    UETFI_DEF_MR_CNT;
	info->domain_attr->tclass = FI_TC_UNSPEC;
	info->domain_attr->max_ep_auth_key = 0;
	if (FI_VERSION_GE(version, FI_VERSION(2, 0)))
		info->domain_attr->max_group_id = 0;

	/* prov_name is filled in by the libfabric core */
	info->fabric_attr->name = strdup(UETFI_FABRIC_NAME);
	info->fabric_attr->prov_version = UETFI_PROV_VERSION;

	if (!info->domain_attr->name || !info->fabric_attr->name)
		goto err;
	return info;
err:
	fi_freeinfo(info);
	return NULL;
}

/* collects one fi_info per usable interface */
struct uetfi_getinfo_walk {
	uint32_t version;
	const struct fi_info *hints;
	const uint8_t *dest;
	bool want_src;
	uint32_t src_ip;
	struct fi_info *head, *tail;
};

static int uetfi_getinfo_one(const struct uetfi_ifinfo *ifinfo, void *arg)
{
	struct uetfi_getinfo_walk *w = arg;
	struct fi_info *cur;

	if (w->want_src && ifinfo->ipv4 != w->src_ip)
		return 0;
	cur = uetfi_info_alloc(w->version, w->hints, ifinfo, w->dest);
	if (!cur)
		return -FI_ENOMEM;
	if (w->tail)
		w->tail->next = cur;
	else
		w->head = cur;
	w->tail = cur;
	return 0;
}

static int uetfi_getinfo(uint32_t version, const char *node,
			 const char *service, uint64_t flags,
			 const struct fi_info *hints, struct fi_info **info)
{
	struct uetfi_ifinfo ifinfo;
	struct uetfi_getinfo_walk walk;
	struct fi_info *cur;
	struct uet_addr addr;
	uint8_t dest_wire[UETFI_ADDR_LEN], *dest = NULL;
	const char *ifname = NULL;
	uint32_t src_ip = 0;
	bool want_src = false;
	int ret;

	(void) service;
	*info = NULL;

	if (FI_VERSION_LT(version, FI_VERSION(1, 5)))
		return -FI_ENODATA;

	ret = uetfi_check_hints(version, hints);
	if (ret)
		return ret;

	if (node && (flags & FI_SOURCE)) {
		if (uetfi_parse_ipv4(node, &src_ip))
			return -FI_ENODATA;
		want_src = true;
	} else if (node) {
		uint32_t ip;

		if (uetfi_parse_ipv4(node, &ip))
			return -FI_ENODATA;
		uetfi_addr_default(&addr, ip);
		uetfi_addr_encode(&addr, dest_wire);
		dest = dest_wire;
	}
	if (hints && hints->src_addr) {
		if (uetfi_addr_decode(hints->src_addr, hints->src_addrlen,
				      &addr) || uet_addr_is_ipv6(&addr))
			return -FI_ENODATA;
		src_ip = addr.fa.v4;
		want_src = true;
	}
	if (hints && hints->dest_addr) {
		if (uetfi_addr_decode(hints->dest_addr, hints->dest_addrlen,
				      &addr))
			return -FI_ENODATA;
		memcpy(dest_wire, hints->dest_addr, UETFI_ADDR_LEN);
		dest = dest_wire;
	}

	if (hints && hints->domain_attr && hints->domain_attr->name)
		ifname = hints->domain_attr->name;
	else
		ifname = uetfi_default_ifname();

	if (ifname) {
		ret = uetfi_if_query(ifname, &ifinfo);
		if (ret) {
			UETFI_INFO(FI_LOG_CORE, "%s is not usable (%s)\n",
				   ifname, uetfi_core_desc.if_usable);
			return -FI_ENODATA;
		}
		if (want_src && ifinfo.ipv4 != src_ip)
			return -FI_ENODATA;
		cur = uetfi_info_alloc(version, hints, &ifinfo, dest);
		if (!cur)
			return -FI_ENOMEM;
		*info = cur;
		return 0;
	}

	/* none named: one fi_info per usable interface */
	memset(&walk, 0, sizeof(walk));
	walk.version = version;
	walk.hints = hints;
	walk.dest = dest;
	walk.want_src = want_src;
	walk.src_ip = src_ip;
	ret = uetfi_if_foreach(uetfi_getinfo_one, &walk);
	if (ret) {
		fi_freeinfo(walk.head);
		return ret;
	}
	*info = walk.head;
	return walk.head ? 0 : -FI_ENODATA;
}

/*******************************************************************
 * fabric
 *******************************************************************/

static int uetfi_fabric_close(struct fid *fid)
{
	struct uetfi_fabric *fab =
		container_of(fid, struct uetfi_fabric, fabric_fid.fid);

	if (fab->refs)
		return -FI_EBUSY;
	free(fab);
	return 0;
}

static struct fi_ops uetfi_fabric_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = uetfi_fabric_close,
	.bind = UETFI_NOSYS(int (*)(struct fid *, struct fid *, uint64_t)),
	.control = UETFI_NOSYS(int (*)(struct fid *, int, void *)),
	.ops_open = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
					void **, void *)),
	.tostr = UETFI_NOSYS(int (*)(const struct fid *, char *, size_t)),
	.ops_set = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
				       void *, void *)),
};

static int uetfi_domain2(struct fid_fabric *fabric, struct fi_info *info,
			 struct fid_domain **dom, uint64_t flags,
			 void *context)
{
	if (flags)
		return -FI_EBADFLAGS;
	return uetfi_domain_open(fabric, info, dom, context);
}

static struct fi_ops_fabric uetfi_fabric_ops = {
	.size = sizeof(struct fi_ops_fabric),
	.domain = uetfi_domain_open,
	.passive_ep = UETFI_NOSYS(int (*)(struct fid_fabric *, struct fi_info *,
					  struct fid_pep **, void *)),
	.eq_open = UETFI_NOSYS(int (*)(struct fid_fabric *,
				       struct fi_eq_attr *, struct fid_eq **,
				       void *)),
	.wait_open = UETFI_NOSYS(int (*)(struct fid_fabric *,
					 struct fi_wait_attr *,
					 struct fid_wait **)),
	.trywait = UETFI_NOSYS(int (*)(struct fid_fabric *, struct fid **,
				       int)),
	.domain2 = uetfi_domain2,
};

static int uetfi_fabric_open(struct fi_fabric_attr *attr,
			     struct fid_fabric **fabric, void *context)
{
	struct uetfi_fabric *fab;

	if (attr && attr->name && strcmp(attr->name, UETFI_FABRIC_NAME))
		return -FI_ENODATA;

	fab = calloc(1, sizeof(*fab));
	if (!fab)
		return -FI_ENOMEM;
	fab->fabric_fid.fid.fclass = FI_CLASS_FABRIC;
	fab->fabric_fid.fid.context = context;
	fab->fabric_fid.fid.ops = &uetfi_fabric_fi_ops;
	fab->fabric_fid.ops = &uetfi_fabric_ops;
	fab->fabric_fid.api_version = attr ? attr->api_version :
					     FI_VERSION(FI_MAJOR_VERSION,
							FI_MINOR_VERSION);
	*fabric = &fab->fabric_fid;
	return 0;
}

static void uetfi_cleanup(void)
{
}

struct fi_provider uetfi_prov = {
	.version = UETFI_PROV_VERSION,
	.fi_version = FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
	.name = UETFI_PROV_NAME,
	.getinfo = uetfi_getinfo,
	.fabric = uetfi_fabric_open,
	.cleanup = uetfi_cleanup,
};

__attribute__((visibility("default")))
struct fi_provider *fi_prov_ini(void)
{
	fi_param_define(&uetfi_prov, "ifname", FI_PARAM_STRING,
			"Netdev (ibverbs device for an ernic build) to use "
			"when the application does not name one in "
			"domain_attr->name (default: $UET_IFNAME, else every "
			"usable Ethernet interface; $UET_ERNIC_DEVICE, else "
			"every ionic device with an engine)");
	fi_param_define(&uetfi_prov, "rudi", FI_PARAM_BOOL,
			"Mark remotely writable regions IDEMPOTENT_SAFE and "
			"send writes without immediate data as connectionless "
			"RUDI, so targets keep no state for initiators "
			"(default: true)");
	fi_param_define(&uetfi_prov, "tx_timeout", FI_PARAM_INT,
			"PDS retransmit timeout in milliseconds, exported as "
			"UET_PDS_TX_TIMEOUT (default: $UET_PDS_TX_TIMEOUT, "
			"else 200)");
	fi_param_define(&uetfi_prov, "progress_burst", FI_PARAM_INT,
			"Progress calls into the reference provider per CQ "
			"read; each handles at most one received packet "
			"(default: 64)");

	fi_param_define(&uetfi_prov, "segment_size", FI_PARAM_SIZE_T,
			"Writes are sent in segments of this many bytes "
			"(default: 16384, 1048576 over an ernic engine)");
	fi_param_define(&uetfi_prov, "max_segments", FI_PARAM_INT,
			"Write segments in flight per endpoint. The reference "
			"provider has no flow control for RUDI, so this bounds "
			"the burst a target has to absorb (default: 2, 4 over "
			"an ernic engine)");
	fi_param_define(&uetfi_prov, "tx_retries", FI_PARAM_INT,
			"PDS retransmissions before a write fails, exported "
			"as UET_PDS_MAX_TX_RETRIES. A target must make progress "
			"at least once per tx_timeout * tx_retries (default: "
			"$UET_PDS_MAX_TX_RETRIES, else 25)");

	uetfi_params.segment_size = uetfi_core_desc.segment_size;
	uetfi_params.max_segments = uetfi_core_desc.max_segments;
	fi_param_get_str(&uetfi_prov, "ifname", &uetfi_params.ifname);
	fi_param_get_bool(&uetfi_prov, "rudi", &uetfi_params.rudi);
	fi_param_get_int(&uetfi_prov, "tx_timeout",
			 &uetfi_params.tx_timeout_ms);
	fi_param_get_int(&uetfi_prov, "progress_burst",
			 &uetfi_params.progress_burst);
	fi_param_get_size_t(&uetfi_prov, "segment_size",
			    &uetfi_params.segment_size);
	fi_param_get_int(&uetfi_prov, "max_segments",
			 &uetfi_params.max_segments);
	fi_param_get_int(&uetfi_prov, "tx_retries", &uetfi_params.tx_retries);
	if (uetfi_params.progress_burst < 1)
		uetfi_params.progress_burst = 1;
	if (uetfi_params.segment_size < 1024)
		uetfi_params.segment_size = 1024;
	if (uetfi_params.max_segments < 1)
		uetfi_params.max_segments = 1;
	if (uetfi_params.max_segments > UETFI_MAX_QUEUE_SIZE)
		uetfi_params.max_segments = UETFI_MAX_QUEUE_SIZE;

	return &uetfi_prov;
}
