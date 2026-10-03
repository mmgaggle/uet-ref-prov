/*
 * Domain, memory registration and address vector.
 */

#include <stdlib.h>
#include <string.h>

#include "uet_fi.h"

/*******************************************************************
 * memory regions
 *
 * The reference provider associates a region with one endpoint
 * (FI_MR_ENDPOINT): registered, then bound, then enabled. The libfabric
 * application does not have to do that here. A region registered before
 * the domain's endpoint exists is bound when the endpoint is opened and
 * enabled when the endpoint is; one registered later is bound and enabled
 * at once. fi_mr_bind() to the endpoint and fi_mr_enable() are accepted
 * as well.
 *
 * Keys are assigned by the provider (FI_MR_PROV_KEY). Remote addresses
 * are offsets from the start of the region (no FI_MR_VIRT_ADDR).
 *******************************************************************/

int uetfi_mr_attach(struct uetfi_mr *mr, struct uetfi_ep *ep)
{
	int ret;

	if (mr->ep == ep)
		return 0;
	if (mr->ep)
		return -FI_EBUSY;

	ret = uet_ep_bind_mr(ep->h, mr->h, UET_FLAGS_NONE);
	if (ret)
		return ret;
	mr->ep = ep;
	if (ep->enabled) {
		ret = uet_mr_enable(mr->h);
		if (ret)
			return ret;
		mr->enabled = true;
	}
	return 0;
}

/* enable every region bound to the endpoint that is not enabled yet */
int uetfi_mr_enable_bound(struct uetfi_ep *ep)
{
	struct uetfi_dlist *it;
	struct uetfi_mr *mr;
	int ret;

	uetfi_dlist_foreach(&ep->dom->mr_list, it) {
		mr = container_of(it, struct uetfi_mr, entry);
		if (mr->ep != ep || mr->enabled)
			continue;
		ret = uet_mr_enable(mr->h);
		if (ret)
			return ret;
		mr->enabled = true;
	}
	return 0;
}

/* the core returned the endpoint's regions to the registered state */
void uetfi_mr_detach_ep(struct uetfi_ep *ep)
{
	struct uetfi_dlist *it;
	struct uetfi_mr *mr;

	uetfi_dlist_foreach(&ep->dom->mr_list, it) {
		mr = container_of(it, struct uetfi_mr, entry);
		if (mr->ep == ep) {
			mr->ep = NULL;
			mr->enabled = false;
		}
	}
}

static int uetfi_mr_close(struct fid *fid)
{
	struct uetfi_mr *mr = container_of(fid, struct uetfi_mr, mr_fid.fid);
	int ret;

	if (mr->enabled) {
		ret = uet_mr_disable(mr->h);
		if (ret)
			return ret;
		mr->enabled = false;
	}
	ret = uet_mr_close(mr->h);
	if (ret)
		return ret;
	uetfi_dlist_remove(&mr->entry);
	mr->dom->refs--;
	free(mr);
	return 0;
}

static int uetfi_mr_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct uetfi_mr *mr = container_of(fid, struct uetfi_mr, mr_fid.fid);
	struct uetfi_ep *ep;

	if (flags)
		return -FI_EBADFLAGS;
	if (bfid->fclass != FI_CLASS_EP)
		return -FI_EINVAL;
	ep = container_of(bfid, struct uetfi_ep, ep_fid.fid);
	if (ep->dom != mr->dom)
		return -FI_EINVAL;
	return uetfi_mr_attach(mr, ep);
}

static int uetfi_mr_control(struct fid *fid, int command, void *arg)
{
	struct uetfi_mr *mr = container_of(fid, struct uetfi_mr, mr_fid.fid);
	int ret;

	(void) arg;
	if (command != FI_ENABLE)
		return -FI_ENOSYS;
	/* an unbound region is enabled along with the domain's endpoint */
	if (!mr->ep || mr->enabled || !mr->ep->enabled)
		return 0;
	ret = uet_mr_enable(mr->h);
	if (!ret)
		mr->enabled = true;
	return ret;
}

static int uetfi_mr_close_op(struct fid *fid)
{
	struct uetfi_domain *dom =
		container_of(fid, struct uetfi_mr, mr_fid.fid)->dom;
	int ret;

	uetfi_lock(dom);
	ret = uetfi_mr_close(fid);
	uetfi_unlock(dom);
	return ret;
}

static int uetfi_mr_bind_op(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct uetfi_domain *dom =
		container_of(fid, struct uetfi_mr, mr_fid.fid)->dom;
	int ret;

	uetfi_lock(dom);
	ret = uetfi_mr_bind(fid, bfid, flags);
	uetfi_unlock(dom);
	return ret;
}

static int uetfi_mr_control_op(struct fid *fid, int command, void *arg)
{
	struct uetfi_domain *dom =
		container_of(fid, struct uetfi_mr, mr_fid.fid)->dom;
	int ret;

	uetfi_lock(dom);
	ret = uetfi_mr_control(fid, command, arg);
	uetfi_unlock(dom);
	return ret;
}

static struct fi_ops uetfi_mr_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = uetfi_mr_close_op,
	.bind = uetfi_mr_bind_op,
	.control = uetfi_mr_control_op,
	.ops_open = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
					void **, void *)),
	.tostr = UETFI_NOSYS(int (*)(const struct fid *, char *, size_t)),
	.ops_set = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
				       void *, void *)),
};

static int uetfi_mr_regattr_locked(struct uetfi_domain *dom,
				   const struct fi_mr_attr *attr,
				   uint64_t flags, struct fid_mr **mr_fid)
{
	struct uetfi_mr *mr;
	uint64_t key_flags = UET_MR_KEY_NONE;
	int ret;

	if (flags)
		return -FI_EBADFLAGS;
	if (attr->iov_count != 1 || !attr->mr_iov)
		return -FI_EINVAL;
	if (attr->offset)
		return -FI_EINVAL;
	if (attr->iface != FI_HMEM_SYSTEM)
		return -FI_ENOSYS;
	if (attr->auth_key_size)
		return -FI_ENOSYS;

	/*
	 * RUDI places each packet independently and may place one more than
	 * once, so it may only target regions marked IDEMPOTENT_SAFE.
	 */
	if (uetfi_params.rudi && (attr->access & FI_REMOTE_WRITE))
		key_flags |= UET_MR_KEY_IDEMPOTENT_SAFE;

	mr = calloc(1, sizeof(*mr));
	if (!mr)
		return -FI_ENOMEM;
	ret = uet_mr_reg(dom->h, attr->mr_iov[0].iov_base,
			 attr->mr_iov[0].iov_len, attr->access, key_flags,
			 UET_FLAGS_NONE, attr->context, &mr->h);
	if (ret) {
		free(mr);
		return ret;
	}

	mr->dom = dom;
	mr->mr_fid.fid.fclass = FI_CLASS_MR;
	mr->mr_fid.fid.context = attr->context;
	mr->mr_fid.fid.ops = &uetfi_mr_fi_ops;
	mr->mr_fid.mem_desc = mr;
	mr->mr_fid.key = uet_mr_key(mr->h);
	uetfi_dlist_insert_tail(&mr->entry, &dom->mr_list);
	dom->refs++;

	if (dom->ep) {
		ret = uetfi_mr_attach(mr, dom->ep);
		if (ret) {
			uetfi_mr_close(&mr->mr_fid.fid);
			return ret;
		}
	}

	*mr_fid = &mr->mr_fid;
	return 0;
}

static int uetfi_mr_regattr(struct fid *fid, const struct fi_mr_attr *attr,
			    uint64_t flags, struct fid_mr **mr_fid)
{
	struct uetfi_domain *dom;
	int ret;

	if (fid->fclass != FI_CLASS_DOMAIN || !attr || !mr_fid)
		return -FI_EINVAL;
	dom = container_of(fid, struct uetfi_domain, domain_fid.fid);
	uetfi_lock(dom);
	ret = uetfi_mr_regattr_locked(dom, attr, flags, mr_fid);
	uetfi_unlock(dom);
	return ret;
}

static int uetfi_mr_regv(struct fid *fid, const struct iovec *iov,
			 size_t count, uint64_t access, uint64_t offset,
			 uint64_t requested_key, uint64_t flags,
			 struct fid_mr **mr, void *context)
{
	struct fi_mr_attr attr;

	(void) requested_key;	/* FI_MR_PROV_KEY */
	memset(&attr, 0, sizeof(attr));
	attr.mr_iov = iov;
	attr.iov_count = count;
	attr.access = access;
	attr.offset = offset;
	attr.context = context;
	attr.iface = FI_HMEM_SYSTEM;
	return uetfi_mr_regattr(fid, &attr, flags, mr);
}

static int uetfi_mr_reg(struct fid *fid, const void *buf, size_t len,
			uint64_t access, uint64_t offset,
			uint64_t requested_key, uint64_t flags,
			struct fid_mr **mr, void *context)
{
	struct iovec iov = {
		.iov_base = (void *) buf,
		.iov_len = len,
	};

	return uetfi_mr_regv(fid, &iov, 1, access, offset, requested_key,
			     flags, mr, context);
}

static struct fi_ops_mr uetfi_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = uetfi_mr_reg,
	.regv = uetfi_mr_regv,
	.regattr = uetfi_mr_regattr,
};

/*******************************************************************
 * address vector
 *
 * fi_av_insert() takes the 32-byte addresses fi_getname() returns.
 * Inserting a peer the first time resolves its next hop (the core asks
 * the kernel, which may have to ARP for it), so the same peer inserted
 * again shares that entry. That resolution runs without the domain's
 * lock. A target needs no entry for the initiators that write to it.
 *******************************************************************/

struct uetfi_peer *uetfi_av_peer(struct uetfi_av *av, fi_addr_t addr)
{
	if (addr == FI_ADDR_UNSPEC || addr == FI_ADDR_NOTAVAIL)
		return NULL;
	if (av->type == FI_AV_MAP)
		return (struct uetfi_peer *) (uintptr_t) addr;
	if (addr >= av->table_len)
		return NULL;
	return av->table[addr];
}

static void uetfi_peer_unref(struct uetfi_av *av, struct uetfi_peer *peer)
{
	struct uetfi_peer **pp;

	if (--peer->refs)
		return;
	for (pp = &av->peers; *pp; pp = &(*pp)->next) {
		if (*pp == peer) {
			*pp = peer->next;
			break;
		}
	}
	free(peer);
}

static struct uetfi_peer *uetfi_av_find(struct uetfi_av *av,
					 const uint8_t *wire)
{
	struct uetfi_peer *peer;

	for (peer = av->peers; peer; peer = peer->next)
		if (!memcmp(peer->wire, wire, UETFI_ADDR_LEN))
			break;
	return peer;
}

/* called with the domain's lock held; drops it while the core resolves a
 * new peer */
static int uetfi_av_insert_one(struct uetfi_av *av, const void *addr,
			       fi_addr_t *fi_addr)
{
	struct uetfi_domain *dom = av->dom;
	struct uetfi_peer *peer, *fresh;
	struct uet_addr ua;
	uint8_t wire[UETFI_ADDR_LEN];
	size_t slot;
	int ret;

	ret = uetfi_addr_decode(addr, UETFI_ADDR_LEN, &ua);
	if (ret)
		return ret;
	uetfi_addr_encode(&ua, wire);

	peer = uetfi_av_find(av, wire);
	if (!peer) {
		fresh = calloc(1, sizeof(*fresh));
		if (!fresh)
			return -FI_ENOMEM;
		fresh->addr = ua;
		memcpy(fresh->wire, wire, UETFI_ADDR_LEN);
		/*
		 * The core resolves the next hop here, which may wait on
		 * ARP; writes and completion reads on the domain go on
		 * meanwhile. The core keeps a pointer to fresh->addr, and
		 * fresh is nobody else's until it is on the list.
		 */
		uetfi_unlock(dom);
		ret = uet_av_insert(dom->h, &fresh->addr, &fresh->h);
		uetfi_lock(dom);
		if (ret) {
			free(fresh);
			return ret;
		}
		/* another thread may have inserted the same peer meanwhile */
		peer = uetfi_av_find(av, wire);
		if (peer) {
			uet_av_remove(fresh->h);
			free(fresh);
		} else {
			peer = fresh;
			peer->next = av->peers;
			av->peers = peer;
		}
	}

	if (av->type == FI_AV_MAP) {
		peer->refs++;
		*fi_addr = (fi_addr_t) (uintptr_t) peer;
		return 0;
	}

	if (av->nfree) {
		slot = av->free_slots[--av->nfree];
	} else {
		if (av->table_len == av->table_cap) {
			size_t cap = av->table_cap ? av->table_cap * 2 : 64;
			struct uetfi_peer **t;
			size_t *f;

			t = realloc(av->table, cap * sizeof(*t));
			if (!t)
				goto nomem;
			av->table = t;
			f = realloc(av->free_slots, cap * sizeof(*f));
			if (!f)
				goto nomem;
			av->free_slots = f;
			av->table_cap = cap;
		}
		slot = av->table_len++;
	}
	peer->refs++;
	av->table[slot] = peer;
	*fi_addr = slot;
	return 0;

nomem:
	if (!peer->refs) {
		peer->refs = 1;
		uet_av_remove(peer->h);
		uetfi_peer_unref(av, peer);
	}
	return -FI_ENOMEM;
}

static int uetfi_av_insert(struct fid_av *av_fid, const void *addr,
			   size_t count, fi_addr_t *fi_addr, uint64_t flags,
			   void *context)
{
	struct uetfi_av *av = container_of(av_fid, struct uetfi_av, av_fid);
	int *sync_err = (flags & FI_SYNC_ERR) ? context : NULL;
	fi_addr_t fa;
	size_t i;
	int ret, inserted = 0;

	if (flags & ~(FI_MORE | FI_SYNC_ERR))
		return -FI_EBADFLAGS;

	uetfi_lock(av->dom);
	for (i = 0; i < count; i++) {
		ret = uetfi_av_insert_one(av, (const uint8_t *) addr +
					  i * UETFI_ADDR_LEN, &fa);
		if (sync_err)
			sync_err[i] = ret;
		if (ret) {
			UETFI_WARN(FI_LOG_AV, "address %zu not inserted: %s\n",
				   i, fi_strerror(-ret));
			fa = FI_ADDR_NOTAVAIL;
		} else {
			inserted++;
		}
		if (fi_addr)
			fi_addr[i] = fa;
	}
	uetfi_unlock(av->dom);
	return inserted;
}

static int uetfi_av_remove(struct fid_av *av_fid, fi_addr_t *fi_addr,
			   size_t count, uint64_t flags)
{
	struct uetfi_av *av = container_of(av_fid, struct uetfi_av, av_fid);
	struct uetfi_peer *peer;
	size_t i;
	int ret = 0, r;

	if (flags)
		return -FI_EBADFLAGS;

	uetfi_lock(av->dom);
	for (i = 0; i < count; i++) {
		peer = uetfi_av_peer(av, fi_addr[i]);
		if (!peer) {
			ret = -FI_EINVAL;
			continue;
		}
		if (peer->refs == 1) {
			/* fails while operations to the peer are in flight */
			r = uet_av_remove(peer->h);
			if (r) {
				ret = r;
				continue;
			}
		}
		if (av->type == FI_AV_TABLE) {
			av->table[fi_addr[i]] = NULL;
			av->free_slots[av->nfree++] = fi_addr[i];
		}
		uetfi_peer_unref(av, peer);
	}
	uetfi_unlock(av->dom);
	return ret;
}

static int uetfi_av_lookup(struct fid_av *av_fid, fi_addr_t fi_addr,
			   void *addr, size_t *addrlen)
{
	struct uetfi_av *av = container_of(av_fid, struct uetfi_av, av_fid);
	struct uetfi_peer *peer;

	uetfi_lock(av->dom);
	peer = uetfi_av_peer(av, fi_addr);
	if (!peer) {
		uetfi_unlock(av->dom);
		return -FI_EINVAL;
	}
	memcpy(addr, peer->wire, *addrlen < UETFI_ADDR_LEN ?
					 *addrlen : UETFI_ADDR_LEN);
	uetfi_unlock(av->dom);
	*addrlen = UETFI_ADDR_LEN;
	return 0;
}

static const char *uetfi_av_straddr(struct fid_av *av_fid, const void *addr,
				    char *buf, size_t *len)
{
	(void) av_fid;
	return uetfi_addr_str(addr, UETFI_ADDR_LEN, buf, len);
}

static int uetfi_av_close(struct fid *fid)
{
	struct uetfi_av *av = container_of(fid, struct uetfi_av, av_fid.fid);
	struct uetfi_domain *dom = av->dom;
	struct uetfi_peer *peer;
	int ret;

	uetfi_lock(dom);
	if (av->refs) {
		uetfi_unlock(dom);
		return -FI_EBUSY;
	}
	while ((peer = av->peers)) {
		ret = uet_av_remove(peer->h);
		if (ret && dom->abandoned) {
			/*
			 * An abandoned endpoint still counts an operation to
			 * this peer. The core's entry stays, and with it the
			 * address it points to.
			 */
			av->peers = peer->next;
			continue;
		}
		if (ret) {
			uetfi_unlock(dom);
			return ret;
		}
		av->peers = peer->next;
		free(peer);
	}
	dom->refs--;
	uetfi_unlock(dom);
	free(av->table);
	free(av->free_slots);
	free(av);
	return 0;
}

static struct fi_ops uetfi_av_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = uetfi_av_close,
	.bind = UETFI_NOSYS(int (*)(struct fid *, struct fid *, uint64_t)),
	.control = UETFI_NOSYS(int (*)(struct fid *, int, void *)),
	.ops_open = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
					void **, void *)),
	.tostr = UETFI_NOSYS(int (*)(const struct fid *, char *, size_t)),
	.ops_set = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
				       void *, void *)),
};

static struct fi_ops_av uetfi_av_ops = {
	.size = sizeof(struct fi_ops_av),
	.insert = uetfi_av_insert,
	.insertsvc = UETFI_NOSYS(int (*)(struct fid_av *, const char *,
					 const char *, fi_addr_t *, uint64_t,
					 void *)),
	.insertsym = UETFI_NOSYS(int (*)(struct fid_av *, const char *, size_t,
					 const char *, size_t, fi_addr_t *,
					 uint64_t, void *)),
	.remove = uetfi_av_remove,
	.lookup = uetfi_av_lookup,
	.straddr = uetfi_av_straddr,
	.av_set = UETFI_NOSYS(int (*)(struct fid_av *, struct fi_av_set_attr *,
				      struct fid_av_set **, void *)),
	.insert_auth_key = UETFI_NOSYS(int (*)(struct fid_av *, const void *,
					       size_t, fi_addr_t *, uint64_t)),
	.lookup_auth_key = UETFI_NOSYS(int (*)(struct fid_av *, fi_addr_t,
					       void *, size_t *)),
	.set_user_id = UETFI_NOSYS(int (*)(struct fid_av *, fi_addr_t,
					   fi_addr_t, uint64_t)),
};

static int uetfi_av_open(struct fid_domain *domain, struct fi_av_attr *attr,
			 struct fid_av **av_fid, void *context)
{
	struct uetfi_domain *dom =
		container_of(domain, struct uetfi_domain, domain_fid);
	struct uetfi_av *av;

	if (!attr)
		return -FI_EINVAL;
	if (attr->type != FI_AV_UNSPEC && attr->type != FI_AV_TABLE &&
	    attr->type != FI_AV_MAP)
		return -FI_EINVAL;
	if (attr->name || attr->rx_ctx_bits)
		return -FI_ENOSYS;
	if (attr->flags & ~FI_SYMMETRIC)
		return -FI_ENOSYS;

	av = calloc(1, sizeof(*av));
	if (!av)
		return -FI_ENOMEM;
	av->dom = dom;
	av->type = attr->type == FI_AV_MAP ? FI_AV_MAP : FI_AV_TABLE;
	av->av_fid.fid.fclass = FI_CLASS_AV;
	av->av_fid.fid.context = context;
	av->av_fid.fid.ops = &uetfi_av_fi_ops;
	av->av_fid.ops = &uetfi_av_ops;
	uetfi_lock(dom);
	dom->refs++;
	uetfi_unlock(dom);
	*av_fid = &av->av_fid;
	return 0;
}

/*******************************************************************
 * domain
 *******************************************************************/

static void uetfi_eq_cb(uet_handle_t h, struct fi_eq_entry *e)
{
	(void) h;
	(void) e;
}

static void uetfi_eq_err_cb(uet_handle_t h, struct fi_eq_err_entry *e)
{
	(void) h;
	(void) e;
}

static int uetfi_domain_close(struct fid *fid)
{
	struct uetfi_domain *dom =
		container_of(fid, struct uetfi_domain, domain_fid.fid);
	int ret;

	if (dom->refs)
		return -FI_EBUSY;
	if (dom->abandoned) {
		/*
		 * An endpoint the core would not close is still in its
		 * domain, so the core's domain cannot close, and its instance
		 * cannot go either: both are left, and the libfabric domain
		 * goes. The core keeps them for the life of the process.
		 */
		UETFI_WARN(FI_LOG_DOMAIN, "keeping the core domain: %d "
			   "endpoint(s) in it would not close\n",
			   dom->abandoned);
	} else {
		ret = uet_domain_close(dom->h);
		if (ret)
			return ret;
		uetfi_core_put();
	}
	pthread_mutex_destroy(&dom->lock);
	fi_freeinfo(dom->core_info);
	dom->fabric->refs--;
	free(dom);
	return 0;
}

static struct fi_ops uetfi_domain_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = uetfi_domain_close,
	.bind = UETFI_NOSYS(int (*)(struct fid *, struct fid *, uint64_t)),
	.control = UETFI_NOSYS(int (*)(struct fid *, int, void *)),
	.ops_open = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
					void **, void *)),
	.tostr = UETFI_NOSYS(int (*)(const struct fid *, char *, size_t)),
	.ops_set = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
				       void *, void *)),
};

static struct fi_ops_domain uetfi_domain_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = uetfi_av_open,
	.cq_open = uetfi_cq_open,
	.endpoint = uetfi_endpoint,
	.scalable_ep = UETFI_NOSYS(int (*)(struct fid_domain *,
					   struct fi_info *, struct fid_ep **,
					   void *)),
	.cntr_open = UETFI_NOSYS(int (*)(struct fid_domain *,
					 struct fi_cntr_attr *,
					 struct fid_cntr **, void *)),
	.poll_open = UETFI_NOSYS(int (*)(struct fid_domain *,
					 struct fi_poll_attr *,
					 struct fid_poll **)),
	.stx_ctx = UETFI_NOSYS(int (*)(struct fid_domain *, struct fi_tx_attr *,
				       struct fid_stx **, void *)),
	.srx_ctx = UETFI_NOSYS(int (*)(struct fid_domain *, struct fi_rx_attr *,
				       struct fid_ep **, void *)),
	.query_atomic = UETFI_NOSYS(int (*)(struct fid_domain *,
					    enum fi_datatype, enum fi_op,
					    struct fi_atomic_attr *,
					    uint64_t)),
	.query_collective = UETFI_NOSYS(int (*)(struct fid_domain *,
						enum fi_collective_op,
						struct fi_collective_attr *,
						uint64_t)),
	.endpoint2 = uetfi_endpoint2,
};

int uetfi_domain_open(struct fid_fabric *fabric, struct fi_info *info,
		      struct fid_domain **domain, void *context)
{
	struct uetfi_fabric *fab =
		container_of(fabric, struct uetfi_fabric, fabric_fid);
	struct uetfi_domain *dom;
	struct fi_info *core_info;
	struct uet_addr *src;
	const char *name = NULL;
	uet_handle_t h;
	size_t mr_cnt;
	int ret;

	if (!info || !info->domain_attr || !info->domain_attr->name)
		return -FI_EINVAL;
	name = info->domain_attr->name;

	dom = calloc(1, sizeof(*dom));
	if (!dom)
		return -FI_ENOMEM;
	pthread_mutex_init(&dom->lock, NULL);
	ret = uetfi_if_query(name, &dom->ifinfo);
	if (ret) {
		UETFI_WARN(FI_LOG_DOMAIN, "%s is not usable (%s)\n", name,
			   uetfi_core_desc.if_usable);
		free(dom);
		return -FI_ENODEV;
	}

	/*
	 * The core's own view of the domain: its endpoints take their
	 * address from src_addr (a struct uet_addr), and it requires
	 * FI_MR_ENDPOINT, which this glue handles for the application.
	 */
	core_info = fi_dupinfo(info);
	src = calloc(1, sizeof(*src));
	if (!core_info || !src) {
		ret = -FI_ENOMEM;
		goto err;
	}
	free(core_info->src_addr);
	free(core_info->dest_addr);
	core_info->dest_addr = NULL;
	core_info->dest_addrlen = 0;
	uetfi_addr_default(src, dom->ifinfo.ipv4);
	core_info->src_addr = src;
	core_info->src_addrlen = sizeof(*src);
	src = NULL;
	mr_cnt = info->domain_attr->mr_cnt ? info->domain_attr->mr_cnt :
					     UETFI_DEF_MR_CNT;
	if (mr_cnt > UETFI_MAX_MR_CNT)
		mr_cnt = UETFI_MAX_MR_CNT;
	core_info->domain_attr->mr_cnt = mr_cnt;
	core_info->domain_attr->mr_mode = FI_MR_ENDPOINT | FI_MR_PROV_KEY;

	ret = uetfi_core_get(dom->ifinfo.name, &h);
	if (ret)
		goto err;
	dom->domain_fid.fid.fclass = FI_CLASS_DOMAIN;
	dom->domain_fid.fid.context = context;
	dom->domain_fid.fid.ops = &uetfi_domain_fi_ops;
	dom->domain_fid.ops = &uetfi_domain_ops;
	dom->domain_fid.mr = &uetfi_mr_ops;
	ret = uet_domain(h, fabric, core_info, &dom->domain_fid, context,
			 uetfi_eq_cb, uetfi_eq_err_cb, &dom->h);
	if (ret) {
		uetfi_core_put();
		goto err;
	}

	dom->fabric = fab;
	dom->core_info = core_info;
	uetfi_dlist_init(&dom->mr_list);
	fab->refs++;
	*domain = &dom->domain_fid;
	return 0;
err:
	free(src);
	fi_freeinfo(core_info);
	free(dom);
	return ret;
}
