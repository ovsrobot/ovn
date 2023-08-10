#ifndef INC_PROC_NORTHD_H
#define INC_PROC_NORTHD_H 1

#include <config.h>

#include "northd.h"
#include "ovsdb-idl.h"

void inc_proc_northd_init(struct ovsdb_idl_loop *nb,
                          struct ovsdb_idl_loop *sb);
bool inc_proc_northd_run(struct ovsdb_idl_txn *ovnnb_txn,
                         struct ovsdb_idl_txn *ovnsb_txn,
                         bool recompute, uint32_t backoff_ms);
void inc_proc_northd_cleanup(void);
bool inc_proc_northd_can_run(bool recompute);

#endif /* INC_PROC_NORTHD */
