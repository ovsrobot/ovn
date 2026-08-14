#ifndef EN_IC_ROUTE_H
#define EN_IC_ROUTE_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_route_run(struct engine_node *node, void *data);
void *en_route_init(struct engine_node *node, struct engine_arg *arg);
void en_route_cleanup(void *data);

#endif /* EN_IC_ROUTE_H */
