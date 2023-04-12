#define _GNU_SOURCE
#include <stdlib.h>
#include <unistd.h>
#include "libpldm/instance_db/instance_db.h"
#include "libpldm/pldm.h" // for tid type and return codes

//#include <linux/fcntl.h>

#include <inttypes.h> //PRIu8
#include <err.h> //errx


#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>


#define PLDM_INSTANCE_ID_FILE_PATH "/var/lib/libpldm/"
#define PLDM_INSTANCE_ID_FILE_PREFIX                                           \
	PLDM_INSTANCE_ID_FILE_PATH "pldm_instance_ids_tid_"

#define IID_N_TIDS 256
#define IID_N_IIDS 32

struct pldm_instance_db {
	pldm_iid_t prev[IID_N_TIDS];
	int lock_db_fd;

};


static void instantiate_iid_db(const char *dbpath)
{
	int perms;
	int dims;
	int fd;
	int rc;

//	rc = unlink(dbpath); //deletes file if exists
//	if (rc < 0 && errno != ENOENT)
//		err(EXIT_FAILURE, "Failed to unlink file at path %s", dbpath);

	fd = open(dbpath, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR); 
	if (fd < 0)
		err(EXIT_FAILURE, "Failed to create instance ID database at %s", dbpath);

	dims = IID_N_TIDS * IID_N_IIDS;
	rc = ftruncate(fd, dims);
	if (rc < 0)
		err(EXIT_FAILURE, "Failed to truncate %s to %d bytes", dbpath, dims);

	perms = S_IRUSR | S_IRGRP | S_IROTH;
	rc = fchmod(fd, perms);
	if (rc < 0)
		err(EXIT_FAILURE, "Failed to chmod %s to 0%03o", dbpath, perms);

	rc = close(fd);
	if (rc < 0)
		err(EXIT_FAILURE, "Failed to close fd %d for %s", fd, dbpath);
}

/*static void generate_showlocks(char *showlocks, size_t len)
{
	int rc;

	rc = snprintf(showlocks, len, "grep OFDLCK /proc/locks");
	if (rc < 0)
		err(EXIT_FAILURE, "Failed to snprintf() into showlocks buffer");
	else if ((size_t)rc > len)
		errx(EXIT_FAILURE, "Failed to snprintf() into buffer of length %zu: %d", len, rc);
}

static void execute_showlocks(char *showlocks)
{
	int rc;

	printf("--- start /proc/locks ---\n");
	rc = system(showlocks);
	if (!WIFEXITED(rc))
		errx(EXIT_FAILURE, "Failed to execute `%s`: %d", showlocks, rc);
	printf("--- end /proc/locks ---\n\n");
}*/

static inline int iid_next(pldm_iid_t cur)
{
       	return (cur + 1) % IID_N_IIDS;
}

int pldm_instance_db_init(struct pldm_instance_db *ctx, const char *dbpath)
{
	ctx = calloc(sizeof(struct pldm_instance_db), 1);	
	if (!ctx) {
		return -1;
	}

	instantiate_iid_db(dbpath);
	/* Lock database may be read-only, either by permissions or mountpoint */
	ctx->lock_db_fd = open(dbpath, O_RDONLY | O_CLOEXEC);
	if (ctx->lock_db_fd < 0)
		return -errno;

	return 0;
	
}

int pldm_instance_db_init_default(struct pldm_instance_db *ctx)
{
    return pldm_instance_db_init(ctx, "/usr/share/libpldm/instance-db/default");
}

int pldm_instance_db_destroy(struct pldm_instance_db *ctx)
{
	if (!ctx) {
		return 0;
	}
	int rc = close(ctx->lock_db_fd);
	/* on Linux and most implementations, the file descriptor is guaranteed to be closed even if there was an error */
	free(ctx);
	return rc;
}

int pldm_instance_db_alloc(struct pldm_instance_db *ctx, pldm_tid_t tid, pldm_iid_t *iid)
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
	int _iid;

	_iid = ctx->prev[tid];
//	assert(iid < IID_N_IIDS);
	if (_iid >= IID_N_IIDS) {
		warnx("Invalid previous instance ID: %" PRIu8, _iid);
		return -EPROTO;
	}

	while ((_iid = iid_next(_iid)) != ctx->prev[tid]) {
		struct flock flop;
		off_t loff;
		int rc;

		/* Derive the instance ID offset in the lock database */
		loff = tid * IID_N_IIDS + _iid;

		/* Reserving the TID's IID. Done via a shared lock */
		flop = cfls;
		flop.l_start = loff;
		rc = fcntl(ctx->lock_db_fd, F_OFD_SETLK, &flop);
		if (rc < 0) {
			warnx("%s: fcntl(..., F_OFD_SETLK, ...)", __func__);
			return -errno;
		}

		/*
		 * If we *may* promote the lock to exclusive then this IID is only reserved by us.
		 * This is now our allocated IID.
		 *
		 * If we *may not* promote the lock to exclusive then this IID is also reserved on
		 * another file descriptor. Move on to the next IID index.
		 *
		 * Note that we cannot actually *perform* the promotion in practice because this is
		 * prevented by the lock database being opened O_RDONLY.
		 */
		flop = cflx;
		flop.l_start = loff;
		rc = fcntl(ctx->lock_db_fd, F_OFD_GETLK, &flop);
		if (rc < 0) {
			warnx("%s: fcntl(..., F_OFD_GETLK, ...)", __func__);
			return -errno;
		}

		/* F_UNLCK is the type of the lock if we could successfully promote it to F_WRLCK */
		if (flop.l_type == F_UNLCK) {
			ctx->prev[tid] = _iid;
			*iid = (uint8_t) _iid;
			return 0;
		} else if (flop.l_type == F_WRLCK) {
			warnx("Protocol violation: write lock acquired on (tid: %d, iid: %d)",
			      tid, _iid);
		} else if (flop.l_type != F_RDLCK) {
			warnx("Unexpected lock type found on (tid: %d, iid: %d) during promotion test: %hd",
			      tid, _iid, flop.l_type);
		}
	}

	/* Failed to allocate an IID after a full loop. Make the caller try again */
	return -EAGAIN;

}

int pldm_instance_db_free(struct pldm_instance_db *ctx, pldm_tid_t tid, pldm_iid_t iid)
{
	static const struct flock cflu = {
		.l_type = F_UNLCK,
		.l_whence = SEEK_SET,
		.l_len = 1,
	};
	struct flock flop;
	int rc;

	if (ctx->prev[tid] != iid)
		return -EINVAL;

	flop = cflu;
	flop.l_start = tid * IID_N_IIDS + iid;
	rc = fcntl(ctx->lock_db_fd, F_OFD_SETLK, &flop);
	if (rc < 0) {
		warnx("%s: fcntl(..., F_OFD_SETLK, ...)", __func__);
		return -errno;
	}

	return 0;

}


