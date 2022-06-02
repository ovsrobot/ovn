/* Copyright (c) 2020, Red Hat, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#ifndef OVN_LIB_LB_H
#define OVN_LIB_LB_H 1

#include <sys/types.h>
#include <netinet/in.h>
#include "openvswitch/hmap.h"
#include "sset.h"
#include "ovn-util.h"

struct nbrec_load_balancer;
struct sbrec_load_balancer;
struct sbrec_datapath_binding;
struct ovn_port;
struct uuid;

struct ovn_northd_lb {
    struct hmap_node hmap_node;

    const struct nbrec_load_balancer *nlb; /* May be NULL. */
    const struct sbrec_load_balancer *slb; /* May be NULL. */
    const char *proto;
    char *selection_fields;
    struct ovn_lb_vip *vips;
    struct ovn_northd_lb_vip *vips_nb;
    size_t n_vips;

    struct sset ips_v4;
    struct sset ips_v6;

    size_t n_nb_ls;
    size_t n_allocated_nb_ls;
    struct ovn_datapath **nb_ls;

    size_t n_nb_lr;
    size_t n_allocated_nb_lr;
    struct ovn_datapath **nb_lr;
};

struct ovn_lb_vip {
    struct in6_addr vip;
    char *vip_str;
    uint16_t vip_port;

    struct ovn_lb_backend *backends;
    size_t n_backends;
    bool empty_backend_rej;
};

struct ovn_lb_backend {
    struct in6_addr ip;
    char *ip_str;
    uint16_t port;
};

/* ovn-northd specific backend information. */
struct ovn_northd_lb_vip {
    char *vip_port_str;
    char *backend_ips;
    struct ovn_northd_lb_backend *backends_nb;
    size_t n_backends;

    struct nbrec_load_balancer_health_check *lb_health_check;
};

struct ovn_northd_lb_backend {
    struct ovn_port *op; /* Logical port to which the ip belong to. */
    bool health_check;
    char *svc_mon_src_ip; /* Source IP to use for monitoring. */
    const struct sbrec_service_monitor *sbrec_monitor;
};

struct ovn_northd_lb *ovn_northd_lb_create(const struct nbrec_load_balancer *);
struct ovn_northd_lb * ovn_northd_lb_find(struct hmap *, const struct uuid *);
void ovn_northd_lb_destroy(struct ovn_northd_lb *);
void
ovn_northd_lb_add_lr(struct ovn_northd_lb *lb, struct ovn_datapath *od);
void
ovn_northd_lb_add_ls(struct ovn_northd_lb *lb, struct ovn_datapath *od);

struct ovn_controller_lb {
    const struct sbrec_load_balancer *slb; /* May be NULL. */

    struct ovn_lb_vip *vips;
    size_t n_vips;
    bool hairpin_orig_tuple; /* True if ovn-northd stores the original
                              * destination tuple in registers.
                              */
    bool hairpin_use_ct_mark; /* True if ovn-northd uses ct_mark for
                               * load balancer sessions.  False if it uses
                               * ct_label.
                               */

    struct lport_addresses hairpin_snat_ips; /* IP (v4 and/or v6) to be used
                                              * as source for hairpinned
                                              * traffic.
                                              */
};

struct ovn_controller_lb *ovn_controller_lb_create(
    const struct sbrec_load_balancer *);
void ovn_controller_lb_destroy(struct ovn_controller_lb *);

#endif /* OVN_LIB_LB_H 1 */
