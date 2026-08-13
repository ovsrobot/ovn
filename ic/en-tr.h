#ifndef EN_IC_TR_H
#define EN_IC_TR_H 1

#include <config.h>

#include "lib/inc-proc-eng.h"

enum engine_node_state en_tr_run(struct engine_node *node, void *data);
void *en_tr_init(struct engine_node *node, struct engine_arg *arg);
void en_tr_cleanup(void *data);

#endif /* EN_IC_TR_H */
