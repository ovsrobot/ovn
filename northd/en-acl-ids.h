#ifndef EN_ACL_IDS_H
#define EN_ACL_IDS_H

#include <config.h>
#include <stdbool.h>

#include "lib/inc-proc-eng.h"

bool northd_acl_id_handler(struct engine_node *node, void *data);
void *en_acl_id_init(struct engine_node *, struct engine_arg *);
void en_acl_id_run(struct engine_node *, void *data);
void en_acl_id_cleanup(void *data);

struct acl_id_data;
void sync_acl_ids(const struct acl_id_data *, struct ovsdb_idl_txn *,
                  struct ovsdb_idl_index *sbrec_acl_id_by_id);

struct nbrec_acl;
int64_t get_acl_id(const struct acl_id_data *, const struct nbrec_acl *);
#endif
