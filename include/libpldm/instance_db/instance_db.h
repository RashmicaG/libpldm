#ifndef INSTANCE_DB_H
#define INSTANCE_DB_H

#ifdef __cplusplus
extern "C" {
#endif

//#include <stddef.h>
#include <stdint.h>
#include "libpldm/pldm.h"

typedef uint8_t pldm_iid_t;
struct pldm_instance_db;
int pldm_instance_db_init(struct pldm_instance_db *ctx, const char *dbpath);
int pldm_instance_db_init_default(struct pldm_instance_db *ctx);
int pldm_instance_db_destroy(struct pldm_instance_db *ctx);

/**
 * @brief Allocates an instance ID for a destination TID
 *
 * @param[in] ctx - pldm requester instance
 * @param[in] tid - PLDM TID
 * @param[in] instance_id - caller owned pointer to a PLDM instance ID object. Return
 * 	      PLDM_REQUESTER_INVALID_SETUP if this is NULL. On success, this
 * 	      points to an instance ID to use for a PLDM request message. If
 * 	      there are no instance IDs available,
 * 	      PLDM_REQUESTER_INSTANCE_IDS_EXHAUSTED is returned. Other failures
 * 	      are indicated by the return code PLDM_REQUESTER_INSTANCE_ID_FAIL.
 *
 * @return pldm_requester_rc_t.
 */
int pldm_instance_db_alloc(struct pldm_instance_db *ctx, pldm_tid_t tid, pldm_iid_t *iid);

/**
 * @brief Frees an instance id previously allocated by
 * 	  pldm_requester_allocate_instance_id
 *
 * @param[in] ctx - pldm requester instance
 * @param[in] tid - PLDM TID
 * @param[in] instance_id - If this instance ID was not previously allocated by
 * 	      pldm_requester_allocate_instance_id then the behaviour is
 * 	      undefined.
 *
 * @return pldm_requester_rc_t
 */
int pldm_instance_db_free(struct pldm_instance_db *ctx, pldm_tid_t tid, pldm_iid_t iid);


#ifdef __cplusplus
}
#endif

#endif /* INSTANCE_DB_H */
