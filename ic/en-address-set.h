#ifndef EN_IC_ADDRESS_SET_H
#define EN_IC_ADDRESS_SET_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_address_set_run(struct engine_node *node,
                                           void *data);
void *en_address_set_init(struct engine_node *node, struct engine_arg *arg);
void en_address_set_cleanup(void *data);

#endif /* EN_IC_ADDRESS_SET_H */
