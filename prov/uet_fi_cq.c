/*
 * Completion queues and progress.
 *
 * The reference provider gives every endpoint one transmit and one
 * receive completion queue and does all of its work, receive processing
 * included, when it is asked to make progress. Reading a libfabric CQ
 * here makes progress on the endpoints bound to it, collects the
 * completions of their write segments into write completions (see
 * uet_fi_ep.c), and returns those and the receive completions in the
 * CQ's format. Progress is manual: fi_cq_read() drives it, on the
 * initiator to send and retransmit, and on the target to place incoming
 * writes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "uet_fi.h"

#define UETFI_CQ_BATCH 16

static size_t uetfi_cq_entry_size(enum fi_cq_format format)
{
	switch (format) {
	case FI_CQ_FORMAT_UNSPEC:
	case FI_CQ_FORMAT_CONTEXT:
		return sizeof(struct fi_cq_entry);
	case FI_CQ_FORMAT_MSG:
		return sizeof(struct fi_cq_msg_entry);
	case FI_CQ_FORMAT_DATA:
		return sizeof(struct fi_cq_data_entry);
	case FI_CQ_FORMAT_TAGGED:
		return sizeof(struct fi_cq_tagged_entry);
	default:
		return 0;
	}
}

static void uetfi_cq_store(const struct uetfi_cq *cq, void *dst,
			   const struct fi_cq_data_entry *src)
{
	struct fi_cq_msg_entry *msg = dst;
	struct fi_cq_tagged_entry *tagged = dst;

	switch (cq->format) {
	case FI_CQ_FORMAT_CONTEXT:
		((struct fi_cq_entry *) dst)->op_context = src->op_context;
		break;
	case FI_CQ_FORMAT_MSG:
		msg->op_context = src->op_context;
		msg->flags = src->flags;
		msg->len = src->len;
		break;
	case FI_CQ_FORMAT_DATA:
		*(struct fi_cq_data_entry *) dst = *src;
		break;
	case FI_CQ_FORMAT_TAGGED:
		tagged->op_context = src->op_context;
		tagged->flags = src->flags;
		tagged->len = src->len;
		tagged->buf = src->buf;
		tagged->data = src->data;
		tagged->tag = 0;
		break;
	default:
		break;
	}
}

void uetfi_ep_progress(struct uetfi_ep *ep)
{
	int i;

	for (i = 0; i < uetfi_params.progress_burst; i++)
		uet_ep_progress(ep->h);
	uetfi_ep_harvest(ep);
	uetfi_ep_post(ep);
}

/*
 * Read completions from the endpoint's core receive queue into 'out'
 * (count entries in the CQ's format). They are writes with immediate data
 * that landed here. With cq == NULL the endpoint has no receive CQ and
 * they are discarded, so the core can reuse their descriptors.
 */
static ssize_t uetfi_rx_read(struct uetfi_ep *ep, struct uetfi_cq *cq,
			     char *out, size_t count)
{
	struct fi_cq_data_entry ent[UETFI_CQ_BATCH];
	struct fi_cq_err_entry e;
	size_t n = 0, want;
	ssize_t r, i;

	for (;;) {
		want = UETFI_CQ_BATCH;
		if (cq && count - n < want)
			want = count - n;
		if (!want)
			break;
		r = uet_cq_read(ep->core_rx_cq, ent, want);
		if (r == -FI_EAVAIL) {
			if (cq)
				return n ? (ssize_t) n : -FI_EAVAIL;
			memset(&e, 0, sizeof(e));
			if (uet_cq_readerr(ep->core_rx_cq, &e) != 1)
				break;
			UETFI_WARN(FI_LOG_CQ, "receive error without a CQ: "
				   "%s\n", fi_strerror(e.err));
			continue;
		}
		if (r <= 0)
			break;
		for (i = 0; i < r && cq; i++) {
			if ((ent[i].flags & FI_REMOTE_CQ_DATA) &&
			    !(ent[i].flags & (FI_RECV | FI_READ)))
				ent[i].flags |= FI_RMA | FI_REMOTE_WRITE;
			uetfi_cq_store(cq, out + n * cq->entry_size, &ent[i]);
			n++;
		}
		if ((size_t) r < want)
			break;
	}
	return n;
}

void uetfi_ep_drain_rx(struct uetfi_ep *ep)
{
	uetfi_rx_read(ep, NULL, NULL, 0);
}

/* write completions the endpoint has collected */
static size_t uetfi_tx_read(struct uetfi_ep *ep, struct uetfi_cq *cq,
			    char *out, size_t count)
{
	size_t n = 0;

	while (n < count && ep->comp_count) {
		uetfi_cq_store(cq, out + n * cq->entry_size,
			       &ep->comp[ep->comp_head]);
		ep->comp_head = (ep->comp_head + 1) % ep->tx_size;
		ep->comp_count--;
		ep->ops_outstanding--;
		n++;
	}
	return n;
}

static ssize_t uetfi_cq_readfrom_locked(struct uetfi_cq *cq, void *buf,
					size_t count, fi_addr_t *src_addr)
{
	struct uetfi_ep *ep;
	char *out = buf;
	size_t n = 0, i;
	ssize_t r;
	bool err = false;
	int e;

	for (e = 0; e < cq->neps; e++) {
		ep = cq->eps[e];
		if (!ep->enabled)
			continue;
		uetfi_ep_progress(ep);
		if (!ep->rx_cq)
			uetfi_ep_drain_rx(ep);
	}

	for (e = 0; e < cq->neps; e++) {
		ep = cq->eps[e];
		if (!ep->enabled)
			continue;
		if (ep->tx_cq == cq) {
			n += uetfi_tx_read(ep, cq, out + n * cq->entry_size,
					   count - n);
			if (ep->err_count)
				err = true;
		}
		if (ep->rx_cq == cq && n < count) {
			r = uetfi_rx_read(ep, cq, out + n * cq->entry_size,
					  count - n);
			if (r == -FI_EAVAIL)
				err = true;
			else
				n += r;
		}
	}

	if (src_addr)
		for (i = 0; i < n; i++)
			src_addr[i] = FI_ADDR_NOTAVAIL;
	if (n)
		return n;
	return err ? -FI_EAVAIL : -FI_EAGAIN;
}

static ssize_t uetfi_cq_readfrom(struct fid_cq *cq_fid, void *buf,
				 size_t count, fi_addr_t *src_addr)
{
	struct uetfi_cq *cq = container_of(cq_fid, struct uetfi_cq, cq_fid);
	ssize_t ret;

	uetfi_lock(cq->dom);
	ret = uetfi_cq_readfrom_locked(cq, buf, count, src_addr);
	uetfi_unlock(cq->dom);
	return ret;
}

static ssize_t uetfi_cq_read(struct fid_cq *cq_fid, void *buf, size_t count)
{
	return uetfi_cq_readfrom(cq_fid, buf, count, NULL);
}

static void uetfi_err_copy(struct fi_cq_err_entry *dst,
			   const struct fi_cq_err_entry *src, uint32_t api)
{
	dst->op_context = src->op_context;
	dst->flags = src->flags;
	dst->len = src->len;
	dst->buf = src->buf;
	dst->data = src->data;
	dst->tag = src->tag;
	dst->olen = src->olen;
	dst->err = src->err ? src->err : FI_EIO;
	dst->prov_errno = src->prov_errno;
	/* no provider error data */
	if (FI_VERSION_GE(api, FI_VERSION(1, 5))) {
		if (!dst->err_data_size)
			dst->err_data = NULL;
		dst->err_data_size = 0;
	} else {
		dst->err_data = NULL;
	}
	if (FI_VERSION_GE(api, FI_VERSION(1, 20)))
		dst->src_addr = FI_ADDR_NOTAVAIL;
}

static ssize_t uetfi_cq_readerr_locked(struct uetfi_cq *cq,
				       struct fi_cq_err_entry *buf)
{
	uint32_t api = cq->dom->fabric->fabric_fid.api_version;
	struct fi_cq_err_entry e;
	struct uetfi_ep *ep;
	int i;

	for (i = 0; i < cq->neps; i++) {
		ep = cq->eps[i];
		if (!ep->enabled)
			continue;
		if (ep->tx_cq == cq && ep->err_count) {
			uetfi_err_copy(buf, &ep->errs[ep->err_head], api);
			ep->err_head = (ep->err_head + 1) % ep->tx_size;
			ep->err_count--;
			ep->ops_outstanding--;
			return 1;
		}
		if (ep->rx_cq == cq) {
			memset(&e, 0, sizeof(e));
			if (uet_cq_readerr(ep->core_rx_cq, &e) == 1) {
				uetfi_err_copy(buf, &e, api);
				return 1;
			}
		}
	}
	return -FI_EAGAIN;
}

static ssize_t uetfi_cq_readerr(struct fid_cq *cq_fid,
				struct fi_cq_err_entry *buf, uint64_t flags)
{
	struct uetfi_cq *cq = container_of(cq_fid, struct uetfi_cq, cq_fid);
	ssize_t ret;

	(void) flags;
	uetfi_lock(cq->dom);
	ret = uetfi_cq_readerr_locked(cq, buf);
	uetfi_unlock(cq->dom);
	return ret;
}

static const char *uetfi_cq_strerror(struct fid_cq *cq, int prov_errno,
				     const void *err_data, char *buf,
				     size_t len)
{
	const char *s = prov_errno ? fi_strerror(prov_errno) :
				     "UET transport error";

	(void) cq;
	(void) err_data;
	if (buf && len) {
		snprintf(buf, len, "uet: %s", s);
		return buf;
	}
	return s;
}

static int uetfi_cq_close(struct fid *fid)
{
	struct uetfi_cq *cq = container_of(fid, struct uetfi_cq, cq_fid.fid);
	struct uetfi_domain *dom = cq->dom;

	uetfi_lock(dom);
	if (cq->neps) {
		uetfi_unlock(dom);
		return -FI_EBUSY;
	}
	dom->refs--;
	uetfi_unlock(dom);
	free(cq);
	return 0;
}

static struct fi_ops uetfi_cq_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = uetfi_cq_close,
	.bind = UETFI_NOSYS(int (*)(struct fid *, struct fid *, uint64_t)),
	.control = UETFI_NOSYS(int (*)(struct fid *, int, void *)),
	.ops_open = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
					void **, void *)),
	.tostr = UETFI_NOSYS(int (*)(const struct fid *, char *, size_t)),
	.ops_set = UETFI_NOSYS(int (*)(struct fid *, const char *, uint64_t,
				       void *, void *)),
};

static struct fi_ops_cq uetfi_cq_ops = {
	.size = sizeof(struct fi_ops_cq),
	.read = uetfi_cq_read,
	.readfrom = uetfi_cq_readfrom,
	.readerr = uetfi_cq_readerr,
	.sread = UETFI_NOSYS(ssize_t (*)(struct fid_cq *, void *, size_t,
					 const void *, int)),
	.sreadfrom = UETFI_NOSYS(ssize_t (*)(struct fid_cq *, void *, size_t,
					     fi_addr_t *, const void *, int)),
	.signal = UETFI_NOSYS(int (*)(struct fid_cq *)),
	.strerror = uetfi_cq_strerror,
};

int uetfi_cq_open(struct fid_domain *domain, struct fi_cq_attr *attr,
		  struct fid_cq **cq_fid, void *context)
{
	struct uetfi_domain *dom =
		container_of(domain, struct uetfi_domain, domain_fid);
	enum fi_cq_format format = FI_CQ_FORMAT_CONTEXT;
	struct uetfi_cq *cq;

	if (attr) {
		/* no wait objects: fi_cq_sread() is not supported */
		if (attr->wait_obj != FI_WAIT_NONE &&
		    attr->wait_obj != FI_WAIT_UNSPEC)
			return -FI_ENOSYS;
		if (attr->format != FI_CQ_FORMAT_UNSPEC)
			format = attr->format;
	}
	if (!uetfi_cq_entry_size(format))
		return -FI_EINVAL;

	cq = calloc(1, sizeof(*cq));
	if (!cq)
		return -FI_ENOMEM;
	cq->dom = dom;
	cq->format = format;
	cq->entry_size = uetfi_cq_entry_size(format);
	cq->cq_fid.fid.fclass = FI_CLASS_CQ;
	cq->cq_fid.fid.context = context;
	cq->cq_fid.fid.ops = &uetfi_cq_fi_ops;
	cq->cq_fid.ops = &uetfi_cq_ops;
	uetfi_lock(dom);
	dom->refs++;
	uetfi_unlock(dom);
	*cq_fid = &cq->cq_fid;
	return 0;
}

int uetfi_cq_attach_ep(struct uetfi_cq *cq, struct uetfi_ep *ep)
{
	int i;

	for (i = 0; i < cq->neps; i++)
		if (cq->eps[i] == ep)
			return 0;
	if (cq->neps == UETFI_CQ_MAX_EPS)
		return -FI_ENOSPC;
	cq->eps[cq->neps++] = ep;
	return 0;
}

void uetfi_cq_detach_ep(struct uetfi_cq *cq, struct uetfi_ep *ep)
{
	int i;

	for (i = 0; i < cq->neps; i++) {
		if (cq->eps[i] == ep) {
			cq->eps[i] = cq->eps[--cq->neps];
			return;
		}
	}
}
