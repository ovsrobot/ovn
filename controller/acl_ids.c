/* Copyright (c) 2024, Red Hat, Inc.
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

#include "openvswitch/hmap.h"
#include "openvswitch/rconn.h"
#include "openvswitch/ofp-ct.h"
#include "openvswitch/ofp-util.h"
#include "openvswitch/ofp-msgs.h"
#include "openvswitch/vlog.h"
#include "lib/socket-util.h"

#include "lib/ovn-sb-idl.h"
#include "acl_ids.h"

VLOG_DEFINE_THIS_MODULE(acl_ids);

enum acl_id_state {
    /* The ID exists in the SB DB. */
    ACTIVE,
    /* The ID has been removed from the DB and needs to have its conntrack
     * entries flushed.
     */
    SB_DELETED,
    /* We have sent the conntrack flush request to OVS for this ACL ID. */
    FLUSHING,
    /* We have either successfully flushed the ID, or we have failed enough
     * times that we have given up.
     */
    TO_DELETE,
};

struct acl_id {
    int64_t id;
    enum acl_id_state state;
    struct hmap_node hmap_node;
    ovs_be32 xid;
    int flush_count;
};

struct tracked_acl_ids {
    struct hmap ids;
};

static struct acl_id *
find_tracked_acl_id(struct tracked_acl_ids *tracked_ids, int64_t id)
{
    uint32_t hash = hash_uint64(id);
    struct acl_id *acl_id;
    HMAP_FOR_EACH_WITH_HASH (acl_id, hmap_node, hash, &tracked_ids->ids) {
        if (acl_id->id == id) {
            return acl_id;
        }
    }
    return NULL;
}

static void
acl_id_destroy(struct acl_id *acl_id)
{
    free(acl_id);
}

void *
en_acl_id_init(struct engine_node *node OVS_UNUSED,
               struct engine_arg *arg OVS_UNUSED)
{
    struct tracked_acl_ids *ids = xzalloc(sizeof *ids);
    hmap_init(&ids->ids);
    return ids;
}

void
en_acl_id_run(struct engine_node *node, void *data)
{
    const struct sbrec_acl_id_table *sb_acl_id_table =
        EN_OVSDB_GET(engine_get_input("SB_acl_id", node));
    const struct sbrec_acl_id *sb_id;

    struct tracked_acl_ids *ids = data;
    struct acl_id *id;

    /* Pre-mark each active ID as SB_DELETED. */
    HMAP_FOR_EACH (id, hmap_node, &ids->ids) {
        if (id->state == ACTIVE) {
            id->state = SB_DELETED;
        }
    }

    SBREC_ACL_ID_TABLE_FOR_EACH (sb_id, sb_acl_id_table) {
        id = find_tracked_acl_id(ids, sb_id->id);
        if (!id) {
            id = xzalloc(sizeof *id);
            id->id = sb_id->id;
            hmap_insert(&ids->ids, &id->hmap_node, hash_uint64(sb_id->id));
        }
        id->state = ACTIVE;
    }

    engine_set_node_state(node, EN_UPDATED);
}

void
en_acl_id_cleanup(void *data)
{
    struct tracked_acl_ids *tracked_ids = data;
    struct acl_id *id;
    HMAP_FOR_EACH_POP (id, hmap_node, &tracked_ids->ids) {
        acl_id_destroy(id);
    }
    hmap_destroy(&tracked_ids->ids);
}

static struct rconn *swconn;
static ovs_be32 barrier_xid;

void
acl_ids_update_swconn(const char *target, int probe_interval)
{
    if (!swconn) {
        swconn = rconn_create(0, 0, DSCP_DEFAULT, 1 << OFP15_VERSION);
    }
    ovn_update_swconn_at(swconn, target, probe_interval, "acl_ids");
}

#define MAX_FLUSHES 3

static void
acl_ids_handle_rconn_msg(struct ofpbuf *msg, struct tracked_acl_ids *acl_ids)
{
    const struct ofp_header *oh = msg->data;

    enum ofptype type;
    ofptype_decode(&type, oh);

    if (type == OFPTYPE_ECHO_REQUEST) {
        rconn_send(swconn, ofputil_encode_echo_reply(oh), NULL);
        return;
    }

    struct acl_id *acl_id;
    if (oh->xid != barrier_xid) {
        if (type != OFPTYPE_ERROR) {
            return;
        }
        /* Uh oh! It looks like one of the flushes failed :(
         * Let's find this particular one and move its state
         * back to SB_DELETED so we can retry the flush. Of
         * course, if this is a naughty little ID and has
         * been flushed unsuccessfully too many times, we'll
         * set it to TO_DELETE so it doesn't cause any more
         * trouble.
         */
        HMAP_FOR_EACH (acl_id, hmap_node, &acl_ids->ids) {
            if (acl_id->xid != oh->xid) {
                continue;
            }

            acl_id->xid = 0;
            acl_id->flush_count++;
            if (acl_id->flush_count >= MAX_FLUSHES) {
                acl_id->state = TO_DELETE;
            } else {
                acl_id->state = SB_DELETED;
            }

            break;
        }
    } else {
        HMAP_FOR_EACH (acl_id, hmap_node, &acl_ids->ids) {
            if (acl_id->state != FLUSHING) {
                continue;
            }
            acl_id->state = TO_DELETE;
        }
        barrier_xid = 0;
    }
}

static void
flush_expired_ids(struct tracked_acl_ids *acl_ids)
{
    if (barrier_xid != 0) {
        /* We haven't received the previous barrier's reply, so
         * hold off on sending new flushes until we get the
         * reply.
         */
        return;
    }

    ovs_u128 mask = {
        /* ct_labels.label BITS[80-95] */
        .u64.hi = 0xffff0000,
    };
    struct acl_id *acl_id;
    bool send_barrier = false;
    HMAP_FOR_EACH (acl_id, hmap_node, &acl_ids->ids) {
        if (acl_id->state != SB_DELETED) {
            continue;
        }
        ovs_u128 ct_id = {
            .u64.hi = acl_id->id << 16,
        };
        VLOG_DBG("Flushing conntrack entry for ACL id %"PRId64, acl_id->id);
        struct ofp_ct_match match = {
            .labels = ct_id,
            .labels_mask = mask,
        };
        struct ofpbuf *msg = ofp_ct_match_encode(&match, NULL,
                                                 rconn_get_version(swconn));
        const struct ofp_header *oh = msg->data;
        acl_id->xid = oh->xid;
        acl_id->state = FLUSHING;
        rconn_send(swconn, msg, NULL);
        send_barrier = true;
    }

    if (!send_barrier) {
        return;
    }

    struct ofpbuf *barrier = ofputil_encode_barrier_request(OFP15_VERSION);
    const struct ofp_header *oh = barrier->data;
    barrier_xid = oh->xid;
    rconn_send(swconn, barrier, NULL);
}

static void
clear_flushed_ids(struct tracked_acl_ids *acl_ids)
{
    struct acl_id *acl_id;
    HMAP_FOR_EACH_SAFE (acl_id, hmap_node, &acl_ids->ids) {
        if (acl_id->state != TO_DELETE) {
            continue;
        }
        hmap_remove(&acl_ids->ids, &acl_id->hmap_node);
        acl_id_destroy(acl_id);
    }
}

#define MAX_RECV_MSGS 50

void
acl_ids_run(struct tracked_acl_ids *acl_ids)
{
    rconn_run(swconn);
    if (!rconn_is_connected(swconn)) {
        rconn_run_wait(swconn);
        rconn_recv_wait(swconn);
        return;
    }

    for (int i = 0; i < MAX_RECV_MSGS; i++) {
        struct ofpbuf *msg = rconn_recv(swconn);
        if (!msg) {
            break;
        }
        acl_ids_handle_rconn_msg(msg, acl_ids);
        ofpbuf_delete(msg);
    }
    flush_expired_ids(acl_ids);
    clear_flushed_ids(acl_ids);

    rconn_run_wait(swconn);
    rconn_recv_wait(swconn);
}
