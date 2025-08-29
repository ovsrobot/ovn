/* Copyright (c) 2015, 2016 Nicira, Inc.
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
#include <stdint.h>

/* OpenFlow table numbers.
 *
 * These are heavily documented in ovn-architecture(7), please update it if
 * you make any changes. */
extern int OFTABLE_PHY_TO_LOG;

/* Start of LOG_PIPELINE_LEN tables. */
extern int OFTABLE_LOG_INGRESS_PIPELINE;
extern int OFTABLE_OUTPUT_LARGE_PKT_DETECT;
extern int OFTABLE_OUTPUT_INIT;
extern int OFTABLE_OUTPUT_LARGE_PKT_PROCESS;
extern int OFTABLE_REMOTE_OUTPUT;
extern int OFTABLE_REMOTE_VTEP_OUTPUT;
extern int OFTABLE_LOCAL_OUTPUT;
extern int OFTABLE_CHECK_LOOPBACK;

/* Start of LOG_PIPELINE_LEN tables. */
extern int OFTABLE_LOG_EGRESS_PIPELINE;
extern int OFTABLE_SAVE_INPORT;
extern int OFTABLE_LOG_TO_PHY;
extern int OFTABLE_MAC_BINDING;
extern int OFTABLE_MAC_LOOKUP;
extern int OFTABLE_CHK_LB_HAIRPIN;
extern int OFTABLE_CHK_LB_HAIRPIN_REPLY;
extern int OFTABLE_CT_SNAT_HAIRPIN;
extern int OFTABLE_GET_FDB;
extern int OFTABLE_LOOKUP_FDB;
extern int OFTABLE_CHK_IN_PORT_SEC;
extern int OFTABLE_CHK_IN_PORT_SEC_ND;
extern int OFTABLE_CHK_OUT_PORT_SEC;
extern int OFTABLE_ECMP_NH_MAC;
extern int OFTABLE_ECMP_NH;
extern int OFTABLE_CHK_LB_AFFINITY;
extern int OFTABLE_MAC_CACHE_USE;
extern int OFTABLE_CT_ZONE_LOOKUP;
extern int OFTABLE_CT_ORIG_NW_DST_LOAD;
extern int OFTABLE_CT_ORIG_IP6_DST_LOAD;
extern int OFTABLE_CT_ORIG_TP_DST_LOAD;
extern int OFTABLE_FLOOD_REMOTE_CHASSIS;
extern int OFTABLE_CT_STATE_SAVE;
extern int OFTABLE_CT_ORIG_PROTO_LOAD;
extern int OFTABLE_GET_REMOTE_FDB;
