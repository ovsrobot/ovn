#ifndef EN_IC_ROUTE_H
#define EN_IC_ROUTE_H 1

#include "lib/inc-proc-eng.h"

enum engine_node_state en_route_run(struct engine_node *node, void *data);
void *en_route_init(struct engine_node *node, struct engine_arg *arg);
void en_route_clear_tracked_data(void *data);
void en_route_cleanup(void *data);

enum engine_input_handler_result
route_nb_nb_global_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_nb_logical_switch_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_nb_logical_router_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_nb_logical_router_static_route_handler(struct engine_node *node,
                                             void *data);
enum engine_input_handler_result
route_nb_logical_router_port_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_nb_logical_switch_port_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_nb_load_balancer_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_nb_load_balancer_group_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_icsb_route_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_icsb_port_binding_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_icnb_transit_switch_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_sb_datapath_binding_handler(struct engine_node *node, void *data);
enum engine_input_handler_result
route_sb_learned_route_handler(struct engine_node *node, void *data);

#endif /* EN_IC_ROUTE_H */
