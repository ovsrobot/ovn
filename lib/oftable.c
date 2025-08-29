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
#include <config.h>

#include "lib/oftable.h"

int OFTABLE_PHY_TO_LOG = 0;

/* Start of LOG_PIPELINE_LEN tables. */
int OFTABLE_LOG_INGRESS_PIPELINE = 8;
int OFTABLE_OUTPUT_LARGE_PKT_DETECT  = 41, OFTABLE_OUTPUT_INIT = 41;
int OFTABLE_OUTPUT_LARGE_PKT_PROCESS = 42;
int OFTABLE_REMOTE_OUTPUT            = 43;
int OFTABLE_REMOTE_VTEP_OUTPUT       = 44;
int OFTABLE_LOCAL_OUTPUT             = 45;
int OFTABLE_CHECK_LOOPBACK           = 46;

/* Start of LOG_PIPELINE_LEN tables. */
int OFTABLE_LOG_EGRESS_PIPELINE  = 47;
int OFTABLE_SAVE_INPORT          = 64;
int OFTABLE_LOG_TO_PHY           = 65;
int OFTABLE_MAC_BINDING          = 66;
int OFTABLE_MAC_LOOKUP           = 67;
int OFTABLE_CHK_LB_HAIRPIN       = 68;
int OFTABLE_CHK_LB_HAIRPIN_REPLY = 69;
int OFTABLE_CT_SNAT_HAIRPIN      = 70;
int OFTABLE_GET_FDB              = 71;
int OFTABLE_LOOKUP_FDB           = 72;
int OFTABLE_CHK_IN_PORT_SEC      = 73;
int OFTABLE_CHK_IN_PORT_SEC_ND   = 74;
int OFTABLE_CHK_OUT_PORT_SEC     = 75;
int OFTABLE_ECMP_NH_MAC          = 76;
int OFTABLE_ECMP_NH              = 77;
int OFTABLE_CHK_LB_AFFINITY      = 78;
int OFTABLE_MAC_CACHE_USE        = 79;
int OFTABLE_CT_ZONE_LOOKUP       = 80;
int OFTABLE_CT_ORIG_NW_DST_LOAD  = 81;
int OFTABLE_CT_ORIG_IP6_DST_LOAD = 82;
int OFTABLE_CT_ORIG_TP_DST_LOAD  = 83;
int OFTABLE_FLOOD_REMOTE_CHASSIS = 84;
int OFTABLE_CT_STATE_SAVE        = 85;
int OFTABLE_CT_ORIG_PROTO_LOAD   = 86;
int OFTABLE_GET_REMOTE_FDB       = 87;
