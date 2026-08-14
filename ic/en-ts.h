#ifndef EN_IC_TS_H
#define EN_IC_TS_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_ts_run(struct engine_node *node, void *data);
void *en_ts_init(struct engine_node *node, struct engine_arg *arg);
void en_ts_cleanup(void *data);

enum engine_input_handler_result
en_ts_icnb_transit_switch_handler(struct engine_node *node, void *data);

enum engine_input_handler_result
en_ts_nb_logical_switch_handler(struct engine_node *node, void *data);

enum engine_input_handler_result
en_ts_icsb_datapath_binding_handler(struct engine_node *node, void *data);

#endif /* EN_IC_TS_H */
