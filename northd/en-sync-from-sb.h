#ifndef EN_SYNC_FROM_SB_H
#define EN_SYNC_FROM_SB_H 1

#include "lib/inc-proc-eng.h"

struct en_sync_from_sb_data {
    /* Southbound table references */
    const struct sbrec_port_binding_table *sb_pb_table;
    const struct sbrec_ha_chassis_group_table *sb_ha_ch_grp_table;
};

void *en_sync_from_sb_init(struct engine_node *, struct engine_arg *);
enum engine_node_state en_sync_from_sb_run(struct engine_node *, void *data);
void en_sync_from_sb_cleanup(void *data);
enum engine_input_handler_result
sync_from_sb_northd_handler(struct engine_node *, void *data OVS_UNUSED);

#endif /* end of EN_SYNC_FROM_SB_H */
