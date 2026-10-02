/*
 * Endpoints and RMA write.
 *
 * An endpoint is both initiator and target. As initiator it writes with
 * uet_write(); writes without immediate data to an IDEMPOTENT_SAFE region
 * of a peer advertising the HPC profile go over RUDI, which keeps no state
 * at the target. As target it needs nothing but registered regions and
 * progress: the core places incoming writes, from any initiator, whether
 * or not the initiator is in the target's AV.
 *
 * Writes are sent as segments (see struct uetfi_op) with a bounded
 * number in flight, because the core has no flow control for RUDI.
 *
 * Completion semantics: the core reports a segment complete when every
 * packet of it has been answered by the target (a RUDI response or a RUD
 * acknowledgement), and the target answers a packet only after copying
 * its payload into the region. A write completes when all its segments
 * have, so its completion means the data is in target memory, which is
 * FI_DELIVERY_COMPLETE; it is what FI_INJECT_COMPLETE and
 * FI_TRANSMIT_COMPLETE requests get as well. The immediate data of
 * fi_writedata() travels with the last segment, posted only after the
 * others completed, so the target sees it after the whole write landed.
 *
 * fi_close() discards what is outstanding, without completions: queued
 * segments are dropped here and uet_ep_abort() drops those in flight,
 * so nothing of them is sent again once the close has started.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "uet_fi.h"

/* waiting for writes at close, only if the PDS cannot discard them */
#define UETFI_CLOSE_DRAIN_SECS 10

static struct fi_ops_msg uetfi_msg_nosys;
static struct fi_ops_tagged uetfi_tagged_nosys;
static struct fi_ops_atomic uetfi_atomic_nosys;
static struct fi_ops_collective uetfi_collective_nosys;

/*
 * Fill an ops table whose members after 'size' are all function
 * pointers, as the libfabric ops tables are, with uetfi_nosys.
 */
static void uetfi_fill_nosys(void *ops, size_t size)
{
	void (**fp)(void) = (void (**)(void)) ((char *) ops + sizeof(size_t));
	size_t i, n = (size - sizeof(size_t)) / sizeof(*fp);

	*(size_t *) ops = size;
	for (i = 0; i < n; i++)
		fp[i] = (void (*)(void)) uetfi_nosys;
}

static void uetfi_init_nosys_ops(void)
{
	static bool done;

	if (done)
		return;
	uetfi_fill_nosys(&uetfi_msg_nosys, sizeof(uetfi_msg_nosys));
	uetfi_fill_nosys(&uetfi_tagged_nosys, sizeof(uetfi_tagged_nosys));
	uetfi_fill_nosys(&uetfi_atomic_nosys, sizeof(uetfi_atomic_nosys));
	uetfi_fill_nosys(&uetfi_collective_nosys,
			 sizeof(uetfi_collective_nosys));
	done = true;
}

/*******************************************************************
 * RMA
 *******************************************************************/

static void uetfi_op_finish(struct uetfi_ep *ep, struct uetfi_op *op)
{
	struct fi_cq_data_entry *c;
	struct fi_cq_err_entry *e;
	size_t slot;

	if (op->err) {
		slot = (ep->err_head + ep->err_count++) % ep->tx_size;
		e = &ep->errs[slot];
		memset(e, 0, sizeof(*e));
		e->op_context = op->context;
		e->flags = FI_RMA | FI_WRITE;
		e->err = op->err;
		e->prov_errno = op->err;
	} else if (!op->silent) {
		slot = (ep->comp_head + ep->comp_count++) % ep->tx_size;
		c = &ep->comp[slot];
		memset(c, 0, sizeof(*c));
		c->op_context = op->context;
		c->flags = FI_RMA | FI_WRITE;
	} else {
		ep->ops_outstanding--;
	}
	op->next = ep->op_free;
	ep->op_free = op;
}

static void uetfi_pend_pop(struct uetfi_ep *ep)
{
	ep->pend_head = ep->pend_head->next;
	if (!ep->pend_head)
		ep->pend_tail = NULL;
}

/* a segment of 'op' completed, with error 'err' if nonzero */
static void uetfi_seg_done(struct uetfi_ep *ep, struct uetfi_op *op, int err)
{
	op->segs_out--;
	ep->segs_inflight--;
	if (err && !op->err) {
		op->err = err;
		/* post nothing more of it; only the head can have more */
		if (op->posted < op->len && ep->pend_head == op) {
			op->posted = op->len;
			uetfi_pend_pop(ep);
		}
	}
	if (!op->segs_out && op->posted == op->len && ep->pend_head != op)
		uetfi_op_finish(ep, op);
}

/* collect the completions of the segments the core has finished */
void uetfi_ep_harvest(struct uetfi_ep *ep)
{
	/* entries come in the format the core queue was bound with */
	struct fi_cq_data_entry ent[16];
	struct fi_cq_err_entry e;
	ssize_t r, i;

	while (ep->segs_inflight) {
		r = uet_cq_read(ep->core_tx_cq, ent, 16);
		if (r == -FI_EAVAIL) {
			memset(&e, 0, sizeof(e));
			if (uet_cq_readerr(ep->core_tx_cq, &e) != 1)
				break;
			uetfi_seg_done(ep, e.op_context, e.err ? e.err : FI_EIO);
			continue;
		}
		if (r <= 0)
			break;
		for (i = 0; i < r; i++)
			uetfi_seg_done(ep, ent[i].op_context, 0);
		if (r < 16)
			break;
	}
}

/* hand segments of pending writes to the core, up to max_segs in flight */
void uetfi_ep_post(struct uetfi_ep *ep)
{
	struct uetfi_op *op;
	uint64_t *data;
	size_t left, seg;
	ssize_t ret;

	while ((op = ep->pend_head) && ep->segs_inflight < ep->max_segs) {
		left = op->len - op->posted;
		seg = left < ep->seg_size ? left : ep->seg_size;
		data = NULL;
		if (seg == left && op->has_data) {
			/* immediate data only after the rest has landed */
			if (op->segs_out)
				break;
			data = &op->data;
		}
		ret = uet_write(ep->h, UET_DEF_JOB_ID,
				(void *) (op->buf + op->posted), seg, data,
				NULL, op->peer->h, op->addr + op->posted,
				op->key, op);
		if (ret == -FI_EAGAIN)
			break;
		if (ret) {
			UETFI_WARN(FI_LOG_EP_DATA, "write failed: %s\n",
				   fi_strerror((int) -ret));
			op->err = (int) -ret;
			op->posted = op->len;
			uetfi_pend_pop(ep);
			if (!op->segs_out)
				uetfi_op_finish(ep, op);
			continue;
		}
		op->posted += seg;
		op->segs_out++;
		ep->segs_inflight++;
		if (op->posted == op->len)
			uetfi_pend_pop(ep);
	}
}

static ssize_t uetfi_write_common(struct uetfi_ep *ep, const void *buf,
				  size_t len, fi_addr_t dest, uint64_t addr,
				  uint64_t key, void *context, uint64_t flags,
				  const uint64_t *data)
{
	struct uetfi_peer *peer;
	struct uetfi_op *op;

	if (!ep->enabled)
		return -FI_EOPBADSTATE;
	if (!ep->tx_cq)
		return -FI_ENOCQ;
	if (len > UETFI_MAX_MSG_SIZE)
		return -FI_EMSGSIZE;
	if (data && !uetfi_core_desc.cq_data_size)
		return -FI_ENOSYS;	/* cq_data_size is 0 */
	peer = uetfi_av_peer(ep->av, dest);
	if (!peer)
		return -FI_EINVAL;
	if (ep->ops_outstanding >= ep->tx_size || !ep->op_free)
		return -FI_EAGAIN;

	op = ep->op_free;
	ep->op_free = op->next;
	memset(op, 0, sizeof(*op));
	op->context = context;
	op->buf = buf;
	op->len = len;
	op->addr = addr;
	op->key = key;
	op->peer = peer;
	op->silent = ep->tx_selective && !(flags & FI_COMPLETION);
	if (data) {
		op->has_data = true;
		op->data = *data;
	}
	if (ep->pend_tail)
		ep->pend_tail->next = op;
	else
		ep->pend_head = op;
	ep->pend_tail = op;
	ep->ops_outstanding++;

	uetfi_ep_post(ep);
	return 0;
}

static ssize_t uetfi_write(struct fid_ep *ep_fid, const void *buf, size_t len,
			   void *desc, fi_addr_t dest, uint64_t addr,
			   uint64_t key, void *context)
{
	struct uetfi_ep *ep = container_of(ep_fid, struct uetfi_ep, ep_fid);

	(void) desc;	/* local buffers need no registration */
	return uetfi_write_common(ep, buf, len, dest, addr, key, context,
				  ep->tx_op_flags, NULL);
}

static ssize_t uetfi_writev(struct fid_ep *ep_fid, const struct iovec *iov,
			    void **desc, size_t count, fi_addr_t dest,
			    uint64_t addr, uint64_t key, void *context)
{
	struct uetfi_ep *ep = container_of(ep_fid, struct uetfi_ep, ep_fid);

	(void) desc;
	if (count > 1)
		return -FI_EINVAL;
	return uetfi_write_common(ep, count ? iov[0].iov_base : NULL,
				  count ? iov[0].iov_len : 0, dest, addr, key,
				  context, ep->tx_op_flags, NULL);
}

static ssize_t uetfi_writemsg(struct fid_ep *ep_fid,
			      const struct fi_msg_rma *msg, uint64_t flags)
{
	struct uetfi_ep *ep = container_of(ep_fid, struct uetfi_ep, ep_fid);
	const void *buf = NULL;
	size_t len = 0;

	if (flags & ~(UETFI_TX_OP_FLAGS | FI_REMOTE_CQ_DATA | FI_MORE))
		return -FI_EBADFLAGS;
	if (msg->iov_count > 1 || msg->rma_iov_count != 1)
		return -FI_EINVAL;
	if (msg->iov_count) {
		buf = msg->msg_iov[0].iov_base;
		len = msg->msg_iov[0].iov_len;
	}
	if (len > msg->rma_iov[0].len)
		return -FI_EINVAL;
	return uetfi_write_common(ep, buf, len, msg->addr, msg->rma_iov[0].addr,
				  msg->rma_iov[0].key, msg->context, flags,
				  (flags & FI_REMOTE_CQ_DATA) ? &msg->data :
								NULL);
}

static ssize_t uetfi_writedata(struct fid_ep *ep_fid, const void *buf,
			       size_t len, void *desc, uint64_t data,
			       fi_addr_t dest, uint64_t addr, uint64_t key,
			       void *context)
{
	struct uetfi_ep *ep = container_of(ep_fid, struct uetfi_ep, ep_fid);

	(void) desc;
	/* immediate data needs target state, so this goes over RUD */
	return uetfi_write_common(ep, buf, len, dest, addr, key, context,
				  ep->tx_op_flags, &data);
}

static struct fi_ops_rma uetfi_rma_ops = {
	.size = sizeof(struct fi_ops_rma),
	.read = UETFI_NOSYS(ssize_t (*)(struct fid_ep *, void *, size_t, void *,
					fi_addr_t, uint64_t, uint64_t, void *)),
	.readv = UETFI_NOSYS(ssize_t (*)(struct fid_ep *, const struct iovec *,
					 void **, size_t, fi_addr_t, uint64_t,
					 uint64_t, void *)),
	.readmsg = UETFI_NOSYS(ssize_t (*)(struct fid_ep *,
					   const struct fi_msg_rma *,
					   uint64_t)),
	.write = uetfi_write,
	.writev = uetfi_writev,
	.writemsg = uetfi_writemsg,
	.inject = UETFI_NOSYS(ssize_t (*)(struct fid_ep *, const void *, size_t,
					  fi_addr_t, uint64_t, uint64_t)),
	.writedata = uetfi_writedata,
	.injectdata = UETFI_NOSYS(ssize_t (*)(struct fid_ep *, const void *,
					      size_t, uint64_t, fi_addr_t,
					      uint64_t, uint64_t)),
};

/*******************************************************************
 * connection management: only fi_getname
 *******************************************************************/

static int uetfi_getname(fid_t fid, void *addr, size_t *addrlen)
{
	struct uetfi_ep *ep = container_of(fid, struct uetfi_ep, ep_fid.fid);
	struct uet_addr ua;
	int ret;

	if (*addrlen < UETFI_ADDR_LEN) {
		*addrlen = UETFI_ADDR_LEN;
		return -FI_ETOOSMALL;
	}
	ret = uet_getname(ep->h, &ua);
	if (ret)
		return ret;
	uetfi_addr_encode(&ua, addr);
	*addrlen = UETFI_ADDR_LEN;
	return 0;
}

static struct fi_ops_cm uetfi_cm_ops = {
	.size = sizeof(struct fi_ops_cm),
	.setname = UETFI_NOSYS(int (*)(fid_t, void *, size_t)),
	.getname = uetfi_getname,
	.getpeer = UETFI_NOSYS(int (*)(struct fid_ep *, void *, size_t *)),
	.connect = UETFI_NOSYS(int (*)(struct fid_ep *, const void *,
				       const void *, size_t)),
	.listen = UETFI_NOSYS(int (*)(struct fid_pep *)),
	.accept = UETFI_NOSYS(int (*)(struct fid_ep *, const void *, size_t)),
	.reject = UETFI_NOSYS(int (*)(struct fid_pep *, fid_t, const void *,
				      size_t)),
	.shutdown = UETFI_NOSYS(int (*)(struct fid_ep *, uint64_t)),
	.join = UETFI_NOSYS(int (*)(struct fid_ep *, const void *, uint64_t,
				    struct fid_mc **, void *)),
};

/*******************************************************************
 * endpoint
 *******************************************************************/

static int uetfi_ep_enable(struct uetfi_ep *ep)
{
	int ret;

	if (ep->enabled)
		return 0;
	if (!ep->av)
		return -FI_ENOAV;
	if (!ep->tx_cq && !ep->rx_cq)
		return -FI_ENOCQ;

	/*
	 * The core queues always exist, in DATA format, whatever the
	 * application bound: the receive queue carries completions of
	 * writes with immediate data, and both must be read for the core
	 * to recycle descriptors.
	 */
	if (!ep->core_tx_cq) {
		memset(&ep->core_cq_attr, 0, sizeof(ep->core_cq_attr));
		ep->core_cq_attr.format = FI_CQ_FORMAT_DATA;
		ep->core_cq_attr.wait_obj = FI_WAIT_NONE;
		ep->core_cq_attr.size = ep->tx_size > ep->rx_size ?
					ep->tx_size : ep->rx_size;
		ret = uet_ep_bind_cq(ep->h, &ep->core_cq_attr,
				     &ep->core_cq_fid, FI_SEND, NULL,
				     &ep->core_tx_cq);
		if (ret)
			return ret;
	}
	if (!ep->core_rx_cq) {
		ret = uet_ep_bind_cq(ep->h, &ep->core_cq_attr,
				     &ep->core_cq_fid, FI_RECV, NULL,
				     &ep->core_rx_cq);
		if (ret)
			return ret;
	}

	ret = uetfi_mr_enable_bound(ep);
	if (ret)
		return ret;
	ret = uet_ep_enable(ep->h);
	if (ret)
		return ret;
	ep->enabled = true;
	return 0;
}

static int uetfi_ep_close(struct fid *fid)
{
	struct uetfi_ep *ep = container_of(fid, struct uetfi_ep, ep_fid.fid);
	struct uetfi_domain *dom = ep->dom;
	struct timespec t0, t1;
	size_t segs = ep->segs_inflight, ops = ep->ops_outstanding;
	time_t deadline;
	int ret;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	/*
	 * fi_endpoint(3): operations still outstanding when the endpoint is
	 * closed are discarded, without completions. Writes not yet handed
	 * to the core go with the endpoint's memory, and the core drops the
	 * segments in flight: from here on none of them is sent or
	 * retransmitted. The core endpoint then closes at once.
	 */
	ep->pend_head = ep->pend_tail = NULL;
	ret = uet_ep_abort(ep->h);
	if (ret == -FI_ENOSYS && ep->enabled) {
		/* a PDS that cannot abort (UET_PDS=sng): let them finish */
		UETFI_WARN(FI_LOG_EP_CTRL, "the PDS cannot discard writes; "
			   "waiting for those in flight\n");
		deadline = time(NULL) + UETFI_CLOSE_DRAIN_SECS;
		while (ep->segs_inflight && time(NULL) < deadline) {
			uetfi_ep_progress(ep);
			uetfi_ep_drain_rx(ep);
		}
	} else if (ret && ret != -FI_ENOSYS) {
		return ret;
	}

	/*
	 * Closing the core endpoint returns its enabled regions to the
	 * registered state, so they can be bound to a later endpoint;
	 * enable any that were bound but never enabled so they do too.
	 */
	if (!ep->enabled)
		uetfi_mr_enable_bound(ep);

	ret = uet_ep_close(ep->h);
	if (ret)
		return ret;

	clock_gettime(CLOCK_MONOTONIC, &t1);
	UETFI_INFO(FI_LOG_EP_CTRL, "endpoint closed in %ld us, discarding "
		   "%zu writes (%zu segments in flight)\n",
		   (long) ((t1.tv_sec - t0.tv_sec) * 1000000 +
			   (t1.tv_nsec - t0.tv_nsec) / 1000), ops, segs);

	uetfi_mr_detach_ep(ep);
	if (ep->tx_cq)
		uetfi_cq_detach_ep(ep->tx_cq, ep);
	if (ep->rx_cq && ep->rx_cq != ep->tx_cq)
		uetfi_cq_detach_ep(ep->rx_cq, ep);
	if (ep->av)
		ep->av->refs--;
	dom->ep = NULL;
	dom->refs--;
	uetfi_core_release_ep();
	fi_freeinfo(ep->core_info);
	free(ep->ops);
	free(ep->comp);
	free(ep->errs);
	free(ep);
	return 0;
}

static int uetfi_ep_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct uetfi_ep *ep = container_of(fid, struct uetfi_ep, ep_fid.fid);
	struct uetfi_cq *cq;
	struct uetfi_av *av;
	int ret;

	if (ep->enabled)
		return -FI_EOPBADSTATE;

	switch (bfid->fclass) {
	case FI_CLASS_CQ:
		cq = container_of(bfid, struct uetfi_cq, cq_fid.fid);
		if (cq->dom != ep->dom)
			return -FI_EINVAL;
		if (flags & ~(FI_TRANSMIT | FI_RECV | FI_SELECTIVE_COMPLETION))
			return -FI_EBADFLAGS;
		if (!(flags & (FI_TRANSMIT | FI_RECV)))
			return -FI_EINVAL;
		if (((flags & FI_TRANSMIT) && ep->tx_cq) ||
		    ((flags & FI_RECV) && ep->rx_cq))
			return -FI_EINVAL;
		ret = uetfi_cq_attach_ep(cq, ep);
		if (ret)
			return ret;
		if (flags & FI_TRANSMIT) {
			ep->tx_cq = cq;
			ep->tx_selective = !!(flags & FI_SELECTIVE_COMPLETION);
		}
		if (flags & FI_RECV)
			ep->rx_cq = cq;
		return 0;
	case FI_CLASS_AV:
		av = container_of(bfid, struct uetfi_av, av_fid.fid);
		if (av->dom != ep->dom)
			return -FI_EINVAL;
		if (ep->av)
			return -FI_EINVAL;
		ep->av = av;
		av->refs++;
		return 0;
	case FI_CLASS_MR:
		return uetfi_mr_attach(container_of(bfid, struct uetfi_mr,
						    mr_fid.fid), ep);
	default:
		return -FI_ENOSYS;
	}
}

static int uetfi_ep_control(struct fid *fid, int command, void *arg)
{
	struct uetfi_ep *ep = container_of(fid, struct uetfi_ep, ep_fid.fid);
	uint64_t *flags = arg;

	switch (command) {
	case FI_ENABLE:
		return uetfi_ep_enable(ep);
	case FI_GETOPSFLAG:
		if (!flags)
			return -FI_EINVAL;
		if (*flags & FI_TRANSMIT)
			*flags = ep->tx_op_flags;
		else if (*flags & FI_RECV)
			*flags = 0;
		else
			return -FI_EINVAL;
		return 0;
	case FI_SETOPSFLAG:
		if (!flags)
			return -FI_EINVAL;
		if (*flags & FI_TRANSMIT) {
			if (*flags & ~(FI_TRANSMIT | UETFI_TX_OP_FLAGS))
				return -FI_EBADFLAGS;
			ep->tx_op_flags = *flags & ~FI_TRANSMIT;
			return 0;
		}
		if (*flags & FI_RECV)
			return (*flags & ~FI_RECV) ? -FI_EBADFLAGS : 0;
		return -FI_EINVAL;
	default:
		return -FI_ENOSYS;
	}
}

static int uetfi_ep_getopt(fid_t fid, int level, int optname, void *optval,
			   size_t *optlen)
{
	(void) fid;
	(void) level;
	(void) optname;
	(void) optval;
	(void) optlen;
	return -FI_ENOPROTOOPT;
}

static int uetfi_ep_setopt(fid_t fid, int level, int optname,
			   const void *optval, size_t optlen)
{
	(void) fid;
	(void) level;
	(void) optname;
	(void) optval;
	(void) optlen;
	return -FI_ENOPROTOOPT;
}

static ssize_t uetfi_tx_size_left(struct fid_ep *ep_fid)
{
	struct uetfi_ep *ep = container_of(ep_fid, struct uetfi_ep, ep_fid);

	return ep->tx_size - ep->ops_outstanding;
}

static struct fi_ops uetfi_ep_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = uetfi_ep_close,
	.bind = uetfi_ep_bind,
	.control = uetfi_ep_control,
	.ops_open = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
					void **, void *)),
	.tostr = UETFI_NOSYS(int (*)(const struct fid *, char *, size_t)),
	.ops_set = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
				       void *, void *)),
};

static struct fi_ops_ep uetfi_ep_ops = {
	.size = sizeof(struct fi_ops_ep),
	.cancel = UETFI_NOSYS(ssize_t (*)(fid_t, void *)),
	.getopt = uetfi_ep_getopt,
	.setopt = uetfi_ep_setopt,
	.tx_ctx = UETFI_NOSYS(int (*)(struct fid_ep *, int, struct fi_tx_attr *,
				      struct fid_ep **, void *)),
	.rx_ctx = UETFI_NOSYS(int (*)(struct fid_ep *, int, struct fi_rx_attr *,
				      struct fid_ep **, void *)),
	.rx_size_left = UETFI_NOSYS(ssize_t (*)(struct fid_ep *)),
	.tx_size_left = uetfi_tx_size_left,
};

static size_t uetfi_queue_size(size_t requested, size_t def)
{
	if (!requested)
		return def;
	return requested > UETFI_MAX_QUEUE_SIZE ? UETFI_MAX_QUEUE_SIZE :
						  requested;
}

int uetfi_endpoint(struct fid_domain *domain, struct fi_info *info,
		   struct fid_ep **ep_fid, void *context)
{
	struct uetfi_domain *dom =
		container_of(domain, struct uetfi_domain, domain_fid);
	struct uetfi_dlist *it;
	struct uetfi_ep *ep;
	struct uet_addr ua;
	size_t i;
	int ret;

	if (!info || !info->ep_attr)
		return -FI_EINVAL;
	if (info->ep_attr->type != FI_EP_RDM &&
	    info->ep_attr->type != FI_EP_UNSPEC)
		return -FI_EINVAL;
	if (dom->ep)
		return -FI_EBUSY;
	ret = uetfi_core_claim_ep();
	if (ret) {
		UETFI_WARN(FI_LOG_EP_CTRL, "one endpoint per process\n");
		return ret;
	}

	uetfi_init_nosys_ops();
	ep = calloc(1, sizeof(*ep));
	if (!ep) {
		ret = -FI_ENOMEM;
		goto err;
	}
	ep->dom = dom;
	ep->tx_size = uetfi_queue_size(info->tx_attr ? info->tx_attr->size : 0,
				       UETFI_DEF_TX_SIZE);
	ep->rx_size = uetfi_queue_size(info->rx_attr ? info->rx_attr->size : 0,
				       UETFI_DEF_RX_SIZE);
	ep->tx_op_flags = info->tx_attr ?
			  (info->tx_attr->op_flags & UETFI_TX_OP_FLAGS) : 0;

	ep->seg_size = uetfi_segment_size(&dom->ifinfo);
	ep->max_segs = uetfi_params.max_segments;

	ep->core_info = fi_dupinfo(dom->core_info);
	ep->ops = calloc(ep->tx_size, sizeof(*ep->ops));
	ep->comp = calloc(ep->tx_size, sizeof(*ep->comp));
	ep->errs = calloc(ep->tx_size, sizeof(*ep->errs));
	if (!ep->core_info || !ep->ops || !ep->comp || !ep->errs) {
		ret = -FI_ENOMEM;
		goto err;
	}
	for (i = 0; i < ep->tx_size; i++) {
		ep->ops[i].next = ep->op_free;
		ep->op_free = &ep->ops[i];
	}
	/* a core transmit descriptor per segment in flight */
	ep->core_info->tx_attr->size = ep->max_segs;
	ep->core_info->rx_attr->size = ep->rx_size;
	ep->core_info->tx_attr->msg_order = 0;	/* RUD/RUDI, not ROD */
	ep->core_info->tx_attr->tclass = info->tx_attr ?
					 info->tx_attr->tclass : FI_TC_UNSPEC;

	/* an address the application chose must be on this netdev */
	if (info->src_addr) {
		ret = uetfi_addr_decode(info->src_addr, info->src_addrlen, &ua);
		if (ret || uet_addr_is_ipv6(&ua) ||
		    ua.fa.v4 != dom->ifinfo.ipv4) {
			ret = -FI_EINVAL;
			goto err;
		}
		memcpy(ep->core_info->src_addr, &ua, sizeof(ua));
	}

	ep->ep_fid.fid.fclass = FI_CLASS_EP;
	ep->ep_fid.fid.context = context;
	ep->ep_fid.fid.ops = &uetfi_ep_fi_ops;
	ep->ep_fid.ops = &uetfi_ep_ops;
	ep->ep_fid.cm = &uetfi_cm_ops;
	ep->ep_fid.msg = &uetfi_msg_nosys;
	ep->ep_fid.rma = &uetfi_rma_ops;
	ep->ep_fid.tagged = &uetfi_tagged_nosys;
	ep->ep_fid.atomic = &uetfi_atomic_nosys;
	ep->ep_fid.collective = &uetfi_collective_nosys;

	ret = uet_endpoint(dom->h, ep->core_info, &ep->ep_fid, context,
			   &ep->h);
	if (ret)
		goto err;

	dom->ep = ep;
	dom->refs++;

	/* regions registered so far serve this endpoint */
	uetfi_dlist_foreach(&dom->mr_list, it) {
		struct uetfi_mr *mr = container_of(it, struct uetfi_mr, entry);

		if (!mr->ep) {
			ret = uetfi_mr_attach(mr, ep);
			if (ret)
				UETFI_WARN(FI_LOG_EP_CTRL, "binding a region "
					   "failed: %s\n", fi_strerror(-ret));
		}
	}

	*ep_fid = &ep->ep_fid;
	return 0;
err:
	if (ep) {
		fi_freeinfo(ep->core_info);
		free(ep->ops);
		free(ep->comp);
		free(ep->errs);
		free(ep);
	}
	uetfi_core_release_ep();
	return ret;
}

int uetfi_endpoint2(struct fid_domain *domain, struct fi_info *info,
		    struct fid_ep **ep, uint64_t flags, void *context)
{
	if (flags)
		return -FI_EBADFLAGS;
	return uetfi_endpoint(domain, info, ep, context);
}
