#ifndef EN_IC_TR_H
#define EN_IC_TR_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_tr_run(struct engine_node *node, void *data);
void *en_tr_init(struct engine_node *node, struct engine_arg *arg);
void en_tr_cleanup(void *data);

enum engine_input_handler_result
en_tr_nb_logical_router_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
en_tr_icsb_datapath_binding_handler(struct engine_node *node, void *data);

#endif /* EN_IC_TR_H */
