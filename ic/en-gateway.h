#ifndef EN_IC_GATEWAY_H
#define EN_IC_GATEWAY_H 1

#include <config.h>

#include "lib/inc-proc-eng.h"

enum engine_node_state en_gateway_run(struct engine_node *node, void *data);
void *en_gateway_init(struct engine_node *node, struct engine_arg *arg);
void en_gateway_cleanup(void *data);

#endif /* EN_IC_GATEWAY_H */
