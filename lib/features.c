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

#include <config.h>
#include <stdint.h>
#include <stdlib.h>

#include "lib/util.h"
#include "lib/dirs.h"
#include "socket-util.h"
#include "lib/vswitch-idl.h"
#include "openvswitch/vlog.h"
#include "openvswitch/ofpbuf.h"
#include "openvswitch/rconn.h"
#include "openvswitch/ofp-msgs.h"
#include "openvswitch/ofp-meter.h"
#include "ovn/features.h"

VLOG_DEFINE_THIS_MODULE(features);

struct ovs_feature {
    enum ovs_feature_value value;
    const char *name;
};

static struct ovs_feature all_ovs_features[] = {
    {
        .value = OVS_CT_ZERO_SNAT_SUPPORT,
        .name = "ct_zero_snat"
    },
};

/* A bitmap of OVS features that have been detected as 'supported'. */
static uint32_t supported_ovs_features;

static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(5, 5);

/* ovs-vswitchd connection. */
static struct rconn *swconn;

static bool
ovs_feature_is_valid(enum ovs_feature_value feature)
{
    switch (feature) {
    case OVS_CT_ZERO_SNAT_SUPPORT:
    case OVS_DP_METER_SUPPORT:
        return true;
    default:
        return false;
    }
}

bool
ovs_feature_is_supported(enum ovs_feature_value feature)
{
    ovs_assert(ovs_feature_is_valid(feature));
    return supported_ovs_features & feature;
}

static bool
ovn_controller_get_ofp_capa(void)
{
    if (!swconn) {
        return false;
    }

    rconn_run(swconn);
    if (!rconn_is_connected(swconn)) {
        return false;
    }

    bool ret = false;
    /* dump datapath meter capabilities. */
    struct ofpbuf *msg = ofpraw_alloc(OFPRAW_OFPST13_METER_FEATURES_REQUEST,
                                      rconn_get_version(swconn), 0);
    rconn_send(swconn, msg, NULL);
    for (int i = 0; i < 10; i++) {
        msg = rconn_recv(swconn);
        if (!msg) {
            break;
        }

        const struct ofp_header *oh = msg->data;
        enum ofptype type;
        ofptype_decode(&type, oh);

        if (type == OFPTYPE_METER_FEATURES_STATS_REPLY) {
            struct ofputil_meter_features mf;
            ofputil_decode_meter_features(oh, &mf);

            bool old_state = supported_ovs_features & OVS_DP_METER_SUPPORT;
            bool new_state = mf.max_meters > 0;

            if (old_state != new_state) {
                ret = true;
                if (new_state) {
                    supported_ovs_features |= OVS_DP_METER_SUPPORT;
                } else {
                    supported_ovs_features &= ~OVS_DP_METER_SUPPORT;
                }
            }
        }
        ofpbuf_delete(msg);
    }

    return ret;
}

void
ovs_feature_support_init(const struct ovsrec_bridge *br_int)
{
    if (!swconn) {
        swconn = rconn_create(5, 0, DSCP_DEFAULT, 1 << OFP15_VERSION);
    }

    if (swconn && !rconn_is_connected(swconn)) {
        char *target = xasprintf("unix:%s/%s.mgmt", ovs_rundir(),
                                 br_int->name);
        if (strcmp(target, rconn_get_target(swconn))) {
            VLOG_INFO("%s: connecting to switch", target);
            rconn_connect(swconn, target, target);
        }
        free(target);
    }
}

void
ovs_feature_support_deinit(void)
{
    rconn_destroy(swconn);
    swconn = NULL;
}

/* Returns 'true' if the set of tracked OVS features has been updated. */
bool
ovs_feature_support_update(const struct smap *ovs_capabilities)
{
    static struct smap empty_caps = SMAP_INITIALIZER(&empty_caps);
    bool updated = false;

    if (!ovs_capabilities) {
        ovs_capabilities = &empty_caps;
    }

    if (ovn_controller_get_ofp_capa()) {
        updated = true;
    }

    for (size_t i = 0; i < ARRAY_SIZE(all_ovs_features); i++) {
        enum ovs_feature_value value = all_ovs_features[i].value;
        const char *name = all_ovs_features[i].name;
        bool old_state = supported_ovs_features & value;
        bool new_state = smap_get_bool(ovs_capabilities, name, false);
        if (new_state != old_state) {
            updated = true;
            if (new_state) {
                supported_ovs_features |= value;
            } else {
                supported_ovs_features &= ~value;
            }
            VLOG_INFO_RL(&rl, "OVS Feature: %s, state: %s", name,
                         new_state ? "supported" : "not supported");
        }
    }
    return updated;
}
