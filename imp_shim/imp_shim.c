/*
 * Copyright (c) 2026, Broadcom. All rights reserved. The term
 * Broadcom refers to Broadcom Limited and/or its subsidiaries.
 */

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <arpa/inet.h>
#include <linux/ipv6.h>
#include <linux/udp.h>

#include <ofi_list.h>

#include "uet_log.h"
#include "uet_nic.h"
#include "uet_pkt_hdr.h"
#include "crc32c.h"
#include "tomlc17.h"
#include "imp_shim.h"

/* initial number of packet buffers to pre-allocate per plane */
#define IMP_POOL_INIT_PER_PLANE 32

/*
 * Packet mangling (test hook).
 *
 * A packet matching the configured pkt_type/flags filter is either mutated
 * (a PDS header field is corrupted so the peer replies with a NACK) or
 * dropped (only the transmit copy, so the initiator RTO-retransmits). This
 * exercises the initiator's NACK/retransmit processing, otherwise unreachable
 * on the normal data path. See imp_shim_mangle_pkt() for details.
 */
typedef enum {
	IMP_MANGLE_ACTION_MANGLE = 0, /* corrupt a header field (default) */
	IMP_MANGLE_ACTION_DROP,       /* drop the matched packet */
} imp_mangle_action_t;

typedef enum {
	IMP_MANGLE_FIELD_NONE = 0,
	IMP_MANGLE_FIELD_PSN,     /* -> peer PSN_OOR_WINDOW NACK (RETX) */
	IMP_MANGLE_FIELD_SPDCID,  /* -> peer PDC mismatch NACK (CLOSE) */
	IMP_MANGLE_FIELD_DPDCID,  /* -> peer PDC mismatch NACK (CLOSE) */
	IMP_MANGLE_FIELD_SYN,     /* -> peer INVALID_SYN NACK (CLOSE) */
} imp_mangle_field_t;

/* mangle.pkt_type sentinels: any request type (default) or invalid config */
#define IMP_MANGLE_PKT_TYPE_ANY     (-1)
#define IMP_MANGLE_PKT_TYPE_INVALID (-2)

/* packet entry */
struct imp_pkt {
	struct slist_entry entry;      /* list linkage (free pool or tx queue) */
	size_t             pkt_size;   /* the size of the packet */
	size_t             iphdr_off;  /* offset of IP header from pkt_data */
	struct timespec    tx_time;    /* calculated transmit time */
	uint8_t            pkt_data[]; /* flexible size for max_pkt_size */
};

/* per-plane Tx queue */
struct imp_plane {
	struct slist    list;  /* packet list */
	pthread_mutex_t lock;  /* mutex for list management */
	uint32_t        depth; /* current number of packets on the queue */
};

/* packet entry free pool - grows as needed, never shrinks */
struct imp_pool {
	struct slist    free_list;  /* free list */
	pthread_mutex_t lock;       /* mutex for list management */
	size_t          entry_size; /* sizeof(imp_pkt) + max_pkt_size */
	uint32_t        free_count; /* current number of free entries */
	uint32_t        total;      /* total number of entries allocated */
};

/* impairment shim state */
struct imp_shim_state {
	bool              enabled;
	struct uet_nic   *nic;

	bool              running;
	pthread_t         tx_thread;

	int               num_planes; /* number of Tx queues */
	bool              random_enq; /* random vs round-robin selection */
	int               drop_rate;  /* hundredths of a percent */
	uint64_t          delay_max;  /* nanoseconds (random 0..delay_max) */

	/* packet mangling test hook (disabled unless configured) */
	bool                mangle_enable;
	int                 mangle_rate;   /* hundredths of a percent */
	imp_mangle_action_t mangle_action; /* mangle field or drop */
	imp_mangle_field_t  mangle_field;
	int                 mangle_pkt_type; /* PDS type to target, or ANY */
	uint8_t             mangle_flags;    /* these prlg flags must be set */
	bool                mangle_skip_syn; /* never touch SYN (setup) packets */
	uint32_t            mangle_value;

	struct imp_plane *planes;     /* Tx queues */
	uint32_t          enq_idx;    /* round-robin enqueue index */
	struct imp_pool   pool;       /* packet buffer free pool */
};

static struct imp_shim_state imp_state;

static imp_mangle_action_t imp_mangle_action_from_str(const char *s)
{
	if (s != NULL && strcmp(s, "drop") == 0)
		return IMP_MANGLE_ACTION_DROP;
	return IMP_MANGLE_ACTION_MANGLE; /* default */
}

static const char *imp_mangle_action_str(imp_mangle_action_t action)
{
	return (action == IMP_MANGLE_ACTION_DROP) ? "drop" : "mangle";
}

static const char *imp_mangle_field_str(imp_mangle_field_t field)
{
	switch (field) {
	case IMP_MANGLE_FIELD_PSN:    return "psn";
	case IMP_MANGLE_FIELD_SPDCID: return "spdcid";
	case IMP_MANGLE_FIELD_DPDCID: return "dpdcid";
	case IMP_MANGLE_FIELD_SYN:    return "syn";
	default:                      return "none";
	}
}

static imp_mangle_field_t imp_mangle_field_from_str(const char *s)
{
	if (s == NULL)
		return IMP_MANGLE_FIELD_NONE;
	if (strcmp(s, "psn") == 0)
		return IMP_MANGLE_FIELD_PSN;
	if (strcmp(s, "spdcid") == 0)
		return IMP_MANGLE_FIELD_SPDCID;
	if (strcmp(s, "dpdcid") == 0)
		return IMP_MANGLE_FIELD_DPDCID;
	if (strcmp(s, "syn") == 0)
		return IMP_MANGLE_FIELD_SYN;
	return IMP_MANGLE_FIELD_NONE;
}

/*
 * Name <-> value table for the full uet_pds_pkt_type_t enum. Lets the config
 * target any PDS packet type by name for fault injection.
 */
static const struct {
	const char        *name;
	uet_pds_pkt_type_t type;
} imp_mangle_pkt_type_map[] = {
	{ "reserved",   UET_PDS_TYPE_RESERVED   },
	{ "security",   UET_PDS_TYPE_SECURITY   },
	{ "rud_req",    UET_PDS_TYPE_RUD_REQ    },
	{ "rod_req",    UET_PDS_TYPE_ROD_REQ    },
	{ "rudi_req",   UET_PDS_TYPE_RUDI_REQ   },
	{ "rudi_resp",  UET_PDS_TYPE_RUDI_RESP  },
	{ "uud_req",    UET_PDS_TYPE_UUD_REQ    },
	{ "ack",        UET_PDS_TYPE_ACK        },
	{ "ack_cc",     UET_PDS_TYPE_ACK_CC     },
	{ "ack_ccx",    UET_PDS_TYPE_ACK_CCX    },
	{ "nack",       UET_PDS_TYPE_NACK       },
	{ "ctrl",       UET_PDS_TYPE_CTRL       },
	{ "nack_ccx",   UET_PDS_TYPE_NACK_CCX   },
	{ "rud_cc_req", UET_PDS_TYPE_RUD_CC_REQ },
	{ "rod_cc_req", UET_PDS_TYPE_ROD_CC_REQ },
};

/*
 * Parse mangle.pkt_type into a PDS type value. Absent or "any" targets every
 * packet type (default); an unrecognized value returns
 * IMP_MANGLE_PKT_TYPE_INVALID.
 */
static int imp_mangle_pkt_type_from_str(const char *s)
{
	size_t i;

	if (s == NULL || strcmp(s, "any") == 0)
		return IMP_MANGLE_PKT_TYPE_ANY;

	for (i = 0; i < (sizeof(imp_mangle_pkt_type_map) /
			 sizeof(imp_mangle_pkt_type_map[0])); i++) {
		if (strcmp(s, imp_mangle_pkt_type_map[i].name) == 0)
			return (int)imp_mangle_pkt_type_map[i].type;
	}

	return IMP_MANGLE_PKT_TYPE_INVALID;
}

static const char *imp_mangle_pkt_type_str(int type)
{
	size_t i;

	if (type == IMP_MANGLE_PKT_TYPE_ANY)
		return "any";

	for (i = 0; i < (sizeof(imp_mangle_pkt_type_map) /
			 sizeof(imp_mangle_pkt_type_map[0])); i++) {
		if ((int)imp_mangle_pkt_type_map[i].type == type)
			return imp_mangle_pkt_type_map[i].name;
	}

	return "unknown";
}

/*
 * Allocate a single pool entry. Called during init to seed the pool and at
 * runtime when the free list is exhausted.
 */
static struct imp_pkt *imp_pool_alloc_entry(struct imp_pool *pool)
{
	struct imp_pkt *pkt;

	pkt = calloc(1, pool->entry_size);
	if (pkt != NULL)
		pool->total++;

	return pkt;
}

/*
 * Get a packet buffer from the pool. If the free list is empty, a new entry
 * is allocated (the pool grows but never shrinks).
 */
static struct imp_pkt *imp_pool_get(struct imp_pool *pool)
{
	struct imp_pkt *pkt = NULL;

	pthread_mutex_lock(&pool->lock);

	if (!slist_empty(&pool->free_list)) {
		slist_remove_head_container(&pool->free_list,
					    struct imp_pkt, pkt, entry);
		pool->free_count--;
	} else {
		pkt = imp_pool_alloc_entry(pool);
		if (pkt != NULL)
			UET_IMP_INFO("pool grew to %u entries", pool->total);
	}

	pthread_mutex_unlock(&pool->lock);

	return pkt;
}

/* Return a packet buffer to the pool's free list. */
static void imp_pool_put(struct imp_pool *pool,
			 struct imp_pkt *pkt)
{
	pthread_mutex_lock(&pool->lock);

	slist_insert_head(&pkt->entry, &pool->free_list);
	pool->free_count++;

	pthread_mutex_unlock(&pool->lock);
}

/*
 * Initialize the packet buffer free pool. Pre-allocates entries based on
 * (num_planes * IMP_POOL_INIT_PER_PLANE).
 */
static int imp_pool_init(struct imp_pool *pool,
			 size_t max_pkt_size,
			 int num_planes)
{
	struct imp_pkt *pkt;
	int count;
	int i;

	memset(pool, 0, sizeof(*pool));
	pthread_mutex_init(&pool->lock, NULL);
	slist_init(&pool->free_list);
	pool->entry_size = (sizeof(struct imp_pkt) + max_pkt_size);

	count = (num_planes * IMP_POOL_INIT_PER_PLANE);

	for (i = 0; i < count; i++) {
		pkt = imp_pool_alloc_entry(pool);
		if (pkt == NULL) {
			UET_IMP_ERR("failed to pre-allocate pool entry");
			return -ENOMEM;
		}

		slist_insert_head(&pkt->entry, &pool->free_list);
		pool->free_count++;
	}

	UET_IMP_INFO("pool: pre-allocated %d entries (size = %zu bytes)",
		     count, pool->entry_size);

	return 0;
}

/*
 * Free all packet buffer free pool entries (any entries still on the plane
 * queues must be drained before calling this function).
 */
static void imp_pool_finalize(struct imp_pool *pool)
{
	struct imp_pkt *pkt;

	while (!slist_empty(&pool->free_list)) {
		slist_remove_head_container(&pool->free_list,
					    struct imp_pkt, pkt, entry);
		free(pkt);
	}

	pool->free_count = 0;
	pool->total = 0;

	pthread_mutex_destroy(&pool->lock);
}

/* Enqueue a packet onto a plane's Tx queue. */
static void imp_plane_enqueue(struct imp_plane *plane,
			      struct imp_pkt *pkt)
{
	pthread_mutex_lock(&plane->lock);

	slist_insert_tail(&pkt->entry, &plane->list);
	plane->depth++;

	pthread_mutex_unlock(&plane->lock);
}

/*
 * Try to dequeue a packet from the front of a plane's Tx queue if its transmit
 * time has been reached. Returns the packet if dequeued, NULL otherwise.
 */
static struct imp_pkt *imp_plane_try_dequeue(struct imp_plane *plane,
					     struct timespec *now)
{
	struct imp_pkt *pkt;

	pthread_mutex_lock(&plane->lock);

	if (slist_empty(&plane->list)) {
		pthread_mutex_unlock(&plane->lock);
		return NULL;
	}

	/* peek at the head packet */
	pkt = container_of(plane->list.head, struct imp_pkt, entry);

	/* check if the head packet's transmit time has been reached */
	if ((now->tv_sec > pkt->tx_time.tv_sec) ||
	    ((now->tv_sec == pkt->tx_time.tv_sec) &&
	     (now->tv_nsec >= pkt->tx_time.tv_nsec))) {
		slist_remove_head(&plane->list);
		plane->depth--;
		pthread_mutex_unlock(&plane->lock);
		return pkt;
	}

	pthread_mutex_unlock(&plane->lock);

	UET_IMP_DBG("packet skipped");
	return NULL;
}

/*
 * Transmit thread.
 *
 * Processes all planes in round-robin fashion. For each plane, attempt to
 * dequeue and transmit the head packet if its transmit time has been
 * reached. After transmission, the packet buffer is returned to the pool.
 */
static void *imp_tx_thread(void *arg)
{
	struct imp_shim_state *st = (struct imp_shim_state *)arg;
	struct timespec now;
	struct imp_pkt *pkt;
	int plane_idx;

	UET_IMP_INFO("Tx thread started");

	while (st->running) {
		for (plane_idx = 0; plane_idx < st->num_planes; plane_idx++) {
			clock_gettime(CLOCK_MONOTONIC, &now);

			pkt = imp_plane_try_dequeue(&st->planes[plane_idx],
						    &now);
			if (pkt == NULL)
				continue;

			uet_nic_tx_pkt(st->nic,
				       pkt->pkt_data,
				       (pkt->pkt_data + pkt->iphdr_off),
				       pkt->pkt_size);

			imp_pool_put(&st->pool, pkt);
		}

		sched_yield(); /* Don't hog the CPU... */
	}

	UET_IMP_INFO("Tx thread exiting");
	return NULL;
}

/* Read and validate the TOML configuration file. */
static int imp_read_config(const char *config_path)
{
	toml_result_t result;
	toml_datum_t val;

	result = toml_parse_file_ex(config_path);
	if (!result.ok) {
		UET_IMP_ERR("failed to parse config '%s': %s",
			    config_path, result.errmsg);
		return -EINVAL;
	}

	/* num_planes (required) */
	val = toml_seek(result.toptab, "config.num_planes");
	if (val.type != TOML_INT64 || val.u.int64 < 1) {
		UET_IMP_ERR("config: num_planes must be a positive integer");
		toml_free(result);
		return -EINVAL;
	}
	imp_state.num_planes = (int)val.u.int64;

	/* random_enq (required) */
	val = toml_seek(result.toptab, "config.random_enq");
	if (val.type != TOML_BOOLEAN) {
		UET_IMP_ERR("config: random_enq must be a boolean");
		toml_free(result);
		return -EINVAL;
	}
	imp_state.random_enq = val.u.boolean;

	/* drop_rate (required) */
	val = toml_seek(result.toptab, "config.drop_rate");
	if (val.type != TOML_INT64 || val.u.int64 < 0) {
		UET_IMP_ERR("config: drop_rate must be a non-negative integer");
		toml_free(result);
		return -EINVAL;
	}
	imp_state.drop_rate = (int)val.u.int64;

	/* delay_max (required) */
	val = toml_seek(result.toptab, "config.delay_max");
	if (val.type != TOML_INT64 || val.u.int64 < 0) {
		UET_IMP_ERR("config: delay_max must be a non-negative integer");
		toml_free(result);
		return -EINVAL;
	}
	imp_state.delay_max = (uint64_t)val.u.int64;

	/*
	 * [mangle] (optional test hook). Absent or enable=false leaves the
	 * hook disabled and the normal Tx path untouched.
	 */
	val = toml_seek(result.toptab, "mangle.enable");
	imp_state.mangle_enable = (val.type == TOML_BOOLEAN) ? val.u.boolean
							     : false;
	if (imp_state.mangle_enable) {
		val = toml_seek(result.toptab, "mangle.rate");
		imp_state.mangle_rate = (val.type == TOML_INT64) ?
					(int)val.u.int64 : 0;

		val = toml_seek(result.toptab, "mangle.action");
		imp_state.mangle_action = imp_mangle_action_from_str(
			(val.type == TOML_STRING) ? val.u.s : NULL);

		val = toml_seek(result.toptab, "mangle.field");
		imp_state.mangle_field = imp_mangle_field_from_str(
			(val.type == TOML_STRING) ? val.u.s : NULL);

		val = toml_seek(result.toptab, "mangle.flags");
		imp_state.mangle_flags = (val.type == TOML_INT64) ?
			((uint8_t)val.u.int64 & UET_PDS_FLAGS_MASK) : 0;

		/*
		 * skip_syn (default true): never mangle/drop connection-setup
		 * packets. Corrupting a SYN yields a fatal INVALID_SYN NACK
		 * that tears down the PDC, which defeats recoverable
		 * fault-injection tests.
		 */
		val = toml_seek(result.toptab, "mangle.skip_syn");
		imp_state.mangle_skip_syn = (val.type == TOML_BOOLEAN) ?
					    val.u.boolean : true;

		val = toml_seek(result.toptab, "mangle.pkt_type");
		imp_state.mangle_pkt_type = imp_mangle_pkt_type_from_str(
			(val.type == TOML_STRING) ? val.u.s : NULL);

		val = toml_seek(result.toptab, "mangle.value");
		imp_state.mangle_value = (val.type == TOML_INT64) ?
					 (uint32_t)val.u.int64 : 0;

		/* field is only required for the mangle (not drop) action */
		if ((imp_state.mangle_action == IMP_MANGLE_ACTION_MANGLE) &&
		    (imp_state.mangle_field == IMP_MANGLE_FIELD_NONE)) {
			UET_IMP_ERR("config: mangle.field invalid; "
				    "disabling mangle hook");
			imp_state.mangle_enable = false;
		}

		if (imp_state.mangle_pkt_type == IMP_MANGLE_PKT_TYPE_INVALID) {
			UET_IMP_ERR("config: mangle.pkt_type invalid; "
				    "disabling mangle hook");
			imp_state.mangle_enable = false;
		}
	}

	toml_free(result);

	UET_IMP_INFO("config: num_planes=%d drop_rate=%d delay_max=%lu "
		     "random_enq=%s",
		     imp_state.num_planes, imp_state.drop_rate,
		     (unsigned long)imp_state.delay_max,
		     imp_state.random_enq ? "true" : "false");

	if (imp_state.mangle_enable)
		UET_IMP_INFO("config: mangle enabled rate=%d action=%s field=%s "
			     "pkt_type=%s flags=0x%02x skip_syn=%d value=%u",
			     imp_state.mangle_rate,
			     imp_mangle_action_str(imp_state.mangle_action),
			     imp_mangle_field_str(imp_state.mangle_field),
			     imp_mangle_pkt_type_str(imp_state.mangle_pkt_type),
			     imp_state.mangle_flags,
			     imp_state.mangle_skip_syn,
			     imp_state.mangle_value);

	return 0;
}

int imp_shim_init(struct uet_nic *nic)
{
	const char *config_path;
	int i, rc;

	memset(&imp_state, 0, sizeof(imp_state));
	imp_state.enabled = false;

	config_path = getenv(UET_IMPAIRMENT_SHIM);
	if (config_path == NULL)
		return 0;

	UET_IMP_INFO("initializing with config '%s'", config_path);

	/* seed random number generator */
	srand(time(NULL) ^ (uintptr_t)nic);

	/* read the config file */
	rc = imp_read_config(config_path);
	if (rc != 0)
		return rc;

	/* initialize the packet buffer pool */
	rc = imp_pool_init(&imp_state.pool, nic->max_pkt_size,
			   imp_state.num_planes);
	if (rc != 0)
		return rc;

	/* allocate the per-plane Tx queues */
	imp_state.planes = calloc(imp_state.num_planes,
				  sizeof(struct imp_plane));
	if (imp_state.planes == NULL) {
		UET_IMP_ERR("failed to allocate %d planes",
			    imp_state.num_planes);
		imp_pool_finalize(&imp_state.pool);
		return -ENOMEM;
	}

	for (i = 0; i < imp_state.num_planes; i++) {
		slist_init(&imp_state.planes[i].list);
		pthread_mutex_init(&imp_state.planes[i].lock, NULL);
	}

	imp_state.enabled = true;
	imp_state.nic = nic;

	/* spawn the Tx thread */
	imp_state.running = true;
	rc = pthread_create(&imp_state.tx_thread, NULL, imp_tx_thread,
			    &imp_state);
	if (rc != 0) {
		UET_IMP_ERR("failed to create Tx thread: %s", strerror(rc));
		free(imp_state.planes);
		imp_state.planes = NULL;
		imp_pool_finalize(&imp_state.pool);
		imp_state.enabled = false;
		return -rc;
	}

	UET_IMP_INFO("initialized successfully");
	return 0;
}

void imp_shim_finalize(void)
{
	struct imp_pkt *pkt;
	int i;

	if (!imp_state.enabled)
		return;

	UET_IMP_INFO("finalizing");

	/* stop the Tx thread */
	imp_state.running = false;
	pthread_join(imp_state.tx_thread, NULL);

	/* drain all the per-plane Tx queues back to the pool */
	for (i = 0; i < imp_state.num_planes; i++) {
		while (!slist_empty(&imp_state.planes[i].list)) {
			slist_remove_head_container(&imp_state.planes[i].list,
						    struct imp_pkt, pkt,
						    entry);
			imp_pool_put(&imp_state.pool, pkt);
		}

		pthread_mutex_destroy(&imp_state.planes[i].lock);
	}

	/* free the per-plane Tx queues */
	free(imp_state.planes);
	imp_state.planes = NULL;

	/* free all pool entries */
	UET_IMP_INFO("pool: %u total entries allocated", imp_state.pool.total);
	imp_pool_finalize(&imp_state.pool);

	imp_state.enabled = false;

	UET_IMP_INFO("finalized");
}

/*
 * Field mutation for a request packet (uet_pds_req layout). The selected
 * field is replaced outright with the configured value. Returns true if a
 * field was mutated (and the CRC must be recomputed).
 */
static bool imp_mangle_field_req(struct uet_pds_prlg *prlg, uint8_t *pds)
{
	struct uet_pds_req *req = (struct uet_pds_req *)pds;
	uint16_t spdcid = ntohs(req->spdcid);
	uint16_t tnf;
	uint8_t flags, old_flags;

	switch (imp_state.mangle_field) {
	case IMP_MANGLE_FIELD_PSN:
		UET_IMP_INFO("mangle: PDC %u req psn %u -> %u",
			     spdcid, ntohl(req->psn), imp_state.mangle_value);
		req->psn = htonl(imp_state.mangle_value);
		break;
	case IMP_MANGLE_FIELD_SPDCID:
		UET_IMP_INFO("mangle: PDC %u req spdcid %u -> %u",
			     spdcid, spdcid,
			     (uint16_t)imp_state.mangle_value);
		req->spdcid = htons((uint16_t)imp_state.mangle_value);
		break;
	case IMP_MANGLE_FIELD_DPDCID:
		UET_IMP_INFO("mangle: PDC %u req dpdcid %u -> %u",
			     spdcid, ntohs(req->dpdcid),
			     (uint16_t)imp_state.mangle_value);
		req->dpdcid = htons((uint16_t)imp_state.mangle_value);
		break;
	case IMP_MANGLE_FIELD_SYN:
		tnf = ntohs(prlg->type_next_flags);
		flags = (tnf & UET_PDS_FLAGS_MASK);
		old_flags = flags;
		if (imp_state.mangle_value) /* non-zero sets, zero clears */
			flags |= UET_PDS_REQ_FLAGS_SYN;
		else
			flags &= ~UET_PDS_REQ_FLAGS_SYN;
		tnf = ((tnf & ~UET_PDS_FLAGS_MASK) | flags);
		prlg->type_next_flags = htons(tnf);
		UET_IMP_INFO("mangle: PDC %u req SYN flags 0x%02x -> 0x%02x",
			     spdcid, old_flags, flags);
		break;
	default:
		return false;
	}

	return true;
}

/*
 * TODO: reserved for future ACK field mutation (uet_pds_ack layout, e.g.
 * cack_psn / spdcid / dpdcid). Drop (action="drop") already works for ACK.
 */
static bool imp_mangle_field_ack(struct uet_pds_prlg *prlg, uint8_t *pds)
{
	(void)prlg;
	(void)pds;
	UET_IMP_WARN("mangle: field mutation for ack not yet implemented");
	return false;
}

/*
 * TODO: reserved for future NACK field mutation (uet_pds_nack layout, e.g.
 * nack_code / nack_psn / spdcid / dpdcid). Drop already works for NACK.
 */
static bool imp_mangle_field_nack(struct uet_pds_prlg *prlg, uint8_t *pds)
{
	(void)prlg;
	(void)pds;
	UET_IMP_WARN("mangle: field mutation for nack not yet implemented");
	return false;
}

/*
 * TODO: reserved for future CTRL field mutation (uet_pds_ctrl layout, e.g.
 * psn / spdcid / dpdcid / SYN). Drop already works for CTRL.
 */
static bool imp_mangle_field_ctrl(struct uet_pds_prlg *prlg, uint8_t *pds)
{
	(void)prlg;
	(void)pds;
	UET_IMP_WARN("mangle: field mutation for ctrl not yet implemented");
	return false;
}

/*
 * Dispatch the field mutation to the per-type handler. Only request types are
 * implemented today; ack/nack/ctrl are reserved (see the stubs above) and the
 * field mutation is skipped for them, while the drop action already works for
 * any packet type.
 */
static bool imp_mangle_field_apply(uint8_t type, struct uet_pds_prlg *prlg,
				   uint8_t *pds)
{
	switch (type) {
	case UET_PDS_TYPE_RUD_REQ:
	case UET_PDS_TYPE_ROD_REQ:
	case UET_PDS_TYPE_RUDI_REQ:
	case UET_PDS_TYPE_UUD_REQ:
	case UET_PDS_TYPE_RUD_CC_REQ:
	case UET_PDS_TYPE_ROD_CC_REQ:
		return imp_mangle_field_req(prlg, pds);
	case UET_PDS_TYPE_ACK:
	case UET_PDS_TYPE_ACK_CC:
	case UET_PDS_TYPE_ACK_CCX:
		return imp_mangle_field_ack(prlg, pds);
	case UET_PDS_TYPE_NACK:
	case UET_PDS_TYPE_NACK_CCX:
		return imp_mangle_field_nack(prlg, pds);
	case UET_PDS_TYPE_CTRL:
		return imp_mangle_field_ctrl(prlg, pds);
	default:
		return false;
	}
}

/*
 * Test hook: intercept an outgoing UET packet matching the configured
 * pkt_type/flags filter and apply the configured action:
 *
 *   - "mangle": mutate a PDS header field and recompute the packet CRC so the
 *     peer still accepts the packet, validates the corrupted field, and emits
 *     a NACK back to this (initiator) node.
 *   - "drop": discard the packet (the caller frees the transmit copy). Since
 *     only the transmit copy is affected, the retained packet is left intact
 *     and the initiator RTO-retransmits.
 *
 * Both exercise the initiator NACK/retransmit processing, otherwise
 * unreachable on the normal data path.
 *
 * Returns true if the caller must drop the packet, false otherwise (either
 * not eligible, or mutated in place and still to be transmitted).
 *
 * The target packet type is selected by mangle.pkt_type: "any" (default)
 * targets every packet type, while a specific type targets exactly that type.
 * For the mangle action, field mutation is only implemented for request types
 * (ack/nack/ctrl are reserved and skipped); the drop action works for any
 * type.
 *
 * Constraints:
 *   - Only works with security DISABLED. Secured packets are encrypted with
 *     an ICV and cannot be meaningfully modified here.
 *   - For "mangle", the CRC MUST be recomputed, otherwise the peer silently
 *     drops the packet on CRC mismatch and never sends a NACK.
 *
 * Operates on the transmit copy only, so the caller's retained packet (used
 * for retransmission) is left intact and recovery can still succeed.
 */
static bool imp_shim_mangle_pkt(uint8_t *pkt, size_t iphdr_off,
				size_t pkt_size)
{
	uint8_t *ip = pkt + iphdr_off;
	uint8_t *pds, *crc_start;
	size_t ip_hdr_len, crc_off;
	struct uet_pds_prlg *prlg;
	uint8_t type, ip_ver;
	uint8_t flags;
	uint32_t crc;

	if (!imp_state.mangle_enable || imp_state.mangle_rate <= 0)
		return false;

	if ((rand() % 10000) >= imp_state.mangle_rate)
		return false;

	/* locate the IP header and determine its version / length */
	ip_ver = (ip[0] >> 4);
	if (ip_ver == 4)
		ip_hdr_len = ((ip[0] & 0x0f) * 4);
	else if (ip_ver == 6)
		ip_hdr_len = sizeof(struct ipv6hdr);
	else
		return false;

	/* the PDS header follows the IP and the entropy or UDP header */
	pds = ip + ip_hdr_len + uet_ip_encap_len(ip, (ip_ver == 6));
	if ((size_t)((pds - pkt) + sizeof(struct uet_pds_req)) > pkt_size)
		return false;

	/* decode the PDS packet type from the prologue */
	prlg = (struct uet_pds_prlg *)pds;
	type = (ntohs(prlg->type_next_flags) & UET_PDS_TYPE_MASK) >>
	       UET_PDS_TYPE_SHIFT;

	/*
	 * Decide eligibility by packet type: a specific mangle.pkt_type targets
	 * exactly that type, while "any" (default) targets every packet type.
	 */
	if ((imp_state.mangle_pkt_type != IMP_MANGLE_PKT_TYPE_ANY) &&
	    (type != (uint8_t)imp_state.mangle_pkt_type))
		return false;

	/*
	 * Apply the prologue flags filter: the packet is eligible only if all
	 * the configured flag bits are set. 0 means no flag constraint. Flag
	 * bits are per packet type (see uet_pkt_hdr.h); e.g. for requests
	 * SYN=0x04, AR=0x08, RETX=0x10.
	 */
	flags = (ntohs(prlg->type_next_flags) & UET_PDS_FLAGS_MASK);
	if ((flags & imp_state.mangle_flags) != imp_state.mangle_flags)
		return false;

	/*
	 * Never touch connection-setup packets when skip_syn is set: mangling
	 * or dropping a SYN yields a fatal INVALID_SYN NACK at the peer and
	 * closes the PDC, which is almost never the intent of a recoverable
	 * fault-injection test. The SYN flag bit (0x04) is common to request
	 * and control prologues.
	 */
	if (imp_state.mangle_skip_syn && (flags & UET_PDS_REQ_FLAGS_SYN)) {
		UET_IMP_DBG("mangle: skipping SYN %s packet",
			    imp_mangle_pkt_type_str(type));
		return false;
	}

	/* drop action: tell the caller to discard the matched packet */
	if (imp_state.mangle_action == IMP_MANGLE_ACTION_DROP) {
		UET_IMP_INFO("mangle: dropped Tx %s packet (%zu bytes)",
			     imp_mangle_pkt_type_str(type), pkt_size);
		return true;
	}

	/*
	 * mangle action: mutate the configured field using the per-type
	 * handler. If nothing was mutated (unsupported type/field), leave the
	 * packet unchanged and let it transmit as-is.
	 */
	if (!imp_mangle_field_apply(type, prlg, pds))
		return false;

	/*
	 * Recompute the CRC over the same range PDS uses on transmit: from the
	 * IP source address through the end of the packet, excluding the 4-byte
	 * CRC trailer itself.
	 */
	if (pkt_size < CRC_LEN)
		return false;
	crc_start = (ip_ver == 6) ? (ip + 8) : (ip + 12);
	crc_off = (pkt_size - CRC_LEN);
	crc = crc32c(crc_start, (size_t)((pkt + crc_off) - crc_start));
	memcpy((pkt + crc_off), &crc, CRC_LEN);

	UET_IMP_INFO("mangled %s on Tx %s packet (CRC recomputed)",
		     imp_mangle_field_str(imp_state.mangle_field),
		     imp_mangle_pkt_type_str(type));

	return false;
}

int imp_shim_tx_pkt(struct uet_nic *nic,
		    void *pkt,
		    void *iphdr,
		    size_t pkt_size)
{
	struct imp_pkt *imp_pkt;
	struct timespec now;
	uint64_t delay_ns;
	uint32_t plane_idx;

	/* random drop check */
	if ((imp_state.drop_rate > 0) &&
	    ((rand() % 10000) < imp_state.drop_rate)) {
		UET_IMP_INFO("dropped packet (%zu bytes)", pkt_size);
		return 0;
	}

	/* get a packet buffer from the pool */
	imp_pkt = imp_pool_get(&imp_state.pool);
	if (imp_pkt == NULL) {
		UET_IMP_ERR("failed to get packet buffer from pool");
		return -ENOMEM;
	}

	memcpy(imp_pkt->pkt_data, pkt, pkt_size);
	imp_pkt->pkt_size  = pkt_size;
	imp_pkt->iphdr_off = (size_t)((uint8_t *)iphdr - (uint8_t *)pkt);

	/*
	 * Optionally mangle the copy to induce a peer NACK, or drop it outright
	 * (return the buffer to the pool and skip transmit).
	 */
	if (imp_shim_mangle_pkt(imp_pkt->pkt_data, imp_pkt->iphdr_off,
				pkt_size)) {
		imp_pool_put(&imp_state.pool, imp_pkt);
		return 0;
	}

	/* calculate random delay and transmit time */
	clock_gettime(CLOCK_MONOTONIC, &now);

	delay_ns = (imp_state.delay_max > 0)
			? ((uint64_t)rand() % imp_state.delay_max) : 0;
	UET_IMP_DBG("delay packet (%lu)", delay_ns);

	/*
	 * Add the random delay to the current time to get the transmit time.
	 * Since tv_nsec is the sum of two sub-second values (each up to
	 * 999999999), it can overflow into the next second. If so, carry the
	 * extra second into tv_sec and normalize tv_nsec.
	 */
#define ONE_SEC 1000000000ULL
	imp_pkt->tx_time.tv_sec  = (now.tv_sec + (delay_ns / ONE_SEC));
	imp_pkt->tx_time.tv_nsec = (now.tv_nsec + (delay_ns % ONE_SEC));
	if (imp_pkt->tx_time.tv_nsec >= ONE_SEC) {
		imp_pkt->tx_time.tv_sec++;
		imp_pkt->tx_time.tv_nsec -= ONE_SEC;
	}

	/* select a plane for packet enqueue */
	if (imp_state.random_enq)
		plane_idx = (rand() % imp_state.num_planes);
	else
		plane_idx = (imp_state.enq_idx++ % imp_state.num_planes);

	imp_plane_enqueue(&imp_state.planes[plane_idx], imp_pkt);

	return 0;
}

bool imp_shim_is_enabled(void)
{
	return imp_state.enabled;
}

