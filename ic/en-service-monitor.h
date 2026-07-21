#ifndef EN_IC_SERVICE_MONITOR_H
#define EN_IC_SERVICE_MONITOR_H 1

#include <config.h>

#include "lib/inc-proc-eng.h"

enum engine_node_state en_service_monitor_run(struct engine_node *node,
                                              void *data);
void *en_service_monitor_init(struct engine_node *node,
                              struct engine_arg *arg);
void en_service_monitor_cleanup(void *data);

#endif /* EN_IC_SERVICE_MONITOR_H */
