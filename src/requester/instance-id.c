// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#define _GNU_SOURCE
#include "libpldm/requester/instance-id.h"
#include "libpldm/pldm.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define PLDM_TID_MAX 256
#define PLDM_INST_ID_MAX 32

struct pldm_instance_db {
	pldm_instance_id_t prev[PLDM_TID_MAX];
	int lock_db_fd;
};

static inline int iid_next(pldm_instance_id_t cur)
{
	return (cur + 1) % PLDM_INST_ID_MAX;
}

int pldm_instance_db_init(struct pldm_instance_db **ctx, const char *dbpath)
{
	struct pldm_instance_db *l_ctx;

	/* Make sure the provided pointer was initialised to NULL. In the future
	 * if we stabilise the ABI and expose the struct definition the caller
	 * can potentially pass a valid pointer to a struct they've allocated
	 */
	if (!ctx || *ctx) {
		return -EINVAL;
	}

	l_ctx = calloc(1, sizeof(struct pldm_instance_db));
	if (!l_ctx) {
		return -ENOMEM;
	}

	/* Initialise previous ID values so the next one is zero */
	for (int i = 0; i < PLDM_TID_MAX; i++) {
		l_ctx->prev[i] = 31;
	}

	/* Lock database may be read-only, either by permissions or mountpoint
	 */
	l_ctx->lock_db_fd = open(dbpath, O_RDONLY | O_CLOEXEC);
	if (l_ctx->lock_db_fd < 0) {
		free(l_ctx);
		return -errno;
	}
	*ctx = l_ctx;

	return 0;
}

int pldm_instance_db_init_default(struct pldm_instance_db **ctx)
{
	return pldm_instance_db_init(ctx,
				     "/usr/share/libpldm/instance-db/default");
}

int pldm_instance_db_destroy(struct pldm_instance_db *ctx)
{
	if (!ctx) {
		return 0;
	}
	close(ctx->lock_db_fd);
	free(ctx);
	return 0;
}

int pldm_instance_id_alloc(struct pldm_instance_db *ctx, pldm_tid_t tid,
			   pldm_instance_id_t *iid)
{
	static const struct flock cfls = {
	    .l_type = F_RDLCK,
	    .l_whence = SEEK_SET,
	    .l_len = 1,
	};
	static const struct flock cflx = {
	    .l_type = F_WRLCK,
	    .l_whence = SEEK_SET,
	    .l_len = 1,
	};
	uint8_t l_iid;

	if (!iid) {
		return -EINVAL;
	}

	l_iid = ctx->prev[tid];
	if (l_iid >= PLDM_INST_ID_MAX) {
		return -EPROTO;
	}

	while ((l_iid = iid_next(l_iid)) != ctx->prev[tid]) {
		struct flock flop;
		off_t loff;
		int rc;

		/* Derive the instance ID offset in the lock database */
		loff = tid * PLDM_INST_ID_MAX + l_iid;

		/* Reserving the TID's IID. Done via a shared lock */
		flop = cfls;
		flop.l_start = loff;
		rc = fcntl(ctx->lock_db_fd, F_OFD_SETLK, &flop);
		if (rc < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				return -EAGAIN;
			}
			return -EPROTO;
		}

		/*
		 * If we *may* promote the lock to exclusive then this IID is
		 * only reserved by us. This is now our allocated IID.
		 *
		 * If we *may not* promote the lock to exclusive then this IID
		 * is also reserved on another file descriptor. Move on to the
		 * next IID index.
		 *
		 * Note that we cannot actually *perform* the promotion in
		 * practice because this is prevented by the lock database being
		 * opened O_RDONLY.
		 */
		flop = cflx;
		flop.l_start = loff;
		rc = fcntl(ctx->lock_db_fd, F_OFD_GETLK, &flop);
		if (rc < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				return -EAGAIN;
			}
			return -EPROTO;
		}

		/* F_UNLCK is the type of the lock if we could successfully
		 * promote it to F_WRLCK */
		if (flop.l_type == F_UNLCK) {
			ctx->prev[tid] = l_iid;
			*iid = l_iid;
			return 0;
		}
		if (flop.l_type != F_RDLCK) {
			return -EPROTO;
		}
	}

	/* Failed to allocate an IID after a full loop. Make the caller try
	 * again */
	return -EAGAIN;
}

int pldm_instance_id_free(struct pldm_instance_db *ctx, pldm_tid_t tid,
			  pldm_instance_id_t iid)
{
	static const struct flock cflu = {
	    .l_type = F_UNLCK,
	    .l_whence = SEEK_SET,
	    .l_len = 1,
	};
	struct flock flop;
	int rc;

	flop = cflu;
	flop.l_start = tid * PLDM_INST_ID_MAX + iid;
	rc = fcntl(ctx->lock_db_fd, F_OFD_SETLK, &flop);
	if (rc < 0) {
		if (errno == EAGAIN || errno == EINTR) {
			return -EAGAIN;
		}
		return -EPROTO;
	}

	return 0;
}
