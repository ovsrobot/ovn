#ifndef EN_SB_SYNC_H
#define EN_SB_SYNC_H 1

#include "lib/inc-proc-eng.h"

void *en_sb_sync_init(struct engine_node *, struct engine_arg *);
void en_sb_sync_run(struct engine_node *, void *data);
void en_sb_sync_cleanup(void *data);

void *en_address_set_sync_init(struct engine_node *, struct engine_arg *);
void en_address_set_sync_run(struct engine_node *, void *data);
void en_address_set_sync_cleanup(void *data);

#endif
