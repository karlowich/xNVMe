/*
 * fio xNVMe IO Engine
 *
 * IO engine using the asynchronous interface of the xNVMe C API.
 *
 * See: http://xnvme.io/
 *
 * -----------------------------------------------------------------------------
 *
 * Notes on the implementation and fio IO engines in general
 * =========================================================
 *
 * Built-in engine interface:
 *
 * - static void fio_init xnvme_fioe_register(void)
 * - static void fio_exit xnvme_fioe_unregister(void)
 * - static struct ioengine_ops ioengine
 * - Usage: '--ioengine=myengine'
 *
 * External engine interface:
 *
 * - struct ioengine_ops ioengine
 * - Usage: '--ioengine=external:/path/to/myengine.so'
 *
 * When writing an external engine you actually have two choices, you can:
 *
 * 1) Follow the "External engine interface" as described above
 * 2) Fake an internal engine
 *    - Implement the "Built-in engine interface"
 *    - Inject the engine via LD_PRELOAD=/path/to/myengine.so
 *    - NOTE: by injecting you are potentially overwriting more symbols than
 *      just those required by the "Built-in engine interface"
 *
 * It seems like the "cleanest" approach is to implement en engine following the
 * "External engine interface", however, there is some spurious behavior/race
 * causing a segfault when accessing `td->io_ops` in `_queue()`.
 *
 * However, for some reason, this segfault does not occur if `td->io_ops` is
 * touched during `_init()` which is why `_init()` echoes the value of
 * `td->io_ops`.
 *
 * CAVEAT: Multi-device support
 *
 * Support is here, however, there is one limiting caveat, and two others noted
 * in case issues should arise.
 *
 * - 1) iomem_{alloc/free} introduces a limitation with regards to multiple
 *   devices. Specifically, the devices opened must use backends which share
 *   memory allocators. E.g. using be:laio + be:liou is fine, using be:liou +
 *   be:spdk is not.
 *   This is due to the fio 'io_memem_*' helpers are not tied to devices, as
 *   such, it is required that all devices opened use compatible
 *   buffer-allocators. Currently, the implementation does dot check for this
 *   unsupported use-case, and will thus lead to a runtime error.
 *
 * - 2) The implementation assumes that 'thread_data.o.nr_files' is available
 *   and that instances of 'fio_file.fileno' are valued [0,
 *   thread_data.o.nr_files -1].
 *   This is to pre-allocate file-wrapping-structures, xnvme_fioe_fwrap, at I/O
 *   engine initialization time and to reference file-wrapping with
 *   constant-time lookup
 *
 * - 3) The _open() and _close() functions do not implement the "real"
 *   device/file opening, this is done in _init() and torn down in _cleanup() as
 *   the io-engine needs device handles ready for iomem_{alloc/free}
 *
 * CAVEAT: Supporting NVMe devices formatted with extended-LBA
 *
 * To support extended-lba initial work has been done in xNVMe, however, further
 * work is probably need for this to trickle up from the fio I/O engine, also,
 * in the io-engine ">> ssw" are used which does not account for extended LBA.
 */
#include <stdlib.h>
#include <assert.h>
#include <libxnvme.h>
#include <libxnvme_3p.h>
#include <libxnvme_nvm.h>
#include <libxnvme_znd.h>
#include "fio.h"
#include "zbd_types.h"
#include "optgroup.h"

static pthread_mutex_t g_serialize = PTHREAD_MUTEX_INITIALIZER;

struct xnvme_fioe_fwrap {
	///< fio file representation
	struct fio_file *fio_file;

	///< xNVMe device handle
	struct xnvme_dev *dev;
	///< xNVMe device geometry
	const struct xnvme_geo *geo;

	struct xnvme_queue *queue;

	uint32_t ssw;
	uint32_t lba_nbytes;

	uint8_t _pad[24];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fioe_fwrap) == 64, "Incorrect size")

struct xnvme_fioe_data {
	///< I/O completion queue
	struct io_u **iocq;

	///< # of iocq entries; incremented via getevents()/cb_pool()
	uint64_t completed;

	///< # of errors; incremented when observed on completion via getevents()/cb_pool()
	uint64_t ecount;

	///< Controller which device/file to select
	int32_t prev;
	int32_t cur;

	///< Number of devices/files for which open() has been called
	int64_t nopen;
	///< Number of devices/files allocated in files[]
	uint64_t nallocated;

	uint8_t _pad[16];

	struct xnvme_fioe_fwrap files[];
};
XNVME_STATIC_ASSERT(sizeof(struct xnvme_fioe_data) == 64, "Incorrect size")

#ifndef log_err
//#ifdef WIN32
ssize_t log_err(const char* format, ...)
{
	char buffer[1024];
	va_list args;
	size_t len;

	va_start(args, format);
	len = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	len = min(len, sizeof(buffer) - 1);

	return fwrite(buffer, len, 1, stderr);
}

ssize_t log_info(const char* format, ...)
{
	char buffer[1024];
	va_list args;
	size_t len;

	va_start(args, format);
	len = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	len = min(len, sizeof(buffer) - 1);

	return fwrite(buffer, len, 1, stdout);
}
#endif

struct xnvme_fioe_options {
	void *padding;
	unsigned int hipri;
	unsigned int sqpoll_thread;
	unsigned int xnvme_dev_nsid;
	char *xnvme_async;
	char *xnvme_sync;
	char *xnvme_admin;
};

static struct fio_option options[] = {
	{
		.name   = "hipri",
		.lname  = "High Priority",
		.type   = FIO_OPT_STR_SET,
		.off1   = offsetof(struct xnvme_fioe_options, hipri),
		.help   = "Use polled IO completions",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_INVALID,
	},
	{
		.name   = "sqthread_poll",
		.lname  = "Kernel SQ thread polling",
		.type   = FIO_OPT_INT,
		.off1   = offsetof(struct xnvme_fioe_options, sqpoll_thread),
		.help   = "Offload submission/completion to kernel thread",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_INVALID,
	},

	{
		.name   = "xnvme_async",
		.lname  = "xNVMe Asynchronous command-interface",
		.type   = FIO_OPT_STR_STORE,
		.off1   = offsetof(struct xnvme_fioe_options, xnvme_async),
		.help   = "Select async. cmd-interface: [emu,thrpool,io_uring,libaio,posix,nil",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_INVALID,
	},
	{
		.name   = "xnvme_sync",
		.lname  = "xNVMe Synchronous. command-interface",
		.type   = FIO_OPT_STR_STORE,
		.off1   = offsetof(struct xnvme_fioe_options, xnvme_sync),
		.help   = "Select xNVMe synchronous cmd-interface: [nvme,psync]",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_INVALID,
	},
	{
		.name   = "xnvme_admin",
		.lname  = "xNVMe Admin command-interface",
		.type   = FIO_OPT_STR_STORE,
		.off1   = offsetof(struct xnvme_fioe_options, xnvme_sync),
		.help   = "Select xNVMe admin cmd-interface: {nvme,block,file_as_ns}",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_INVALID,
	},

	{
		.name   = "xnvme_dev_nsid",
		.lname  = "xNVMe Namespace-Identifier, for user-space NVMe driver",
		.type   = FIO_OPT_INT,
		.off1   = offsetof(struct xnvme_fioe_options, xnvme_dev_nsid),
		.help   = "xNVMe Namespace-Identifier, for user-space NVMe driver",
		.category = FIO_OPT_C_ENGINE,
		.group  = FIO_OPT_G_INVALID,
	},

	{
		.name   = NULL,
	},
};

static void
cb_pool(struct xnvme_cmd_ctx *ctx, void *cb_arg)
{
	struct io_u *io_u = cb_arg;
	struct xnvme_fioe_data *xd = io_u->engine_data;

	if (xnvme_cmd_ctx_cpl_status(ctx)) {
		xnvme_cmd_ctx_pr(ctx, XNVME_PR_DEF);
		xd->ecount += 1;
		io_u->error = EIO;
	}

	xd->iocq[xd->completed++] = io_u;
	xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
}

static struct xnvme_opts
xnvme_opts_from_fioe(struct thread_data *td)
{
	struct xnvme_fioe_options *o = td->eo;
	struct xnvme_opts opts = xnvme_opts_default();

	opts.nsid = o->xnvme_dev_nsid;
	opts.async = o->xnvme_async;
	opts.sync = o->xnvme_sync;
	opts.admin = o->xnvme_admin;

	opts.poll_io = o->hipri;
	opts.poll_sq = o->sqpoll_thread;

	opts.direct = td->o.odirect;

	return opts;
}

#ifdef XNVME_DEBUG_ENABLED
static void
_fio_file_pr(struct fio_file *f)
{
	if (!f) {
		log_info("fio_file: ~\n");
		return;
	}

	log_info("fio_file: { ");
	log_info("file_name: '%s', ", f->file_name);
	log_info("fileno: %d, ", f->fileno);
	log_info("io_size: %zu, ", f->io_size);
	log_info("real_file_size: %zu, ", f->real_file_size);
	log_info("file_offset: %zu", f->file_offset);
	log_info("}\n");
}
#endif

static int
_dev_close(struct thread_data *td, struct xnvme_fioe_fwrap *fwrap)
{
	if (fwrap->dev) {
		xnvme_queue_term(fwrap->queue);
	}
	xnvme_dev_close(fwrap->dev);

	memset(fwrap, 0, sizeof(*fwrap));

	return 0;
}

static void
xnvme_fioe_cleanup(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	int err;

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_lock(), err: %d", err);
		// NOTE: not returning here
	}

	for (uint64_t i = 0; i < xd->nallocated; ++i) {
		int err;

		err = _dev_close(td, &xd->files[i]);
		if (err) {
			XNVME_DEBUG("xnvme_fioe: cleanup(): Unexpected error");
		}
	}
	err = pthread_mutex_unlock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err: %d", err);
	}

	free(xd->iocq);
	free(xd);
	td->io_ops_data = NULL;
}

/**
 * Helper function setting up device handles as addressed by the naming
 * convention of the given `fio_file` filename.
 *
 * Checks thread-options for explicit control of asynchronous implementation via
 * the ``--async={thrpool,emu,posix,io_uring,libaio,nil}``.
 */
static int
_dev_open(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap;
	int flags = 0;
	int err;

	if (f->fileno > (int)xd->nallocated) {
		log_err("xnvme_fioe: _dev_open(); invalid assumption\n");
		return 1;
	}

	fwrap = &xd->files[f->fileno];

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_lock(), err: %d", err);
		return -err;
	}

	fwrap->dev = xnvme_dev_open(f->file_name, &opts);
	if (!fwrap->dev) {
		log_err("xnvme_fioe: init(): {f->file_name: '%s', err: '%s'}\n",
			f->file_name, strerror(errno));
		goto failure;
	}
	fwrap->geo = xnvme_dev_get_geo(fwrap->dev);

	if (xnvme_queue_init(fwrap->dev, td->o.iodepth, flags, &(fwrap->queue))) {
		log_err("xnvme_fioe: init(): failed xnvme_queue_init()\n");
		goto failure;
	}
	xnvme_queue_set_cb(fwrap->queue, cb_pool, NULL);

	fwrap->ssw = xnvme_dev_get_ssw(fwrap->dev);
	fwrap->lba_nbytes = fwrap->geo->lba_nbytes;

	fwrap->fio_file = f;
	fwrap->fio_file->filetype = FIO_TYPE_BLOCK;
	fwrap->fio_file->real_file_size = fwrap->geo->tbytes;
	fio_file_set_size_known(fwrap->fio_file);

	err = pthread_mutex_unlock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err: %d", err);
	}

	return 0;

failure:
	xnvme_queue_term(fwrap->queue);
	xnvme_dev_close(fwrap->dev);

	err = pthread_mutex_unlock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err: %d", err);
	}

	return 1;
}

static int
xnvme_fioe_init(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = NULL;
	struct fio_file *f;
	unsigned int i;

	log_err("xnvme_fioe: init(): td->io_ops: %p\n", td->io_ops);

	if (!td->o.use_thread) {
		log_err("xnvme_fioe: init(): --thread=1 is required\n");
		return 1;
	}
	if (!td->io_ops) {
		log_err("xnvme_fioe: init(): !td->io_ops\n");
		log_err("xnvme_fioe: init(): Check fio version\n");
#ifdef fio_version_string
		log_err("xnvme_fioe: init(): I/O engine running with: '%s'\n",
			fio_version_string);
#endif
		log_err("xnvme_fioe: init(): I/O engine built with:\n");
		xnvme_3p_ver_fpr(stderr, xnvme_3p_ver, XNVME_PR_DEF);
		return 1;
	}

	// Allocate and zero-fill xd
	xd = malloc(sizeof(*xd) + sizeof(*xd->files) * td->o.nr_files);
	if (!xd) {
		log_err("xnvme_fioe: init(): !malloc()\n");
		return 1;
	}
	memset(xd, 0, sizeof(*xd) + sizeof(*xd->files) * td->o.nr_files);

	xd->iocq = malloc(td->o.iodepth * sizeof(struct io_u *));
	memset(xd->iocq, 0, td->o.iodepth * sizeof(struct io_u *));

	xd->prev = -1;
	td->io_ops_data = xd;

	for_each_file(td, f, i) {
		if (_dev_open(td, f)) {
			log_err("xnvme_fioe: init(): _dev_open(%s)\n",
				f->file_name);
			return 1;
		}

		++(xd->nallocated);
	}

	if (xd->nallocated != td->o.nr_files) {
		log_err("xnvme_fioe: init(): nallocated != td->o.nr_files\n");
		return 1;
	}

	return 0;
}

// NOTE: using the first device for buffer-allocators, see CAVEAT 2)
static int
xnvme_fioe_iomem_alloc(struct thread_data *td, size_t total_mem)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[0];

	if (!fwrap->dev) {
		log_err("xnvme_fioe: failed iomem_alloc(); no dev-handle\n");
		return 1;
	}

	td->orig_buffer = xnvme_buf_alloc(fwrap->dev, total_mem);

	return td->orig_buffer == NULL;
}

// NOTE: using the first device for buffer-allocators, see CAVEAT 2)
static void
xnvme_fioe_iomem_free(struct thread_data *td)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = &xd->files[0];

	if (!fwrap->dev) {
		log_err("xnvme_fioe: failed iomem_free(); no dev-handle\n");
		return;
	}

	xnvme_buf_free(fwrap->dev, td->orig_buffer);
}

static int
xnvme_fioe_io_u_init(struct thread_data *td, struct io_u *io_u)
{
	io_u->engine_data = td->io_ops_data;

	return 0;
}

static void
xnvme_fioe_io_u_free(struct thread_data *td, struct io_u *io_u)
{
	io_u->engine_data = NULL;
}

static struct io_u *
xnvme_fioe_event(struct thread_data *td, int event)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	assert(event >= 0);
	assert((unsigned)event < xd->completed);

	return xd->iocq[event];
}

static int
xnvme_fioe_getevents(struct thread_data *td, unsigned int min,
		     unsigned int max, const struct timespec *t)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap = NULL;
	int nfiles = xd->nallocated;
	int err = 0;

	if (t) {
		XNVME_DEBUG("INFO: ignoring timespec");
	}

	if (xd->prev != -1 && ++xd->prev < nfiles) {
		fwrap = &xd->files[xd->prev];
		xd->cur = xd->prev;
	}

	xd->completed = 0;
	for (; ;) {
		if (fwrap == NULL || xd->cur == nfiles) {
			fwrap = &xd->files[0];
			xd->cur = 0;
		}

		while (fwrap != NULL && xd->cur < nfiles && err >= 0) {
			err = xnvme_queue_poke(fwrap->queue,
                                   max - xd->completed);
			if (err < 0) {
				switch (err) {
				case -EBUSY:
				case -EAGAIN:
					usleep(1);
					break;

				default:
					XNVME_DEBUG("Oh my");
					assert(false);
					return 0;
				}
			}
			if (xd->completed >= min) {
				xd->prev = xd->cur;
				return xd->completed;
			}
			xd->cur++;
			fwrap = &xd->files[xd->cur];

			if (err < 0) {
				switch (err) {
				case -EBUSY:
				case -EAGAIN:
					usleep(1);
					break;
				}
			}
		}
	}

	xd->cur = 0;

	return xd->completed;
}

static enum fio_q_status
xnvme_fioe_queue(struct thread_data *td, struct io_u *io_u)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;
	struct xnvme_fioe_fwrap *fwrap;
	struct xnvme_cmd_ctx *ctx;
	uint32_t nsid;
	uint64_t slba;
	uint16_t nlb;
	int err;

	fio_ro_check(td, io_u);

	fwrap = &xd->files[io_u->file->fileno];
	nsid = xnvme_dev_get_nsid(fwrap->dev);

	slba = io_u->offset >> fwrap->ssw;
	nlb = (io_u->xfer_buflen >> fwrap->ssw) - 1;

	if (td->io_ops->flags & FIO_SYNCIO) {
		log_err("xnvme_fioe: queue(): Got sync...\n");
		assert(false);
		return FIO_Q_COMPLETED;
	}

	ctx = xnvme_queue_get_cmd_ctx(fwrap->queue);
	ctx->async.cb_arg = io_u;

	switch (io_u->ddir) {
	case DDIR_READ:
		err = xnvme_nvm_read(ctx, nsid, slba, nlb, io_u->xfer_buf, NULL);
		break;

	case DDIR_WRITE:
		err = xnvme_nvm_write(ctx, nsid, slba, nlb, io_u->xfer_buf, NULL);
		break;

	default:
		log_err("xnvme_fioe: queue(): ENOSYS: %u\n", io_u->ddir);
		err = -1;
		assert(false);
		break;
	}

	switch (err) {
	case 0:
		return FIO_Q_QUEUED;

	case -EBUSY:
	case -EAGAIN:
		xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);
		return FIO_Q_BUSY;

	default:
		log_err("xnvme_fioe: queue(): err: '%d'\n", err);

		xnvme_queue_put_cmd_ctx(ctx->async.queue, ctx);

		io_u->error = abs(err);
		assert(false);
		return FIO_Q_COMPLETED;
	}
}

// See CAVEAT for explanation and _cleanup() + _dev_close() for implementation
static int
xnvme_fioe_close(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	XNVME_DEBUG("xnvme_fioe_close: closing -- nopen: %ld", xd->nopen);
	XNVME_DEBUG_FCALL(_fio_file_pr(f);)

	--(xd->nopen);

	return 0;
}

// See CAVEAT for explanation and _init() + _dev_open() for implementation
static int
xnvme_fioe_open(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_fioe_data *xd = td->io_ops_data;

	XNVME_DEBUG("xnvme_fioe_open: opening -- nopen: %ld", xd->nopen);
	XNVME_DEBUG_FCALL(_fio_file_pr(f);)

	if (f->fileno > (int)xd->nallocated) {
		XNVME_DEBUG("f->fileno > xd->nallocated; invalid assumption");
		return 1;
	}
	if (xd->files[f->fileno].fio_file != f) {
		XNVME_DEBUG("well... that is off..");
		return 1;
	}

	++(xd->nopen);

	return 0;
}

static int
xnvme_fioe_invalidate(struct thread_data *td, struct fio_file *f)
{
	// Consider only doing this with be:spdk
	return 0;
}

#if defined FIO_IOOPS_VERSION && FIO_IOOPS_VERSION >= 30
static int
xnvme_fioe_get_max_open_zones(struct thread_data *td, struct fio_file *f,
			      unsigned int *max_open_zones)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	int err = 0, err_lock;

	if (f->filetype != FIO_TYPE_FILE && \
		f->filetype != FIO_TYPE_BLOCK && \
		f->filetype != FIO_TYPE_CHAR) {
		XNVME_DEBUG("INFO: ignoring filetype: %d", f->filetype);
		return 0;
	}
	err_lock = pthread_mutex_lock(&g_serialize);
	if (err_lock) {
		XNVME_DEBUG("FAILED: pthread_mutex_lock(), err_lock: %d", err_lock);
		return -err_lock;
	}

	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		XNVME_DEBUG("FAILED: retrieving device handle");
		err = -errno;
		goto exit;
	}
	if (xnvme_dev_get_geo(dev)->type != XNVME_GEO_ZONED) {
		errno = EINVAL;
		err = -errno;
		goto exit;
	}

	{
		const struct xnvme_spec_znd_idfy_ns *zns;

		zns = (void*)xnvme_dev_get_ns_css(dev);
		if (!zns) {
			XNVME_DEBUG("FAILED: xnvme_dev_get_ns_css(), errno: %d", errno);
			err = -errno;
			goto exit;
		}

		// intentional overflow as the value is zero-based and NVMe defines 0xFFFFFFFF
		// as unlimited thus overflowing to 0 which is how fio indicates unlimited and
		// otherwise just converting to one-based
		*max_open_zones = zns->mor + 1;
	}

exit:
	xnvme_dev_close(dev);
	err_lock = pthread_mutex_unlock(&g_serialize);
	if (err_lock) {
		XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err_lock: %d", err_lock);
	}

	return err;
}
#endif

/**
 * Currently, this function is called before of I/O engine initialization, so,
 * we cannot consult the file-wrapping done when 'fioe' initializes.
 * Instead we just open based on the given filename.
 *
 * TODO: unify the different setup methods, consider keeping the handle around,
 * and consider how to support the --be option in this usecase
 */
static int
xnvme_fioe_get_zoned_model(struct thread_data *td, struct fio_file *f, enum zbd_zoned_model *model)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	int err = 0;

	if (f->filetype != FIO_TYPE_FILE && \
		f->filetype != FIO_TYPE_BLOCK && \
		f->filetype != FIO_TYPE_CHAR) {
		XNVME_DEBUG("INFO: ignoring filetype: %d", f->filetype);
		return -EINVAL;
	}

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_lock(), err: %d", err);
		return -err;
	}

	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		XNVME_DEBUG("FAILED: retrieving device handle");
		err = -errno;
		goto exit;
	}

	switch (xnvme_dev_get_geo(dev)->type) {
	case XNVME_GEO_UNKNOWN:
		XNVME_DEBUG("INFO: got 'unknown', assigning ZBD_NONE");
		*model = ZBD_NONE;
		break;

	case XNVME_GEO_CONVENTIONAL:
		XNVME_DEBUG("INFO: got 'conventional', assigning ZBD_NONE");
		*model = ZBD_NONE;
		break;

	case XNVME_GEO_ZONED:
		XNVME_DEBUG("INFO: got 'zoned', assigning ZBD_HOST_MANAGED");
		*model = ZBD_HOST_MANAGED;
		break;

	default:
		XNVME_DEBUG("FAILED: hit-default, assigning ZBD_NONE");
		*model = ZBD_NONE;
		errno = EINVAL;
		err = -errno;
		break;
	}

exit:
	xnvme_dev_close(dev);
	{
		int err_lock;

		err_lock = pthread_mutex_unlock(&g_serialize);
		if (err_lock) {
			XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err: %d", err_lock);
		}
	}

	XNVME_DEBUG("INFO: so good to far...");

	return err;
}

/**
 * Fills the given ``zbdz`` with at most ``nr_zones`` zone-descriptors.
 *
 * The implementation converts the NVMe Zoned Command Set log-pages for Zone
 * descriptors into the Linux Kernel Zoned Block Report format.
 *
 * NOTE: This function is called before I/O engine initialization, that is,
 * before ``_dev_open`` has been called and file-wrapping is setup. Thus is has
 * to do the ``_dev_open`` itself, and shut it down again once it is done
 * retrieving the log-pages and converting them to the report format.
 *
 * TODO: unify the different setup methods, consider keeping the handle around,
 * and consider how to support the --async option in this usecase
 */
static int
xnvme_fioe_report_zones(struct thread_data *td, struct fio_file *f, uint64_t offset,
			struct zbd_zone *zbdz, unsigned int nr_zones)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	const struct xnvme_spec_znd_idfy_lbafe *lbafe = NULL;
	struct xnvme_dev *dev = NULL;
	const struct xnvme_geo *geo = NULL;
	struct xnvme_znd_report *rprt = NULL;
	uint32_t ssw;
	uint64_t slba;
	unsigned int limit = 0;
	int err = 0;

	XNVME_DEBUG("report_zones(): '%s', offset: %zu, nr_zones: %u",
		    f->file_name, offset, nr_zones);

	err = pthread_mutex_lock(&g_serialize);
	if (err) {
		XNVME_DEBUG("FAILED: pthread_mutex_lock(), err: %d", err);
		return -err;
	}

	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		XNVME_DEBUG("FAILED: xnvme_dev_open(), errno: %d", errno);
		goto exit;
	}

	geo = xnvme_dev_get_geo(dev);
	ssw = xnvme_dev_get_ssw(dev);
	lbafe = xnvme_znd_dev_get_lbafe(dev);

	limit = nr_zones > geo->nzone ? geo->nzone : nr_zones;

	XNVME_DEBUG("INFO: limit: %u", limit);

	slba = ((offset >> ssw) / geo->nsect) * geo->nsect;

	rprt = xnvme_znd_report_from_dev(dev, slba, limit, 0);
	if (!rprt) {
		XNVME_DEBUG("FAILED: xnvme_znd_report_from_dev(), errno: %d", errno);
		err = -errno;
		goto exit;
	}
	if (rprt->nentries != limit) {
		XNVME_DEBUG("FAILED: nentries != nr_zones");
		err = 1;
		goto exit;
	}
	if (offset > geo->tbytes) {
		XNVME_DEBUG("INFO: out-of-bounds");
		goto exit;
	}

	// Transform the zone-report
	for (uint32_t idx = 0; idx < rprt->nentries; ++idx) {
		struct xnvme_spec_znd_descr *descr = XNVME_ZND_REPORT_DESCR(rprt, idx);

		zbdz[idx].start = descr->zslba << ssw;
		zbdz[idx].len = lbafe->zsze << ssw;
		zbdz[idx].capacity = descr->zcap << ssw;
		zbdz[idx].wp = descr->wp << ssw;

		switch (descr->zt) {
		case XNVME_SPEC_ZND_TYPE_SEQWR:
			zbdz[idx].type = ZBD_ZONE_TYPE_SWR;
			break;

		default:
			log_err("%s: invalid type for zone at offset%zu.\n",
				f->file_name, zbdz[idx].start);
			err = -EIO;
			goto exit;
		}

		switch (descr->zs) {
		case XNVME_SPEC_ZND_STATE_EMPTY:
			zbdz[idx].cond = ZBD_ZONE_COND_EMPTY;
			break;
		case XNVME_SPEC_ZND_STATE_IOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_IMP_OPEN;
			break;
		case XNVME_SPEC_ZND_STATE_EOPEN:
			zbdz[idx].cond = ZBD_ZONE_COND_EXP_OPEN;
			break;
		case XNVME_SPEC_ZND_STATE_CLOSED:
			zbdz[idx].cond = ZBD_ZONE_COND_CLOSED;
			break;
		case XNVME_SPEC_ZND_STATE_FULL:
			zbdz[idx].cond = ZBD_ZONE_COND_FULL;
			break;

		case XNVME_SPEC_ZND_STATE_RONLY:
		case XNVME_SPEC_ZND_STATE_OFFLINE:
		default:
			zbdz[idx].cond = ZBD_ZONE_COND_OFFLINE;
			break;
		}
	}

exit:
	xnvme_buf_virt_free(rprt);

	xnvme_dev_close(dev);
	{
		int err_lock;

		err_lock = pthread_mutex_unlock(&g_serialize);
		if (err_lock) {
			XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err_lock: %d", err_lock);
		}
	}
	XNVME_DEBUG("err: %d, nr_zones: %d", err, (int)nr_zones);

	return err ? err : (int)limit;
}

/**
 * NOTE: This function may get called before I/O engine initialization, that is,
 * before ``_dev_open`` has been called and file-wrapping is setup. In such
 * case it has to do ``_dev_open`` itself, and shut it down again once it is
 * done resetting write pointer of zones.
 */
static int
xnvme_fioe_reset_wp(struct thread_data *td, struct fio_file *f, uint64_t offset, uint64_t length)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_fioe_data *xd = NULL;
	struct xnvme_fioe_fwrap *fwrap = NULL;
	struct xnvme_dev *dev = NULL;
	const struct xnvme_geo *geo = NULL;
	uint64_t first, last;
	uint32_t ssw;
	uint32_t nsid;
	int err = 0;

	if (td->io_ops_data) {
		xd = td->io_ops_data;
		fwrap = &xd->files[f->fileno];

		assert(fwrap->dev);
		assert(fwrap->geo);

		dev = fwrap->dev;
		geo = fwrap->geo;
		ssw = fwrap->ssw;
	} else {
		err = pthread_mutex_lock(&g_serialize);
		if (err) {
			XNVME_DEBUG("FAILED: pthread_mutex_lock(), err: %d", err);
			return -err;
		}

		dev = xnvme_dev_open(f->file_name, &opts);
		if (!dev) {
			XNVME_DEBUG("FAILED: xnvme_dev_open(), errno: %d", errno);
			goto exit;
		}
		geo = xnvme_dev_get_geo(dev);
		ssw = xnvme_dev_get_ssw(dev);
	}

	nsid = xnvme_dev_get_nsid(dev);

	first = ((offset >> ssw) / geo->nsect) * geo->nsect;
	last = (((offset + length) >> ssw) / geo->nsect) * geo->nsect;
	XNVME_DEBUG("INFO: first: 0x%lx, last: 0x%lx", first, last);

	for (uint64_t zslba = first; zslba < last; zslba += geo->nsect) {
		struct xnvme_cmd_ctx ctx = xnvme_cmd_ctx_from_dev(dev);

		if (zslba >= (geo->nsect * geo->nzone)) {
			XNVME_DEBUG("INFO: out-of-bounds");
			err = 0;
			break;
		}

		err = xnvme_znd_mgmt_send(&ctx, nsid, zslba, false,
					  XNVME_SPEC_ZND_CMD_MGMT_SEND_RESET, 0x0, NULL);
		if (err || xnvme_cmd_ctx_cpl_status(&ctx)) {
			err = err ? err : -EIO;
			XNVME_DEBUG("FAILED: err: %d, sc=%d", err, ctx.cpl.status.sc);
			goto exit;
		}
	}

exit:
	if (!td->io_ops_data) {
		xnvme_dev_close(dev);
		{
			int err_lock;

			err_lock = pthread_mutex_unlock(&g_serialize);
			if (err_lock) {
				XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err: %d", err_lock);
			}
		}
	}

	return err;
}
static int
xnvme_fioe_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct xnvme_opts opts = xnvme_opts_from_fioe(td);
	struct xnvme_dev *dev;
	int ret = 0;

	if (fio_file_size_known(f))
		return 0;

	ret = pthread_mutex_lock(&g_serialize);
	if (ret) {
		XNVME_DEBUG("FAILED: pthread_mutex_lock(), err: %d", ret);
		return -ret;
	}

	dev = xnvme_dev_open(f->file_name, &opts);
	if (!dev) {
		XNVME_DEBUG("FAILED: xnvme_dev_open(), errno: %d", errno);
		ret = -errno;
		goto exit;
	}

	f->real_file_size = xnvme_dev_get_geo(dev)->tbytes;
	fio_file_set_size_known(f);
	f->filetype = FIO_TYPE_BLOCK;

exit:
	xnvme_dev_close(dev);
	{
		int err = pthread_mutex_unlock(&g_serialize);
		if (err) {
			XNVME_DEBUG("FAILED: pthread_mutex_unlock(), err: %d", err);
		}
	}

	return ret;

}
struct ioengine_ops ioengine = {
	.name			= "xnvme",
	.version		= FIO_IOOPS_VERSION,
	.options		= options,
	.option_struct_size	= sizeof(struct xnvme_fioe_options),
	.flags			= \
	FIO_DISKLESSIO | \
	FIO_NODISKUTIL | \
	FIO_NOEXTEND | \
	FIO_MEMALIGN | \
	FIO_RAWIO,

	.cleanup	= xnvme_fioe_cleanup,
	.init		= xnvme_fioe_init,

	.iomem_free	= xnvme_fioe_iomem_free,
	.iomem_alloc	= xnvme_fioe_iomem_alloc,

	.io_u_free	= xnvme_fioe_io_u_free,
	.io_u_init	= xnvme_fioe_io_u_init,

	.event		= xnvme_fioe_event,
	.getevents	= xnvme_fioe_getevents,
	.queue		= xnvme_fioe_queue,

	.close_file	= xnvme_fioe_close,
	.open_file	= xnvme_fioe_open,
	.get_file_size  = xnvme_fioe_get_file_size,

	.invalidate		= xnvme_fioe_invalidate,
#if defined FIO_IOOPS_VERSION && FIO_IOOPS_VERSION >= 30
	.get_max_open_zones	= xnvme_fioe_get_max_open_zones,
#endif
	.get_zoned_model	= xnvme_fioe_get_zoned_model,
	.report_zones		= xnvme_fioe_report_zones,
	.reset_wp		= xnvme_fioe_reset_wp,
};
