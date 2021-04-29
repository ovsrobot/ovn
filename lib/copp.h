/* Copyright (c) 2021, Red Hat, Inc.
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

#ifndef OVN_COPP_H
#define OVN_COPP_H 1

/*
 * Control plane protection - metered actions.
 */
enum copp_proto {
    COPP_PROTO_FIRST,
    COPP_ARP = COPP_PROTO_FIRST,
    COPP_ARP_RESOLVE,
    COPP_DHCPV4_OPTS,
    COPP_DHCPV6_OPTS,
    COPP_DNS,
    COPP_EVENT_ELB,
    COPP_ICMP4_ERR,
    COPP_ICMP6_ERR,
    COPP_IGMP,
    COPP_ND_NA,
    COPP_ND_NS,
    COPP_ND_NS_RESOLVE,
    COPP_ND_RA_OPTS,
    COPP_TCP_RESET,
    COPP_BFD,
    COPP_REJECT,
    COPP_PROTO_MAX,
    COPP_PROTO_INVALID = COPP_PROTO_MAX,
};

struct nbrec_copp;
struct ctl_context;

const char *copp_proto_get(enum copp_proto);

const char *copp_meter_get(enum copp_proto proto,
                           const struct nbrec_copp *copp,
                           const struct shash *meter_groups);

void copp_list(struct ctl_context *ctx, const struct nbrec_copp *copp);
const struct nbrec_copp *
copp_add_meter(struct ctl_context *ctx, const struct nbrec_copp *copp,
               const char *proto_name, const char *meter);
void
copp_del_meter(const struct nbrec_copp *copp, const char *proto_name);
char * copp_proto_validate(const char *proto_name);

#endif /* lib/copp.h */
