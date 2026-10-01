/*
 * Subset of the UET SES API (../uet_api.h) used by the libfabric glue.
 *
 * uet_api.h includes libfabric internals (ofi_mem.h) from the vendored
 * 1.20 headers, so it cannot be included by code built against the
 * installed libfabric. These declarations repeat the signatures of the
 * ENABLE_VERBS=0 build. core_check.c includes both headers in one
 * translation unit so the compiler rejects any drift.
 *
 * The libfabric structures passed through this interface (fi_info and its
 * attributes, fi_cq_attr, fi_cq_err_entry) have the same layout in the
 * vendored and the installed headers, apart from members appended to
 * fi_domain_attr and fi_mr_attr that the core does not read.
 */

#ifndef _UET_CORE_H_
#define _UET_CORE_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_eq.h>

#include "../uet_addr.h"

#define UET_DEF_JOB_ID			1
#define UET_FLAGS_NONE			0
#define UET_MR_KEY_NONE			((uint64_t) 0)
#define UET_MR_KEY_IDEMPOTENT_SAFE	0x8000000000000000ULL

typedef void *uet_handle_t;
typedef void *uet_domain_handle_t;
typedef void *uet_ep_handle_t;
typedef void *uet_cq_handle_t;
typedef void *uet_addr_handle_t;
typedef void *uet_mr_handle_t;

typedef void (*uet_eq_callback_t)(uet_handle_t handle,
				  struct fi_eq_entry *eq_entry);
typedef void (*uet_eq_err_callback_t)(uet_handle_t handle,
				      struct fi_eq_err_entry *eq_err_entry);

int uet_initialize(uet_handle_t *handle);
int uet_finalize(uet_handle_t handle);

int uet_domain(uet_handle_t handle, struct fid_fabric *fabric,
	       struct fi_info *info, struct fid_domain *domain,
	       void *context, uet_eq_callback_t eq_callback,
	       uet_eq_err_callback_t eq_err_callback,
	       uet_domain_handle_t *domain_handle);
int uet_domain_close(uet_domain_handle_t domain_handle);

int uet_mr_reg(uet_domain_handle_t domain_handle, const void *buf, size_t len,
	       uint64_t access, uint64_t requested_key, uint64_t flags,
	       void *context, uet_mr_handle_t *mr_handle);
uint64_t uet_mr_key(uet_mr_handle_t mr_handle);
int uet_ep_bind_mr(uet_ep_handle_t ep_handle,
		   uet_mr_handle_t mr_handle, uint64_t flags);
int uet_mr_enable(uet_mr_handle_t mr_handle);
int uet_mr_disable(uet_mr_handle_t mr_handle);
int uet_mr_close(uet_mr_handle_t mr_handle);

int uet_endpoint(uet_domain_handle_t domain_handle,
		 struct fi_info *info, struct fid_ep *ep,
		 void *context, uet_ep_handle_t *ep_handle);
int uet_getname(uet_ep_handle_t ep_handle, struct uet_addr *uet_addr);
int uet_ep_bind_cq(uet_ep_handle_t ep_handle, struct fi_cq_attr *attr,
		   struct fid_cq *cq, uint64_t flags, void *context,
		   uet_cq_handle_t *cq_handle);
int uet_ep_enable(uet_ep_handle_t ep_handle);
int uet_ep_close(uet_ep_handle_t ep_handle);
int uet_ep_progress(uet_ep_handle_t ep_handle);

ssize_t uet_cq_read(uet_cq_handle_t cq_handle, void *buf, size_t count);
ssize_t uet_cq_readerr(uet_cq_handle_t cq_handle,
		       struct fi_cq_err_entry *buf);

int uet_av_insert(uet_domain_handle_t domain_handle,
		  struct uet_addr *uet_addr,
		  uet_addr_handle_t *addr_handle);
int uet_av_remove(uet_addr_handle_t addr_handle);

ssize_t uet_write(uet_ep_handle_t ep_handle, uint32_t job_id,
		  void *buf, size_t len, uint64_t *data,
		  uet_mr_handle_t mr_handle,
		  uet_addr_handle_t dst_addr_handle,
		  uint64_t remote_mem_addr, uint64_t remote_key,
		  void *context);

#endif /* _UET_CORE_H_ */
