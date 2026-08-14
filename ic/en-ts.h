#ifndef EN_IC_TS_H
#define EN_IC_TS_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_ts_run(struct engine_node *node, void *data);
void *en_ts_init(struct engine_node *node, struct engine_arg *arg);
void en_ts_cleanup(void *data);

#endif /* EN_IC_TS_H */
