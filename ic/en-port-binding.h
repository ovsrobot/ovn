#ifndef EN_IC_PORT_BINDING_H
#define EN_IC_PORT_BINDING_H 1

#include <config.h>

#include "lib/inc-proc-eng.h"

enum engine_node_state en_port_binding_run(struct engine_node *node,
                                           void *data);
void *en_port_binding_init(struct engine_node *node, struct engine_arg *arg);
void en_port_binding_clear_tracked_data(void *data);
void en_port_binding_cleanup(void *data);

enum engine_input_handler_result
port_binding_icnb_transit_switch_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
port_binding_icnb_transit_router_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
port_binding_icnb_transit_router_port_handler(struct engine_node *node,
                                              void *data);
enum engine_input_handler_result
port_binding_icsb_port_binding_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
port_binding_nb_logical_switch_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
port_binding_nb_logical_switch_port_handler(struct engine_node *node,
                                            void *data);
enum engine_input_handler_result
port_binding_nb_logical_router_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
port_binding_nb_logical_router_port_handler(struct engine_node *node,
                                            void *data);
enum engine_input_handler_result
port_binding_sb_port_binding_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
port_binding_sb_chassis_handler(struct engine_node *node, void *data);

#endif /* EN_IC_PORT_BINDING_H */
