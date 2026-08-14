#ifndef EN_IC_PORT_BINDING_H
#define EN_IC_PORT_BINDING_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_port_binding_run(struct engine_node *node,
                                           void *data);
void *en_port_binding_init(struct engine_node *node, struct engine_arg *arg);
void en_port_binding_cleanup(void *data);

#endif /* EN_IC_PORT_BINDING_H */
