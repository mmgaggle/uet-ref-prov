/*
 * RMA write test for the libfabric "uet" provider. Uses only the public
 * libfabric API (link with -lfabric).
 *
 *   test_rma [opts] info
 *	open everything, print the endpoint address and fi_info, close
 *
 *   test_rma [opts] target <size> [writers]
 *	register a window of <size> bytes, print its address and key, then
 *	poll the CQ (manual progress lets incoming writes land) until the
 *	whole window holds the expected pattern and [writers] signals
 *	(zero-length writes with immediate data) have arrived
 *
 *   test_rma [opts] write <addr-hex> <key-hex> <base> <offset> <len>
 *	write [offset, offset+len) of the pattern to the target window at
 *	remote address base+offset, in chunks, with FI_DELIVERY_COMPLETE,
 *	then signal the target with a zero-length fi_writedata
 *
 *   test_rma [opts] cutoff <addr-hex> <key-hex> <base> <len> <budget-ms> <dir>
 *	cut off a write and carry on with a new endpoint, as a consumer
 *	does when a write misses its deadline: write 4 KiB of zeros (so the
 *	peer is resolved), create <dir>/ready and wait for <dir>/go (the
 *	link is taken down in between), write <len> bytes of the pattern to
 *	the target window, and when <budget-ms> passes close the endpoint
 *	and CQ. Then open a new CQ and endpoint on the same domain and AV,
 *	register a new window of <len> bytes (token in <dir>/win), wait for
 *	<dir>/rewrite, write the pattern to the target again and signal it,
 *	and wait for a peer to fill and signal the new window
 *
 *   test_rma [opts] -o myfile pair <size> <peerfile>
 *	one endpoint as initiator and target at once: lend a window of
 *	<size> bytes (address in myfile), wait for the peer's file, fill
 *	the peer's window while the peer fills ours, then verify ours
 *
 *   test_rma [opts] mt <addr-hex> <key-hex> <base> <len> <dead-addr-hex>
 *	FI_THREAD_SAFE: a thread writes the pattern over [0, len) of the
 *	target window again and again, one chunk at a time, while the main
 *	thread inserts a peer that never answers ARP (dead-addr-hex). The
 *	insert waits for ARP in the provider; the writes must go on
 *	meanwhile, none slower than 200 ms. Then a write to the dead peer
 *	must fail at once, and the target is signalled
 *
 * options:
 *   -i ifname   netdev (hints->domain_attr->name; default: $UET_IFNAME)
 *   -o file     target: also write "addr key base size" to file
 *   -c bytes    write: chunk size (default 65536)
 *   -q depth    write: writes in flight (default 16)
 *   -a api      write: writemsg (default) or write
 *   -r          write: register the source buffer and pass its desc
 *   -n          write: do not signal the target
 *   -m          use FI_AV_MAP (default FI_AV_TABLE)
 *   -z file     target: when file appears, report how many bytes of the
 *               window are nonzero
 *   -t secs     timeout (default 120)
 *
 * The pattern byte at window offset i is a function of i alone, so any
 * number of writers can fill disjoint ranges of one window.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_rma.h>

#define MAX_ADDR 64

struct opts {
	const char *ifname;
	const char *outfile;
	size_t chunk;
	size_t depth;
	bool use_write;
	bool reg_src;
	bool signal;
	bool av_map;
	const char *snapfile;
	int timeout;
	bool thread_safe;	/* ask for FI_THREAD_SAFE */
};

struct res {
	struct fi_info *info;
	struct fid_fabric *fabric;
	struct fid_domain *domain;
	struct fid_av *av;
	struct fid_cq *cq;
	struct fid_ep *ep;
	uint8_t addr[MAX_ADDR];
	size_t addrlen;
};

static uint8_t pattern(uint64_t i)
{
	return (uint8_t) ((i * 131u + (i >> 12) * 7u + 13u) & 0xff);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define CHECK(call) do {						\
		int _r = (int) (call);					\
		if (_r) {						\
			fprintf(stderr, "%s: %d (%s)\n", #call, _r,	\
				fi_strerror(-_r));			\
			return _r;					\
		}							\
	} while (0)

static void hex(const uint8_t *b, size_t n, char *out)
{
	size_t i;

	for (i = 0; i < n; i++)
		sprintf(out + 2 * i, "%02x", b[i]);
	out[2 * n] = '\0';
}

static int unhex(const char *s, uint8_t *b, size_t max, size_t *n)
{
	size_t len = strlen(s), i;
	unsigned int v;

	if (len % 2 || len / 2 > max)
		return -1;
	for (i = 0; i < len / 2; i++) {
		if (sscanf(s + 2 * i, "%2x", &v) != 1)
			return -1;
		b[i] = v;
	}
	*n = len / 2;
	return 0;
}

static int open_ep(struct res *r, enum fi_cq_format format);

static int setup(struct res *r, const struct opts *o,
		 enum fi_cq_format format)
{
	struct fi_info *hints;
	struct fi_av_attr av_attr = {
		.type = o->av_map ? FI_AV_MAP : FI_AV_TABLE,
	};
	int ret;

	hints = fi_allocinfo();
	if (!hints)
		return -FI_ENOMEM;
	hints->fabric_attr->prov_name = strdup("uet");
	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_RMA | FI_WRITE | FI_REMOTE_WRITE;
	hints->mode = FI_CONTEXT | FI_CONTEXT2;
	hints->domain_attr->mr_mode = FI_MR_PROV_KEY | FI_MR_VIRT_ADDR |
				      FI_MR_ALLOCATED | FI_MR_LOCAL |
				      FI_MR_ENDPOINT;
	hints->domain_attr->threading = o->thread_safe ? FI_THREAD_SAFE :
							 FI_THREAD_DOMAIN;
	if (o->ifname)
		hints->domain_attr->name = strdup(o->ifname);

	ret = fi_getinfo(FI_VERSION(1, 18), NULL, NULL, 0, hints, &r->info);
	fi_freeinfo(hints);
	CHECK(ret);
	CHECK(fi_fabric(r->info->fabric_attr, &r->fabric, NULL));
	CHECK(fi_domain(r->fabric, r->info, &r->domain, NULL));
	CHECK(fi_av_open(r->domain, &av_attr, &r->av, NULL));
	return open_ep(r, format);
}

/* a CQ and an enabled endpoint on the domain, bound to its AV */
static int open_ep(struct res *r, enum fi_cq_format format)
{
	struct fi_cq_attr cq_attr = { .format = format, .size = 256,
				      .wait_obj = FI_WAIT_NONE };

	CHECK(fi_cq_open(r->domain, &cq_attr, &r->cq, NULL));
	CHECK(fi_endpoint(r->domain, r->info, &r->ep, NULL));
	CHECK(fi_ep_bind(r->ep, &r->cq->fid, FI_TRANSMIT | FI_RECV));
	CHECK(fi_ep_bind(r->ep, &r->av->fid, 0));
	CHECK(fi_enable(r->ep));
	r->addrlen = sizeof(r->addr);
	CHECK(fi_getname(&r->ep->fid, r->addr, &r->addrlen));
	return 0;
}

static int teardown(struct res *r)
{
	if (r->ep)
		CHECK(fi_close(&r->ep->fid));
	if (r->av)
		CHECK(fi_close(&r->av->fid));
	if (r->cq)
		CHECK(fi_close(&r->cq->fid));
	if (r->domain)
		CHECK(fi_close(&r->domain->fid));
	if (r->fabric)
		CHECK(fi_close(&r->fabric->fid));
	fi_freeinfo(r->info);
	return 0;
}

static int reg(struct res *r, void *buf, size_t len, uint64_t access,
	       struct fid_mr **mr)
{
	CHECK(fi_mr_reg(r->domain, buf, len, access, 0, 0, 0, mr, NULL));
	if (r->info->domain_attr->mr_mode & FI_MR_ENDPOINT) {
		CHECK(fi_mr_bind(*mr, &r->ep->fid, 0));
		CHECK(fi_mr_enable(*mr));
	}
	return 0;
}

static void report_err(struct fid_cq *cq)
{
	struct fi_cq_err_entry err;
	char buf[128];

	memset(&err, 0, sizeof(err));
	if (fi_cq_readerr(cq, &err, 0) == 1)
		fprintf(stderr, "completion error: err %d (%s), prov_errno "
			"%d (%s), context %p\n", err.err, fi_strerror(err.err),
			err.prov_errno,
			fi_cq_strerror(cq, err.prov_errno, err.err_data, buf,
				       sizeof(buf)), err.op_context);
	else
		fprintf(stderr, "fi_cq_readerr returned nothing\n");
}

static int run_info(const struct opts *o)
{
	struct res r;
	char h[2 * MAX_ADDR + 1];

	memset(&r, 0, sizeof(r));
	CHECK(setup(&r, o, FI_CQ_FORMAT_CONTEXT));
	hex(r.addr, r.addrlen, h);
	printf("ADDR %s\nADDRLEN %zu\n", h, r.addrlen);
	printf("%s", fi_tostr(r.info, FI_TYPE_INFO));
	return teardown(&r);
}

static size_t count_bad(const uint8_t *w, const uint8_t *expect, size_t size,
			size_t *first)
{
	size_t i, bad = 0;

	*first = size;
	if (!memcmp(w, expect, size))
		return 0;
	for (i = 0; i < size; i++) {
		if (w[i] != expect[i]) {
			if (!bad)
				*first = i;
			bad++;
		}
	}
	return bad;
}

static int run_target(const struct opts *o, size_t size, int writers)
{
	struct res r;
	struct fid_mr *mr;
	struct fi_cq_data_entry ce[16];
	char h[2 * MAX_ADDR + 1];
	uint8_t *win, *expect;
	uint64_t base;
	double start, last_check = 0, t_done;
	size_t bad = size, first = 0;
	int signals = 0, i;
	bool snapped = false;
	ssize_t n;
	FILE *f;

	memset(&r, 0, sizeof(r));
	CHECK(setup(&r, o, FI_CQ_FORMAT_DATA));
	win = aligned_alloc(4096, (size + 4095) & ~(size_t) 4095);
	expect = malloc(size);
	if (!win || !expect)
		return -FI_ENOMEM;
	memset(win, 0, size);
	for (size_t j = 0; j < size; j++)
		expect[j] = pattern(j);
	CHECK(reg(&r, win, size, FI_WRITE | FI_REMOTE_WRITE, &mr));
	base = (r.info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) ?
	       (uintptr_t) win : 0;

	hex(r.addr, r.addrlen, h);
	printf("ADDR %s\nKEY %016" PRIx64 "\nBASE %" PRIu64 "\nSIZE %zu\n",
	       h, fi_mr_key(mr), base, size);
	fflush(stdout);
	if (o->outfile) {
		f = fopen(o->outfile, "w");
		if (!f)
			return -errno;
		fprintf(f, "%s %016" PRIx64 " %" PRIu64 " %zu\n", h,
			fi_mr_key(mr), base, size);
		fclose(f);
	}

	/* the target never inserts its writers into its AV */
	start = now();
	for (;;) {
		n = fi_cq_read(r.cq, ce, 16);
		if (n == -FI_EAVAIL) {
			report_err(r.cq);
		} else if (n < 0 && n != -FI_EAGAIN) {
			fprintf(stderr, "fi_cq_read: %s\n", fi_strerror(-n));
			return (int) n;
		}
		for (i = 0; i < n; i++) {
			if (!(ce[i].flags & FI_REMOTE_CQ_DATA))
				continue;
			signals++;
			printf("SIGNAL %d data %" PRIu64 " flags %s\n",
			       signals, ce[i].data,
			       fi_tostr(&ce[i].flags, FI_TYPE_CQ_EVENT_FLAGS));
			fflush(stdout);
			last_check = 0;
		}
		/*
		 * Scanning the window stops progress, so check only once
		 * every writer has signalled (their writes completed with
		 * FI_DELIVERY_COMPLETE before they signalled), and otherwise
		 * once a second.
		 */
		if ((writers && signals >= writers && last_check == 0) ||
		    now() - last_check > 1.0) {
			last_check = now();
			bad = count_bad(win, expect, size, &first);
			if (!bad && signals >= writers)
				break;
		}
		if (o->snapfile && !snapped && !access(o->snapfile, F_OK)) {
			size_t nz = 0;

			for (size_t j = 0; j < size; j++)
				nz += win[j] != 0;
			printf("SNAPSHOT %zu of %zu bytes nonzero, %d signals\n",
			       nz, size, signals);
			fflush(stdout);
			snapped = true;
		}
		if (now() - start > o->timeout) {
			printf("TIMEOUT %zu of %zu bytes wrong (first at %zu), "
			       "%d of %d signals\n", bad, size, first, signals,
			       writers);
			return 1;
		}
	}
	t_done = now();
	printf("VERIFIED %zu bytes, %d signals, %.2f s after setup\n", size,
	       signals, t_done - start);
	fflush(stdout);
	CHECK(fi_close(&mr->fid));
	free(win);
	free(expect);
	return teardown(&r);
}

static int wait_cq(struct res *r, size_t want, int timeout)
{
	struct fi_cq_entry ce[16];
	double end = now() + timeout;
	size_t got = 0;
	ssize_t n;

	while (got < want) {
		n = fi_cq_read(r->cq, ce, 16);
		if (n > 0) {
			got += n;
		} else if (n == -FI_EAVAIL) {
			report_err(r->cq);
			return -FI_EIO;
		} else if (n != -FI_EAGAIN) {
			return (int) n;
		}
		if (now() > end)
			return -FI_ETIMEDOUT;
	}
	return 0;
}

static int run_write(const struct opts *o, const char *addr_hex,
		     const char *key_hex, uint64_t base, size_t offset,
		     size_t len)
{
	struct res r;
	struct fid_mr *mr = NULL;
	struct fi_context2 *ctx;
	struct fi_cq_entry ce[16];
	uint8_t peer_addr[MAX_ADDR];
	size_t peer_len, nchunks, posted = 0, done = 0, inflight = 0, i;
	uint64_t key;
	fi_addr_t peer;
	uint8_t *buf;
	void *desc = NULL;
	double t0, t1, end;
	ssize_t n;
	int ret;

	if (unhex(addr_hex, peer_addr, sizeof(peer_addr), &peer_len)) {
		fprintf(stderr, "bad address\n");
		return 2;
	}
	key = strtoull(key_hex, NULL, 16);

	memset(&r, 0, sizeof(r));
	CHECK(setup(&r, o, FI_CQ_FORMAT_CONTEXT));
	if (peer_len != r.info->src_addrlen) {
		fprintf(stderr, "address is %zu bytes, provider uses %zu\n",
			peer_len, r.info->src_addrlen);
		return 2;
	}
	buf = aligned_alloc(4096, (len + 4095) & ~(size_t) 4095);
	ctx = calloc(o->depth, sizeof(*ctx));
	if (!buf || !ctx)
		return -FI_ENOMEM;
	for (i = 0; i < len; i++)
		buf[i] = pattern(offset + i);
	if (o->reg_src || (r.info->domain_attr->mr_mode & FI_MR_LOCAL)) {
		CHECK(reg(&r, buf, len, FI_WRITE, &mr));
		desc = fi_mr_desc(mr);
	}

	ret = fi_av_insert(r.av, peer_addr, 1, &peer, 0, NULL);
	if (ret != 1) {
		fprintf(stderr, "fi_av_insert: %d\n", ret);
		return 1;
	}

	nchunks = (len + o->chunk - 1) / o->chunk;
	if (!nchunks)
		nchunks = 1;
	t0 = now();
	end = t0 + o->timeout;
	while (done < nchunks) {
		while (posted < nchunks && inflight < o->depth) {
			size_t off = posted * o->chunk;
			size_t clen = len - off < o->chunk ? len - off :
							     o->chunk;
			void *c = &ctx[posted % o->depth];

			if (o->use_write) {
				ret = fi_write(r.ep, buf + off, clen, desc,
					       peer, base + offset + off, key,
					       c);
			} else {
				struct iovec iov = {
					.iov_base = buf + off, .iov_len = clen,
				};
				struct fi_rma_iov rma = {
					.addr = base + offset + off,
					.len = clen, .key = key,
				};
				struct fi_msg_rma msg = {
					.msg_iov = &iov, .desc = &desc,
					.iov_count = 1, .addr = peer,
					.rma_iov = &rma, .rma_iov_count = 1,
					.context = c,
				};

				ret = fi_writemsg(r.ep, &msg, FI_COMPLETION |
						  FI_DELIVERY_COMPLETE);
			}
			if (ret == -FI_EAGAIN)
				break;
			if (ret) {
				fprintf(stderr, "write: %s\n",
					fi_strerror(-ret));
				return ret;
			}
			posted++;
			inflight++;
		}
		n = fi_cq_read(r.cq, ce, 16);
		if (n > 0) {
			done += n;
			inflight -= n;
		} else if (n == -FI_EAVAIL) {
			report_err(r.cq);
			printf("FAILED after %zu of %zu chunks\n", done,
			       nchunks);
			return 1;
		} else if (n != -FI_EAGAIN) {
			fprintf(stderr, "fi_cq_read: %s\n", fi_strerror(-n));
			return (int) n;
		}
		if (now() > end) {
			printf("TIMEOUT %zu of %zu chunks\n", done, nchunks);
			return 1;
		}
	}
	t1 = now();
	printf("WROTE %zu bytes at offset %zu, %zu chunks of %zu via %s, "
	       "%.3f s, %.1f MiB/s\n", len, offset, nchunks, o->chunk,
	       o->use_write ? "fi_write" : "fi_writemsg", t1 - t0,
	       len / (t1 - t0) / (1 << 20));
	fflush(stdout);

	if (o->signal) {
		CHECK(fi_writedata(r.ep, NULL, 0, NULL, offset, peer,
				   base + offset, key, &ctx[0]));
		ret = wait_cq(&r, 1, o->timeout);
		if (ret) {
			fprintf(stderr, "signal: %s\n", fi_strerror(-ret));
			return 1;
		}
		printf("SIGNALLED target\n");
	}

	CHECK(fi_av_remove(r.av, &peer, 1, 0));
	if (mr)
		CHECK(fi_close(&mr->fid));
	free(buf);
	free(ctx);
	return teardown(&r);
}

/* one write, timed */
struct mt_sample {
	double t0, t1;
};

struct mt_writer {
	struct res *r;
	const struct opts *o;
	fi_addr_t peer;
	uint64_t key, base;
	const uint8_t *buf;
	size_t len;
	atomic_bool stop;
	struct mt_sample *s;
	size_t n, cap;
	int err;
};

static void *mt_write_loop(void *arg)
{
	struct mt_writer *w = arg;
	struct fi_context2 ctx;
	struct fi_cq_entry ce;
	size_t off = 0, clen;
	double t0;
	ssize_t n;
	int ret;

	while (!atomic_load(&w->stop) && w->n < w->cap) {
		clen = w->len - off < w->o->chunk ? w->len - off : w->o->chunk;
		t0 = now();
		while ((ret = fi_write(w->r->ep, w->buf + off, clen, NULL,
				       w->peer, w->base + off, w->key, &ctx)) ==
		       -FI_EAGAIN)
			(void) fi_cq_read(w->r->cq, &ce, 0);
		if (ret) {
			w->err = ret;
			break;
		}
		while ((n = fi_cq_read(w->r->cq, &ce, 1)) == -FI_EAGAIN &&
		       now() - t0 < 10)
			;
		if (n != 1) {
			if (n == -FI_EAVAIL)
				report_err(w->r->cq);
			w->err = n == -FI_EAGAIN ? -FI_ETIMEDOUT : (int) n;
			break;
		}
		w->s[w->n].t0 = t0;
		w->s[w->n].t1 = now();
		w->n++;
		off = off + clen == w->len ? 0 : off + clen;
	}
	return NULL;
}

static int run_mt(const struct opts *o, const char *addr_hex,
		  const char *key_hex, uint64_t base, size_t len,
		  const char *dead_hex)
{
	struct opts mo = *o;
	struct res r;
	struct mt_writer w;
	struct fi_context2 ctx;
	struct fi_cq_err_entry err;
	uint8_t peer_addr[MAX_ADDR], dead_addr[MAX_ADDR];
	size_t peer_len, dead_len, during = 0, i;
	fi_addr_t dead;
	pthread_t thr;
	double ti0, ti1, td0, td1, worst_in = 0, worst_out = 0, lat;
	ssize_t n;
	uint8_t *buf;
	int ret, rc = 0;

	if (unhex(addr_hex, peer_addr, sizeof(peer_addr), &peer_len) ||
	    unhex(dead_hex, dead_addr, sizeof(dead_addr), &dead_len)) {
		fprintf(stderr, "bad address\n");
		return 2;
	}
	memset(&r, 0, sizeof(r));
	memset(&w, 0, sizeof(w));
	mo.thread_safe = true;
	CHECK(setup(&r, &mo, FI_CQ_FORMAT_CONTEXT));
	if (r.info->domain_attr->threading != FI_THREAD_SAFE) {
		fprintf(stderr, "the provider is not FI_THREAD_SAFE\n");
		return 1;
	}
	buf = aligned_alloc(4096, (len + 4095) & ~(size_t) 4095);
	if (!buf)
		return -FI_ENOMEM;
	for (i = 0; i < len; i++)
		buf[i] = pattern(i);

	ret = fi_av_insert(r.av, peer_addr, 1, &w.peer, 0, NULL);
	if (ret != 1) {
		fprintf(stderr, "fi_av_insert: %d\n", ret);
		return 1;
	}
	w.r = &r;
	w.o = o;
	w.key = strtoull(key_hex, NULL, 16);
	w.base = base;
	w.buf = buf;
	w.len = len;
	w.cap = 1 << 20;
	w.s = calloc(w.cap, sizeof(*w.s));
	if (!w.s)
		return -FI_ENOMEM;
	atomic_init(&w.stop, false);
	if (pthread_create(&thr, NULL, mt_write_loop, &w))
		return 1;

	usleep(300000);
	ti0 = now();
	ret = fi_av_insert(r.av, dead_addr, 1, &dead, 0, NULL);
	ti1 = now();
	usleep(300000);
	atomic_store(&w.stop, true);
	pthread_join(thr, NULL);
	if (ret != 1 || w.err) {
		fprintf(stderr, "insert %d, writer %d (%s)\n", ret, w.err,
			fi_strerror(-w.err));
		return 1;
	}

	for (i = 0; i < w.n; i++) {
		lat = w.s[i].t1 - w.s[i].t0;
		if (w.s[i].t1 > ti0 && w.s[i].t0 < ti1) {
			during++;
			if (lat > worst_in)
				worst_in = lat;
		} else if (lat > worst_out) {
			worst_out = lat;
		}
	}
	printf("MT insert of an unanswering peer took %.3f s; %zu writes "
	       "of %zu bytes overlapped it, the slowest %.1f ms (%.1f ms "
	       "otherwise, %zu writes in all)\n", ti1 - ti0, during, o->chunk,
	       worst_in * 1e3, worst_out * 1e3, w.n);
	if (ti1 - ti0 < 0.5 || during < 10 || worst_in > 0.2) {
		printf("MT FAILED: the insert did not wait, or the writes "
		       "waited for it\n");
		rc = 1;
	}

	/* the dead peer: a write fails at once, it does not hang */
	td0 = now();
	ret = fi_write(r.ep, buf, o->chunk, NULL, dead, base, w.key, &ctx);
	if (!ret) {
		while ((n = fi_cq_read(r.cq, &err, 1)) == -FI_EAGAIN &&
		       now() - td0 < 10)
			;
		memset(&err, 0, sizeof(err));
		if (n == -FI_EAVAIL && fi_cq_readerr(r.cq, &err, 0) == 1)
			ret = -err.err;
		else
			ret = n == 1 ? 0 : (int) n;
	}
	td1 = now();
	printf("DEAD write to the unanswering peer: %s after %.1f ms\n",
	       ret ? fi_strerror(-ret) : "completed", (td1 - td0) * 1e3);
	if (!ret || td1 - td0 > 0.5)
		rc = 1;

	CHECK(fi_writedata(r.ep, NULL, 0, NULL, 0, w.peer, base, w.key, &ctx));
	if (wait_cq(&r, 1, o->timeout))
		rc = 1;
	CHECK(fi_av_remove(r.av, &dead, 1, 0));
	CHECK(fi_av_remove(r.av, &w.peer, 1, 0));
	free(w.s);
	free(buf);
	ret = teardown(&r);
	return rc ? rc : ret;
}

static int run_pair(const struct opts *o, size_t size, const char *peerfile)
{
	struct res r;
	struct fid_mr *mr;
	struct fi_cq_data_entry ce[16];
	struct fi_context2 *ctx, sig_ctx;
	char h[2 * MAX_ADDR + 1], peer_hex[2 * MAX_ADDR + 1];
	uint8_t *win, *expect, *src, peer_addr[MAX_ADDR];
	uint64_t base, peer_base, peer_key;
	size_t peer_len, peer_size, nchunks, posted = 0, done = 0;
	size_t inflight = 0, i, first;
	fi_addr_t peer;
	bool signal_posted = false, signal_done = false, verified = false;
	int signals = 0, ret;
	double start, end, t_wrote = 0;
	ssize_t n;
	FILE *f;

	memset(&r, 0, sizeof(r));
	CHECK(setup(&r, o, FI_CQ_FORMAT_DATA));
	win = aligned_alloc(4096, (size + 4095) & ~(size_t) 4095);
	expect = malloc(size);
	ctx = calloc(o->depth, sizeof(*ctx));
	if (!win || !expect || !ctx)
		return -FI_ENOMEM;
	memset(win, 0, size);
	for (i = 0; i < size; i++)
		expect[i] = pattern(i);
	/* the peer's window gets the same pattern, so send from 'expect' */
	src = expect;
	CHECK(reg(&r, win, size, FI_WRITE | FI_REMOTE_WRITE, &mr));
	base = (r.info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) ?
	       (uintptr_t) win : 0;
	hex(r.addr, r.addrlen, h);
	if (!o->outfile)
		return -FI_EINVAL;
	f = fopen(o->outfile, "w");
	if (!f)
		return -errno;
	fprintf(f, "%s %016" PRIx64 " %" PRIu64 " %zu\n", h, fi_mr_key(mr),
		base, size);
	fclose(f);
	printf("ADDR %s\nKEY %016" PRIx64 "\n", h, fi_mr_key(mr));
	fflush(stdout);

	/* keep answering while the peer starts up */
	start = now();
	end = start + o->timeout;
	for (;;) {
		f = fopen(peerfile, "r");
		if (f) {
			ret = fscanf(f, "%128s %" SCNx64 " %" SCNu64 " %zu",
				     peer_hex, &peer_key, &peer_base,
				     &peer_size);
			fclose(f);
			if (ret == 4)
				break;
		}
		n = fi_cq_read(r.cq, ce, 16);
		for (i = 0; n > 0 && i < (size_t) n; i++)
			if (ce[i].flags & FI_REMOTE_CQ_DATA)
				signals++;
		if (now() > end) {
			printf("TIMEOUT waiting for %s\n", peerfile);
			return 1;
		}
	}
	if (unhex(peer_hex, peer_addr, sizeof(peer_addr), &peer_len) ||
	    peer_size < size)
		return 2;
	if (fi_av_insert(r.av, peer_addr, 1, &peer, 0, NULL) != 1)
		return 1;

	nchunks = (size + o->chunk - 1) / o->chunk;
	start = now();
	while (!(signal_done && verified)) {
		while (posted < nchunks && inflight < o->depth) {
			size_t off = posted * o->chunk;
			size_t clen = size - off < o->chunk ? size - off :
							      o->chunk;
			struct iovec iov = { .iov_base = src + off,
					     .iov_len = clen };
			struct fi_rma_iov rma = { .addr = peer_base + off,
						  .len = clen,
						  .key = peer_key };
			struct fi_msg_rma msg = {
				.msg_iov = &iov, .iov_count = 1,
				.addr = peer, .rma_iov = &rma,
				.rma_iov_count = 1,
				.context = &ctx[posted % o->depth],
			};

			ret = fi_writemsg(r.ep, &msg, FI_COMPLETION |
					  FI_DELIVERY_COMPLETE);
			if (ret == -FI_EAGAIN)
				break;
			if (ret) {
				fprintf(stderr, "fi_writemsg: %s\n",
					fi_strerror(-ret));
				return 1;
			}
			posted++;
			inflight++;
		}
		if (done == nchunks && !signal_posted) {
			t_wrote = now();
			ret = fi_writedata(r.ep, NULL, 0, NULL, 1, peer,
					   peer_base, peer_key, &sig_ctx);
			if (ret && ret != -FI_EAGAIN) {
				fprintf(stderr, "fi_writedata: %s\n",
					fi_strerror(-ret));
				return 1;
			}
			signal_posted = !ret;
		}
		n = fi_cq_read(r.cq, ce, 16);
		if (n == -FI_EAVAIL) {
			report_err(r.cq);
			return 1;
		}
		for (i = 0; n > 0 && i < (size_t) n; i++) {
			if (ce[i].flags & FI_REMOTE_CQ_DATA) {
				signals++;
			} else if (ce[i].op_context == &sig_ctx) {
				signal_done = true;
			} else {
				done++;
				inflight--;
			}
		}
		if (signals && !verified) {
			/* the peer signals after its writes completed */
			if (count_bad(win, expect, size, &first)) {
				printf("MISMATCH at %zu\n", first);
				return 1;
			}
			verified = true;
		}
		if (now() > end) {
			printf("TIMEOUT wrote %zu of %zu chunks, %d signals\n",
			       done, nchunks, signals);
			return 1;
		}
	}
	printf("PAIR wrote %zu bytes to the peer in %.3f s (%.1f MiB/s) "
	       "while the peer filled our window; ours VERIFIED\n", size,
	       t_wrote - start, size / (t_wrote - start) / (1 << 20));
	fflush(stdout);

	/* let the peer finish before our endpoint stops answering */
	end = now() + 2;
	while (now() < end)
		fi_cq_read(r.cq, ce, 16);

	CHECK(fi_av_remove(r.av, &peer, 1, 0));
	CHECK(fi_close(&mr->fid));
	free(win);
	free(expect);
	free(ctx);
	return teardown(&r);
}

static bool dir_file(const char *dir, const char *name, bool create)
{
	char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, name);
	if (!create)
		return !access(path, F_OK);
	f = fopen(path, "w");
	if (f)
		fclose(f);
	return f != NULL;
}

/* poll the CQ until 'want' transmit completions or the deadline */
static int poll_tx(struct res *r, size_t want, double deadline,
		   int *signals)
{
	struct fi_cq_data_entry ce[16];
	size_t got = 0;
	ssize_t n, i;

	while (got < want) {
		n = fi_cq_read(r->cq, ce, 16);
		if (n == -FI_EAVAIL) {
			report_err(r->cq);
			return -FI_EIO;
		}
		for (i = 0; i < n; i++) {
			if (ce[i].flags & FI_REMOTE_CQ_DATA)
				(*signals)++;
			else
				got++;
		}
		if (now() > deadline)
			return -FI_ETIMEDOUT;
	}
	return 0;
}

static int run_cutoff(const struct opts *o, const char *addr_hex,
		      const char *key_hex, uint64_t base, size_t len,
		      int budget_ms, const char *dir)
{
	struct res r;
	struct fid_mr *mr;
	struct fi_context2 ctx[2];
	char h[2 * MAX_ADDR + 1], path[512];
	uint8_t peer_addr[MAX_ADDR], *src, *zero, *win;
	uint64_t key, my_base;
	size_t peer_len, i, first;
	fi_addr_t peer;
	double t0, tc0, tc1, deadline;
	int ret, signals = 0;
	FILE *f;

	if (unhex(addr_hex, peer_addr, sizeof(peer_addr), &peer_len))
		return 2;
	key = strtoull(key_hex, NULL, 16);

	memset(&r, 0, sizeof(r));
	CHECK(setup(&r, o, FI_CQ_FORMAT_DATA));
	src = malloc(len);
	zero = calloc(1, 4096);
	win = aligned_alloc(4096, (len + 4095) & ~(size_t) 4095);
	if (!src || !zero || !win)
		return -FI_ENOMEM;
	for (i = 0; i < len; i++)
		src[i] = pattern(i);
	memset(win, 0, len);
	if (fi_av_insert(r.av, peer_addr, 1, &peer, 0, NULL) != 1)
		return 1;

	/* warm-up: the peer is resolved and reachable, nothing visible lands */
	CHECK(fi_write(r.ep, zero, 4096, NULL, peer, base, key, &ctx[0]));
	CHECK(poll_tx(&r, 1, now() + o->timeout, &signals));
	dir_file(dir, "ready", true);
	while (!dir_file(dir, "go", false))
		fi_cq_read(r.cq, NULL, 0);

	/* the link is down now: this write cannot complete in time */
	t0 = now();
	CHECK(fi_write(r.ep, src, len, NULL, peer, base, key, &ctx[0]));
	ret = poll_tx(&r, 1, t0 + budget_ms / 1000.0, &signals);
	if (ret != -FI_ETIMEDOUT) {
		printf("FAILED the write did not miss its deadline (%d)\n", ret);
		return 1;
	}
	tc0 = now();
	CHECK(fi_close(&r.ep->fid));
	tc1 = now();
	CHECK(fi_close(&r.cq->fid));
	r.ep = NULL;
	r.cq = NULL;
	printf("CUTOFF after %.2f s, fi_close(ep) took %.3f ms\n", tc0 - t0,
	       (tc1 - tc0) * 1000);
	fflush(stdout);

	/* carry on with a new endpoint on the same domain and AV */
	CHECK(open_ep(&r, FI_CQ_FORMAT_DATA));
	CHECK(reg(&r, win, len, FI_WRITE | FI_REMOTE_WRITE, &mr));
	my_base = (r.info->domain_attr->mr_mode & FI_MR_VIRT_ADDR) ?
		  (uintptr_t) win : 0;
	hex(r.addr, r.addrlen, h);
	snprintf(path, sizeof(path), "%s/win.tmp", dir);
	f = fopen(path, "w");
	if (!f)
		return 1;
	fprintf(f, "%s %016" PRIx64 " %" PRIu64 " %zu\n", h, fi_mr_key(mr),
		my_base, len);
	fclose(f);
	snprintf(h, sizeof(h), "%s/win", dir);
	rename(path, h);
	printf("REOPENED a new endpoint and window, key %016" PRIx64 "\n",
	       fi_mr_key(mr));
	fflush(stdout);

	while (!dir_file(dir, "rewrite", false))
		fi_cq_read(r.cq, NULL, 0);
	t0 = now();
	CHECK(fi_write(r.ep, src, len, NULL, peer, base, key, &ctx[0]));
	CHECK(poll_tx(&r, 1, t0 + o->timeout, &signals));
	CHECK(fi_writedata(r.ep, NULL, 0, NULL, 1, peer, base, key, &ctx[1]));
	CHECK(poll_tx(&r, 1, t0 + o->timeout, &signals));
	printf("REWROTE %zu bytes with the new endpoint in %.3f s and "
	       "signalled\n", len, now() - t0);
	fflush(stdout);

	/* and the new window takes writes */
	deadline = now() + o->timeout;
	while (!signals) {
		struct fi_cq_data_entry ce[16];
		ssize_t n = fi_cq_read(r.cq, ce, 16);

		if (n == -FI_EAVAIL) {
			report_err(r.cq);
			return 1;
		}
		for (ssize_t j = 0; j < n; j++)
			if (ce[j].flags & FI_REMOTE_CQ_DATA)
				signals++;
		if (now() > deadline) {
			printf("TIMEOUT waiting for the peer\n");
			return 1;
		}
	}
	if (count_bad(win, src, len, &first)) {
		printf("MISMATCH in the new window at %zu\n", first);
		return 1;
	}
	printf("NEW WINDOW VERIFIED %zu bytes written by a peer\n", len);
	fflush(stdout);

	/* keep answering while the peer and target finish */
	deadline = now() + 1;
	while (now() < deadline)
		fi_cq_read(r.cq, NULL, 0);

	CHECK(fi_close(&mr->fid));
	free(src);
	free(zero);
	free(win);
	return teardown(&r);
}

static void usage(void)
{
	fprintf(stderr,
		"usage: test_rma [-i ifname] info\n"
		"       test_rma [-i ifname] [-o file] [-t secs] target "
		"<size> [writers]\n"
		"       test_rma [-i ifname] [-c chunk] [-q depth] "
		"[-a writemsg|write] [-r] [-n] [-t secs]\n"
		"                write <addr-hex> <key-hex> <base> <offset> "
		"<len>\n"
		"       test_rma [-i ifname] -o file [-c chunk] [-q depth] "
		"pair <size> <peerfile>\n"
		"       test_rma [-i ifname] cutoff <addr-hex> <key-hex> "
		"<base> <len> <budget-ms> <dir>\n"
		"       test_rma [-i ifname] [-c chunk] mt <addr-hex> "
		"<key-hex> <base> <len> <dead-addr-hex>\n"
		"       (-m: FI_AV_MAP, -t secs: timeout)\n");
}

int main(int argc, char **argv)
{
	struct opts o = {
		.ifname = getenv("UET_IFNAME"),
		.chunk = 65536,
		.depth = 16,
		.signal = true,
		.timeout = 120,
	};
	int c, ret;

	while ((c = getopt(argc, argv, "i:o:c:q:a:rnmz:t:")) != -1) {
		switch (c) {
		case 'i': o.ifname = optarg; break;
		case 'o': o.outfile = optarg; break;
		case 'c': o.chunk = strtoull(optarg, NULL, 0); break;
		case 'q': o.depth = strtoull(optarg, NULL, 0); break;
		case 'a': o.use_write = !strcmp(optarg, "write"); break;
		case 'r': o.reg_src = true; break;
		case 'n': o.signal = false; break;
		case 'm': o.av_map = true; break;
		case 'z': o.snapfile = optarg; break;
		case 't': o.timeout = atoi(optarg); break;
		default: usage(); return 2;
		}
	}
	argc -= optind;
	argv += optind;
	if (!o.chunk || !o.depth) {
		usage();
		return 2;
	}

	if (argc == 1 && !strcmp(argv[0], "info"))
		ret = run_info(&o);
	else if ((argc == 2 || argc == 3) && !strcmp(argv[0], "target"))
		ret = run_target(&o, strtoull(argv[1], NULL, 0),
				 argc == 3 ? atoi(argv[2]) : 0);
	else if (argc == 7 && !strcmp(argv[0], "cutoff"))
		ret = run_cutoff(&o, argv[1], argv[2],
				 strtoull(argv[3], NULL, 0),
				 strtoull(argv[4], NULL, 0), atoi(argv[5]),
				 argv[6]);
	else if (argc == 3 && !strcmp(argv[0], "pair"))
		ret = run_pair(&o, strtoull(argv[1], NULL, 0), argv[2]);
	else if (argc == 6 && !strcmp(argv[0], "mt"))
		ret = run_mt(&o, argv[1], argv[2], strtoull(argv[3], NULL, 0),
			     strtoull(argv[4], NULL, 0), argv[5]);
	else if (argc == 6 && !strcmp(argv[0], "write"))
		ret = run_write(&o, argv[1], argv[2],
				strtoull(argv[3], NULL, 0),
				strtoull(argv[4], NULL, 0),
				strtoull(argv[5], NULL, 0));
	else {
		usage();
		return 2;
	}
	return ret ? 1 : 0;
}
