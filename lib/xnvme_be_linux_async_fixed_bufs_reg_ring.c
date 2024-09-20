// SPDX-FileCopyrightText: Samsung Electronics Co., Ltd
//
// SPDX-License-Identifier: BSD-3-Clause

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <libxnvme.h>
#include <xnvme_be.h>
#include <xnvme_be_nosys.h>
#ifdef XNVME_BE_LINUX_LIBURING_ENABLED
#include <pthread.h>
#include <errno.h>
#include <liburing.h>
#include <xnvme_queue.h>
#include <xnvme_dev.h>
#include <xnvme_be_linux_liburing.h>
#include <xnvme_be_linux.h>
#include <xnvme_be_linux_nvme.h>
#include <linux/nvme_ioctl.h>

static int g_linux_liburing_optional[] = {
	IORING_OP_URING_CMD,
};
static int g_linux_liburing_noptional =
	sizeof g_linux_liburing_optional / sizeof(*g_linux_liburing_optional);

static struct sqpoll_wq {
	pthread_mutex_t mutex;
	struct io_uring ring;
	bool is_initialized;
	int refcount;
} g_sqpoll_wq = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.ring = {0},
	.is_initialized = false,
	.refcount = 0,
};

static int
_linux_liburing_noptional_missing(void)
{
	struct io_uring_probe *probe;
	int missing = 0;

	probe = io_uring_get_probe();
	if (!probe) {
		XNVME_DEBUG("FAILED: io_uring_get_probe()");
		return -ENOSYS;
	}

	for (int i = 0; i < g_linux_liburing_noptional; ++i) {
		if (!io_uring_opcode_supported(probe, g_linux_liburing_optional[i])) {
			missing += 1;
		}
	}

	io_uring_free_probe(probe);

	return missing;
}

static int
_init_retry(unsigned entries, struct io_uring *ring, struct io_uring_params *p)
{
	int err;

retry:
	err = io_uring_queue_init_params(entries, ring, p);
	if (err) {
		if (err == -EINVAL && p->flags & IORING_SETUP_SINGLE_ISSUER) {
			p->flags &= ~IORING_SETUP_SINGLE_ISSUER;
			XNVME_DEBUG("FAILED: io_uring_queue_init_params(), retry(!SINGLE_ISSUER)");
			goto retry;
		}

		XNVME_DEBUG("FAILED: io_uring_queue_init(), err: %d", err);
		return err;
	}

	return 0;
}

int
xnvme_be_linux_fixed_bufs_reg_ring_init(struct xnvme_queue *q, int opts)
{
	struct xnvme_queue_liburing *queue = (void *)q;
	struct xnvme_be_linux_state *state = (void *)queue->base.dev->be.state;
	struct io_uring_params ring_params = {0};
	int err = 0;

	if (_linux_liburing_noptional_missing()) {
		fprintf(stderr, "# FAILED: io_uring cmd, not supported by kernel!\n");
		return -ENOSYS;
	}

	opts |= XNVME_QUEUE_IOU_BIGSQE;


	if ((opts & XNVME_QUEUE_SQPOLL) || (state->poll_sq)) {
		queue->poll_sq = 1;
	}
	if ((opts & XNVME_QUEUE_IOPOLL) || (state->poll_io)) {
		queue->poll_io = 1;
	}

	XNVME_DEBUG("queue->poll_sq: %d", queue->poll_sq);
	XNVME_DEBUG("queue->poll_io: %d", queue->poll_io);

	err = pthread_mutex_lock(&g_sqpoll_wq.mutex);
	if (err) {
		XNVME_DEBUG("FAILED: lock(g_sqpoll_wq.mutex), err: %d", err);
		return -err;
	}

	queue->batching = 1;
	if (getenv("XNVME_QUEUE_BATCHING_OFF")) {
		queue->batching = 0;
	}

	//
	// Ring-initialization
	//
	if (queue->poll_sq) {
		char *env;

		if (!((env = getenv("XNVME_QUEUE_SQPOLL_AWQ")) && atoi(env) == 0)) {
			if (!g_sqpoll_wq.is_initialized) {
				struct io_uring_params sqpoll_wq_params = {0};

				env = getenv("XNVME_QUEUE_SQPOLL_CPU");
				if (env) {
					sqpoll_wq_params.flags |= IORING_SETUP_SQ_AFF;
					sqpoll_wq_params.sq_thread_cpu = atoi(env);
				}
				sqpoll_wq_params.flags |= IORING_SETUP_SQPOLL;
				sqpoll_wq_params.flags |= IORING_SETUP_SINGLE_ISSUER;

				err = _init_retry(queue->base.capacity, &g_sqpoll_wq.ring,
						  &sqpoll_wq_params);
				if (err) {
					XNVME_DEBUG(
						"FAILED: "
						"io_uring_queue_init_params(g_sqpoll_wq.ring), "
						"err: %d",
						err);
					goto exit;
				}

				g_sqpoll_wq.is_initialized = true;
			}

			g_sqpoll_wq.refcount += 1;
			ring_params.wq_fd = g_sqpoll_wq.ring.ring_fd;
			ring_params.flags |= IORING_SETUP_ATTACH_WQ;
		}
		ring_params.flags |= IORING_SETUP_SQPOLL;
		ring_params.flags |= IORING_SETUP_SINGLE_ISSUER;
	}
	if (queue->poll_io) {
		ring_params.flags |= IORING_SETUP_IOPOLL;
	}

	if (opts & XNVME_QUEUE_IOU_BIGSQE) {
		ring_params.flags |= IORING_SETUP_SQE128;
		ring_params.flags |= IORING_SETUP_CQE32;
	}

	err = _init_retry(queue->base.capacity, &queue->ring, &ring_params);
	if (err) {
		XNVME_DEBUG("FAILED: _init_retry, err: %d", err);
		goto exit;
	}

	if (queue->poll_sq) {
		err = io_uring_register_files(&queue->ring, &(state->fd), 1);
		if (err) {
			XNVME_DEBUG("FAILED: io_uring_register_files, err: %d", err);
			goto exit;
		}
	}

	// Returns 1 on success
	err = io_uring_register_ring_fd(&queue->ring);
	if (err != 1) {
		XNVME_DEBUG("FAILED: io_uring_register_ring_fd, err: %d", err);
		goto exit;
	}
	err = 0;

exit:
	if (err && queue->poll_sq && g_sqpoll_wq.is_initialized && (!(--g_sqpoll_wq.refcount))) {
		io_uring_queue_exit(&g_sqpoll_wq.ring);
		g_sqpoll_wq.is_initialized = false;
	}
	if (pthread_mutex_unlock(&g_sqpoll_wq.mutex)) {
		XNVME_DEBUG("FAILED: unlock(g_sqpoll_wq.mutex)");
	}

	return err;
}


int
xnvme_be_linux_fixed_bufs_reg_ring_term(struct xnvme_queue *q)
{
	struct xnvme_queue_liburing *queue = (void *)q;
	int err;

	err = pthread_mutex_lock(&g_sqpoll_wq.mutex);
	if (err) {
		XNVME_DEBUG("FAILED: lock(g_sqpoll_wq.mutex), err: %d", err);
		return -err;
	}

	if (!queue) {
		XNVME_DEBUG("FAILED: queue: %p", (void *)queue);
		err = -EINVAL;
		goto exit;
	}
	if (queue->poll_sq) {
		io_uring_unregister_files(&queue->ring);
	}

	io_uring_unregister_ring_fd(&queue->ring);
	
	io_uring_queue_exit(&queue->ring);

	if (queue->poll_sq && g_sqpoll_wq.is_initialized && (!(--g_sqpoll_wq.refcount))) {
		io_uring_queue_exit(&g_sqpoll_wq.ring);
		g_sqpoll_wq.is_initialized = false;
	}

exit:
	if (pthread_mutex_unlock(&g_sqpoll_wq.mutex)) {
		XNVME_DEBUG("FAILED: unlock(g_sqpoll_wq.mutex)");
	}

	return err;
}

#ifdef NVME_URING_CMD_IO
int
xnvme_be_linux_fixed_bufs_reg_ring_poke(struct xnvme_queue *q, uint32_t max)
{
	struct xnvme_queue_liburing *queue = (void *)q;
	struct io_uring_cqe *cqe;
	struct xnvme_cmd_ctx *ctx;
	unsigned completed;
	int err;

	max = max ? max : queue->base.outstanding;
	max = max > queue->base.outstanding ? queue->base.outstanding : max;

	if (queue->batching) {
		int err = io_uring_submit(&queue->ring);
		if (err < 0) {
			XNVME_DEBUG("io_uring_submit, err: %d", err);
			return err;
		}
	}

	completed = 0;
	for (uint32_t i = 0; i < max; i++) {
		err = io_uring_peek_cqe(&queue->ring, &cqe);
		if (err == -EAGAIN) {
			return completed;
		}

		ctx = io_uring_cqe_get_data(cqe);

#ifdef XNVME_DEBUG_ENABLED
		if (!ctx) {
			XNVME_DEBUG("-{[THIS SHOULD NOT HAPPEN]}-");
			XNVME_DEBUG("cqe->user_data is NULL! => NO REQ!");
			XNVME_DEBUG("cqe->res: %d", cqe->res);
			XNVME_DEBUG("cqe->flags: %u", cqe->flags);
			return -EIO;
		}
#endif

		ctx->cpl.result = cqe->big_cqe[0];

		/** IO64-quirky-handling: this is also for NVME_URING_CMD_IO_VEC */
		err = xnvme_be_linux_nvme_map_cpl(ctx, NVME_URING_CMD_IO, cqe->res);
		if (err) {
			XNVME_DEBUG("FAILED: xnvme_be_linux_nvme_map_cpl(), err: %d", err);
			return err;
		}

		queue->base.outstanding--;

		io_uring_cqe_seen(&queue->ring, cqe);

		ctx->async.cb(ctx, ctx->async.cb_arg);

		completed++;
	}

	return completed;
}

#else
int
xnvme_be_linux_fixed_bufs_reg_ring_poke(struct xnvme_queue *q, uint32_t max)
{
	XNVME_DEBUG("FAILED: not supported, built on system without NVME_URING_CMD_IO");
	return xnvme_be_nosys_queue_poke(q, max);
}
#endif

#ifdef NVME_URING_CMD_IO
int
xnvme_be_linux_fixed_bufs_reg_ring_io(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *XNVME_UNUSED(mbuf),
		       size_t mbuf_nbytes)
{
	struct xnvme_queue_liburing *queue = (void *)ctx->async.queue;
	struct xnvme_be_linux_state *state = (void *)queue->base.dev->be.state;
	struct io_uring_sqe *sqe = NULL;
	int err = 0;

	sqe = io_uring_get_sqe(&queue->ring);
	if (!sqe) {
		return -EAGAIN;
	}

	sqe->opcode = IORING_OP_URING_CMD;
	sqe->off = NVME_URING_CMD_IO;
	sqe->flags = queue->poll_sq ? IOSQE_FIXED_FILE : 0;
	// NOTE: we only ever register a single file, the raw device, so the
	// provided index will always be 0
	sqe->fd = queue->poll_sq ? 0 : state->fd;
	sqe->user_data = (unsigned long)ctx;

	ctx->cmd.common.dptr.lnx_ioctl.data = (uint64_t)dbuf;
	ctx->cmd.common.dptr.lnx_ioctl.data_len = dbuf_nbytes;

	// ctx->cmd.common.mptr = (uint64_t)mbuf;
	// ctx->cmd.common.dptr.lnx_ioctl.metadata_len = mbuf_nbytes;

	memcpy(&sqe->addr3, &ctx->cmd.common, 64);

	sqe->uring_cmd_flags = IORING_URING_CMD_FIXED;
	sqe->buf_index = mbuf_nbytes;
	
	if (queue->batching) {
		goto exit;
	}

	err = io_uring_submit(&queue->ring);
	if (err < 0) {
		XNVME_DEBUG("io_uring_submit(%d), err: %d", ctx->cmd.common.opcode, err);
		return err;
	}

exit:
	queue->base.outstanding += 1;

	return 0;
}
#else
int
xnvme_be_linux_fixed_bufs_reg_ring_io(struct xnvme_cmd_ctx *ctx, void *dbuf, size_t dbuf_nbytes, void *mbuf,
		       size_t mbuf_nbytes)
{
	XNVME_DEBUG("FAILED: not supported, built on system without NVME_URING_CMD_IO");
	return xnvme_be_nosys_queue_cmd_io(ctx, dbuf, dbuf_nbytes, mbuf, mbuf_nbytes);
}
#endif


#ifdef NVME_URING_CMD_IO_VEC
int
xnvme_be_linux_fixed_bufs_reg_ring_iov(struct xnvme_cmd_ctx *ctx, struct iovec *dvec, size_t dvec_cnt,
			size_t XNVME_UNUSED(dvec_nbytes), void *XNVME_UNUSED(mbuf), size_t XNVME_UNUSED(mbuf_nbytes))
{
	struct xnvme_queue_liburing *queue = (void *)ctx->async.queue;
	int err = 0;

	err = io_uring_register_buffers(&queue->ring, dvec, dvec_cnt);

	return err;
}
#else
int
xnvme_be_linux_fixed_bufs_reg_ring_iov(struct xnvme_cmd_ctx *ctx, struct iovec *dvec, size_t dvec_cnt,
			size_t dvec_nbytes, void *mbuf, size_t mbuf_nbytes)
{
	XNVME_DEBUG("FAILED: not supported, built on system without NVME_URING_CMD_IO_VEC");
	return xnvme_be_nosys_queue_cmd_iov(ctx, dvec, dvec_cnt, dvec_nbytes, mbuf, mbuf_nbytes);
}
#endif
#endif

struct xnvme_be_async g_xnvme_be_linux_async_fixed_bufs_reg_ring = {
	.id = "fixed_bufs_reg_ring",
#ifdef XNVME_BE_LINUX_LIBURING_ENABLED
	.cmd_io = xnvme_be_linux_fixed_bufs_reg_ring_io,
	.cmd_iov = xnvme_be_linux_fixed_bufs_reg_ring_iov,
	.poke = xnvme_be_linux_fixed_bufs_reg_ring_poke,
	.wait = xnvme_be_nosys_queue_wait,
	.init = xnvme_be_linux_fixed_bufs_reg_ring_init,
	.term = xnvme_be_linux_fixed_bufs_reg_ring_term,
	.get_completion_fd = xnvme_be_nosys_queue_get_completion_fd,
#else
	.cmd_io = xnvme_be_nosys_queue_cmd_io,
	.cmd_iov = xnvme_be_nosys_queue_cmd_iov,
	.poke = xnvme_be_nosys_queue_poke,
	.wait = xnvme_be_nosys_queue_wait,
	.init = xnvme_be_nosys_queue_init,
	.term = xnvme_be_nosys_queue_term,
	.get_completion_fd = xnvme_be_nosys_queue_get_completion_fd,
#endif
};
