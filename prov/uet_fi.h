/*
 * libfabric provider "uet" over the UEC reference provider.
 *
 * This glue is compiled against the installed libfabric headers and is
 * loaded by libfabric as a DL provider (libuet-fi.so). It maps the
 * libfabric object model onto the SES API of the reference provider
 * (see uet_core.h), which is compiled into the same library.
 *
 * Scope: RDM endpoints with RMA write (initiator) and remote write
 * (target), manual progress, FI_THREAD_SAFE. See README.md.
 *
 * Threads: every call on a domain or on an object of it runs under the
 * domain's lock, with one exception. fi_av_insert() of a new peer resolves
 * its next hop in the core (ARP, which can take a while) without the
 * lock, so other threads write and read completions meanwhile.
 */

#ifndef _UET_FI_H_
#define _UET_FI_H_

#include <net/if.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rdma/fabric.h>
#include <rdma/fi_atomic.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_collective.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>
#include <rdma/providers/fi_log.h>
#include <rdma/providers/fi_prov.h>

#include "uet_core.h"
#include "../uet_payload.h"

#define UETFI_PROV_NAME		"uet"
#define UETFI_FABRIC_NAME	"uet"
#define UETFI_PROV_VERSION	FI_VERSION(0, 1)

/* defaults and limits advertised in fi_info */
#define UETFI_DEF_TX_SIZE	256
#define UETFI_DEF_RX_SIZE	256
#define UETFI_MAX_QUEUE_SIZE	4096
#define UETFI_DEF_MR_CNT	4096
#define UETFI_MAX_MR_CNT	65536
#define UETFI_MAX_MSG_SIZE	((size_t) 0xfffffffe)	/* UET_MAX_MSG_SIZE */
#define UETFI_MR_KEY_SIZE	sizeof(uint64_t)
#define UETFI_CQ_DATA_SIZE	sizeof(uint64_t)	/* the most a core offers */
#define UETFI_CQ_MAX_EPS	8
/* a netdev name, or an ibverbs device name, which can be longer */
#define UETFI_NAME_MAX		64

#define UETFI_CAPS	(FI_RMA | FI_WRITE | FI_REMOTE_WRITE | FI_REMOTE_COMM)
#define UETFI_TX_CAPS	(FI_RMA | FI_WRITE)
#define UETFI_RX_CAPS	(FI_RMA | FI_REMOTE_WRITE)
#define UETFI_MR_MODE	(FI_MR_PROV_KEY)
#define UETFI_TX_OP_FLAGS (FI_COMPLETION | FI_INJECT_COMPLETE | \
			   FI_TRANSMIT_COMPLETE | FI_DELIVERY_COMPLETE)

/*
 * fi_getopt(&ep->fid, FI_OPT_ENDPOINT, FI_UET_OPT_CLOSE_DISCARDS, &b, &len)
 * with bool b: whether fi_close() of the endpoint discards the writes it
 * has outstanding, so none of them lands afterwards. False over a core that
 * cannot take back what it has handed over: the reference core with
 * UET_PDS=sng, or a rocm-ernic device older than ABI version 2 or with
 * pds=sng. A consumer that relies on cutting writes off by closing the
 * endpoint checks it up front. See README.md.
 */
#define FI_UET_OPT_CLOSE_DISCARDS ((int)(FI_PROV_SPECIFIC | 0x5545U))

/*
 * fi_getopt(&ep->fid, FI_OPT_ENDPOINT, FI_UET_OPT_CANCEL_DISCARDS, &b, &len)
 * with bool b: whether fi_cancel() of an outstanding write discards it.
 * When true, fi_cancel(&ep->fid, context) returns 0 once nothing of the
 * write will be sent again (the parts already delivered stay), and the
 * write completes with FI_ECANCELED; -FI_ENOENT for a write that has
 * completed, or none with that context. False on the cores that cannot
 * take one operation back: the reference core with UET_PDS=sng, a
 * rocm-ernic device without UET_ERNIC_CAP_ABORT_OP. See README.md.
 */
#define FI_UET_OPT_CANCEL_DISCARDS ((int)(FI_PROV_SPECIFIC | 0x5543U))

/*
 * fi_control(&mr->fid, FI_UET_MR_REKEY, &key) with uint64_t key (or NULL):
 * give the region a new key, returned in key and by fi_mr_key() from then
 * on. The old key is dead when the call returns, as if the region had been
 * closed: a write with it lands nothing and fails at the initiator, also a
 * late duplicate of one that completed, and a write under way with it
 * fails at its next packet. The region keeps its memory, registration and
 * binding, so this costs far less than fi_close() and fi_mr_reg(), and
 * uses up nothing. -FI_ENOSYS from a core that cannot (a rocm-ernic device
 * without the REKEY command): close and register again instead.
 */
#define FI_UET_MR_REKEY ((int)(FI_PROV_SPECIFIC | 0x554bU))

/*
 * Endpoint address, as returned by fi_getname() and accepted by
 * fi_av_insert(). It is self-contained and byte-order independent:
 *
 *   0      'U' (0x55)
 *   1      format version, 1
 *   2      UET address flags (UET_ADDR_*: valid bits, mode, IPv6)
 *   3      reserved, 0
 *   4-5    FEP capabilities (UET_FEP_CAP_*), big endian
 *   6-7    PIDonFEP, big endian
 *   8-9    resource index, big endian
 *   10-11  number of resource indices, big endian
 *   12-15  initiator id, big endian
 *   16-31  fabric address: IPv4 in network order in 16-19 and zero in
 *          20-31, or the IPv6 address
 */
#define UETFI_ADDR_LEN		32
#define UETFI_ADDR_MAGIC	0x55
#define UETFI_ADDR_VERSION	1

#ifndef container_of
#define container_of(ptr, type, field) \
	((type *) ((char *) (ptr) - offsetof(type, field)))
#endif

#define UETFI_WARN(subsys, ...) FI_WARN(&uetfi_prov, subsys, __VA_ARGS__)
#define UETFI_INFO(subsys, ...) FI_INFO(&uetfi_prov, subsys, __VA_ARGS__)

extern struct fi_provider uetfi_prov;

struct uetfi_params {
	char *ifname;		/* FI_UET_IFNAME */
	int rudi;		/* FI_UET_RUDI */
	int tx_timeout_ms;	/* FI_UET_TX_TIMEOUT, <0 => unset */
	int progress_burst;	/* FI_UET_PROGRESS_BURST */
	size_t segment_size;	/* FI_UET_SEGMENT_SIZE, 0 => the core's */
	int max_segments;	/* FI_UET_MAX_SEGMENTS */
	int tx_retries;
	char *rto;		/* FI_UET_RTO: adaptive or fixed */		/* FI_UET_TX_RETRIES, <0 => unset */
	char *encap;		/* FI_UET_ENCAP, NULL => $UET_ENCAP */
	size_t max_payload;	/* FI_UET_MAX_PAYLOAD, 0 => from the MTU */
};
extern struct uetfi_params uetfi_params;

/* intrusive doubly linked list */
struct uetfi_dlist {
	struct uetfi_dlist *prev, *next;
};

static inline void uetfi_dlist_init(struct uetfi_dlist *h)
{
	h->prev = h->next = h;
}

static inline void uetfi_dlist_insert_tail(struct uetfi_dlist *e,
					   struct uetfi_dlist *h)
{
	e->next = h;
	e->prev = h->prev;
	h->prev->next = e;
	h->prev = e;
}

static inline void uetfi_dlist_remove(struct uetfi_dlist *e)
{
	e->prev->next = e->next;
	e->next->prev = e->prev;
	e->prev = e->next = e;
}

#define uetfi_dlist_foreach(h, it) \
	for ((it) = (h)->next; (it) != (h); (it) = (it)->next)

/*
 * What backs a domain: a netdev for the reference core, an ionic RDMA
 * device for the ernic core.
 */
struct uetfi_ifinfo {
	char name[UETFI_NAME_MAX];
	uint32_t ipv4;		/* host byte order */
	unsigned int mtu;	/* 0 if unknown */
};

struct uetfi_fabric {
	struct fid_fabric fabric_fid;
	int refs;		/* open domains */
};

struct uetfi_domain {
	struct fid_domain domain_fid;
	struct uetfi_fabric *fabric;
	struct uetfi_ifinfo ifinfo;
	/*
	 * The core keeps pointers to the fi_info it is given for the domain
	 * and for each endpoint, so these copies live as long as the object.
	 */
	struct fi_info *core_info;
	uet_domain_handle_t h;
	struct uetfi_ep *ep;		/* the domain's endpoint, if open */
	struct uetfi_dlist mr_list;	/* struct uetfi_mr */
	int refs;			/* av, cq, ep and mr objects */
	int abandoned;			/* core endpoints that would not close */
	pthread_mutex_t lock;		/* see "Threads" above */
};

static inline void uetfi_lock(struct uetfi_domain *dom)
{
	pthread_mutex_lock(&dom->lock);
}

static inline void uetfi_unlock(struct uetfi_domain *dom)
{
	pthread_mutex_unlock(&dom->lock);
}

struct uetfi_mr {
	struct fid_mr mr_fid;
	struct uetfi_domain *dom;
	uet_mr_handle_t h;
	struct uetfi_dlist entry;
	struct uetfi_ep *ep;		/* endpoint the region is bound to */
	bool enabled;
};

/*
 * A peer known to an AV. The core's address vector entry points at
 * 'addr' rather than copying it, so a peer stays allocated, at a fixed
 * address, until its core entry is removed.
 */
struct uetfi_peer {
	struct uet_addr addr;
	uet_addr_handle_t h;
	uint8_t wire[UETFI_ADDR_LEN];
	int refs;			/* AV slots naming this peer */
	struct uetfi_peer *next;
};

struct uetfi_av {
	struct fid_av av_fid;
	struct uetfi_domain *dom;
	enum fi_av_type type;
	struct uetfi_peer *peers;	/* list of distinct peers */
	struct uetfi_peer **table;	/* FI_AV_TABLE: slot -> peer */
	size_t table_len, table_cap;
	size_t *free_slots;
	size_t nfree;
	int refs;			/* bound endpoints */
};

struct uetfi_cq {
	struct fid_cq cq_fid;
	struct uetfi_domain *dom;
	enum fi_cq_format format;
	size_t entry_size;
	struct uetfi_ep *eps[UETFI_CQ_MAX_EPS];
	int neps;
};

/*
 * A write posted by the application. The reference provider transmits
 * every packet of a message at once and has no flow control for RUDI, so
 * a large write, or many, overruns the receiver and ends in retransmit
 * timeouts. A write is therefore sent as segments of segment_size bytes,
 * with at most max_segments segments in flight per endpoint, and
 * completes when all of its segments have.
 */
struct uetfi_op {
	void *context;
	const uint8_t *buf;
	size_t len;
	size_t posted;		/* bytes handed to the core */
	uint64_t addr, key;
	uint64_t data;
	struct uetfi_peer *peer;
	uint32_t segs_out;	/* segments in flight */
	int err;
	bool has_data;
	bool silent;		/* completion only if it fails */
	bool live;		/* posted, not finished */
	bool cancelled;		/* by fi_cancel() */
	struct uetfi_op *next;	/* pending queue or free list */
};

struct uetfi_ep {
	struct fid_ep ep_fid;
	struct uetfi_domain *dom;
	struct fi_info *core_info;
	uet_ep_handle_t h;
	struct uetfi_av *av;
	struct uetfi_cq *tx_cq;
	struct uetfi_cq *rx_cq;
	bool tx_selective;
	uint64_t tx_op_flags;
	size_t tx_size, rx_size;
	bool enabled;
	/* the core keeps pointers to the CQ attributes and fid */
	struct fi_cq_attr core_cq_attr;
	struct fid_cq core_cq_fid;
	uet_cq_handle_t core_tx_cq, core_rx_cq;

	/* writes: tx_size operations, each completing once */
	struct uetfi_op *ops;
	struct uetfi_op *op_free;
	struct uetfi_op *pend_head, *pend_tail;	/* segments left to post */
	size_t ops_outstanding;		/* posted, completion not yet read */
	size_t segs_inflight;
	size_t seg_size, max_segs;
	/* write completions waiting to be read, tx_size each */
	struct fi_cq_data_entry *comp;
	size_t comp_head, comp_count;
	struct fi_cq_err_entry *errs;
	size_t err_head, err_count;
};

/*
 * uet_fi_if_netdev.c (CORE=ref) or uet_fi_if_ernic.c (CORE=ernic): where
 * the core runs and which address it has there
 */
struct uetfi_core_desc {
	const char *name;		/* "ref" or "ernic" */
	const char *if_env;		/* names the interface to the core */
	const char *if_usable;		/* what makes an interface usable */
	const char *init_hint;		/* why uet_initialize can fail */
	size_t cq_data_size;		/* immediate data; 0 if the core has none */
	/*
	 * Default FI_UET_SEGMENT_SIZE: segment_pkts full packets of the
	 * Payload MTU, or segment_size bytes when segment_pkts is 0.
	 */
	unsigned int segment_pkts;
	size_t segment_size;
	int max_segments;		/* default FI_UET_MAX_SEGMENTS */
};
extern const struct uetfi_core_desc uetfi_core_desc;
unsigned int uetfi_payload_mtu(const struct uetfi_ifinfo *ifinfo);
size_t uetfi_segment_size(const struct uetfi_ifinfo *ifinfo);
int uetfi_if_query(const char *name, struct uetfi_ifinfo *ifinfo);
int uetfi_if_foreach(int (*fn)(const struct uetfi_ifinfo *, void *),
		     void *arg);

/* uet_fi_init.c */
extern pthread_mutex_t uetfi_env_lock;	/* held while if_env is changed */
void uetfi_addr_encode(const struct uet_addr *addr, uint8_t *wire);
int uetfi_addr_decode(const void *wire, size_t len, struct uet_addr *addr);
void uetfi_addr_default(struct uet_addr *addr, uint32_t ipv4);
const char *uetfi_addr_str(const void *wire, size_t len, char *buf,
			   size_t *buflen);
int uetfi_core_get(const char *ifname, uet_handle_t *h);
void uetfi_core_put(void);
int uetfi_core_claim_ep(void);
void uetfi_core_release_ep(void);
ssize_t uetfi_nosys(void);

/*
 * Unsupported operations. Every slot of an ops table must be callable,
 * because the libfabric inline wrappers call through them unchecked.
 */
#define UETFI_NOSYS(type) ((type) (void (*)(void)) uetfi_nosys)

/* uet_fi_domain.c */
int uetfi_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		      struct fid_domain **domain, void *context);
int uetfi_mr_attach(struct uetfi_mr *mr, struct uetfi_ep *ep);
int uetfi_mr_enable_bound(struct uetfi_ep *ep);
void uetfi_mr_detach_ep(struct uetfi_ep *ep);
struct uetfi_peer *uetfi_av_peer(struct uetfi_av *av, fi_addr_t addr);

/* uet_fi_cq.c */
int uetfi_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		  struct fid_cq **cq, void *context);
int uetfi_cq_attach_ep(struct uetfi_cq *cq, struct uetfi_ep *ep);
void uetfi_cq_detach_ep(struct uetfi_cq *cq, struct uetfi_ep *ep);
void uetfi_ep_progress(struct uetfi_ep *ep);
void uetfi_ep_drain_rx(struct uetfi_ep *ep);

/* uet_fi_ep.c */
void uetfi_ep_harvest(struct uetfi_ep *ep);
void uetfi_ep_post(struct uetfi_ep *ep);
int uetfi_endpoint(struct fid_domain *domain, struct fi_info *info,
		   struct fid_ep **ep, void *context);
int uetfi_endpoint2(struct fid_domain *domain, struct fi_info *info,
		    struct fid_ep **ep, uint64_t flags, void *context);

#endif /* _UET_FI_H_ */
