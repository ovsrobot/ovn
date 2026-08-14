#ifndef EN_IC_SERVICE_MONITOR_H
#define EN_IC_SERVICE_MONITOR_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_service_monitor_run(struct engine_node *node,
                                              void *data);
void *en_service_monitor_init(struct engine_node *node,
                              struct engine_arg *arg);
void en_service_monitor_cleanup(void *data);

enum engine_input_handler_result
en_service_monitor_sb_port_binding_handler(struct engine_node *node,
                                           void *data);

#endif /* EN_IC_SERVICE_MONITOR_H */
