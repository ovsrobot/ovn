/*
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

#include "en-acl-ids.h"
#include "lib/uuidset.h"
#include "lib/ovn-sb-idl.h"
#include "lib/ovn-nb-idl.h"
#include "lib/bitmap.h"

#include "openvswitch/vlog.h"

VLOG_DEFINE_THIS_MODULE(northd_acl_ids);

enum id_state {
    /* The ID represents a new northbound ACL that has
     * not yet been synced to the southbound DB
     */
    ID_NEW,
    /* The ID represents an ACL ID that has been synced
     * with the southbound DB already
     */
    ID_SYNCED,
    /* The ID represents a deleted NB ACL that also needs
     * to be removed from the southbound DB
     */
    ID_INACTIVE,
};

struct acl_id {
    struct hmap_node node;
    int64_t id;
    struct uuid nb_acl_uuid;
    enum id_state state;
};

#define MAX_ACL_ID 65535

struct acl_id_data {
    struct hmap ids;
    unsigned long *id_bitmap;
};

static void
acl_id_data_init(struct acl_id_data *id_data)
{
    hmap_init(&id_data->ids);
    id_data->id_bitmap = bitmap_allocate(MAX_ACL_ID);
}

static struct acl_id_data *
acl_id_data_alloc(void)
{
    struct acl_id_data *id_data = xzalloc(sizeof *id_data);
    acl_id_data_init(id_data);

    return id_data;
}

static void
acl_id_data_destroy(struct acl_id_data *id_data)
{
    struct acl_id *acl_id;
    HMAP_FOR_EACH_POP (acl_id, node, &id_data->ids) {
        free(acl_id);
    }
    hmap_destroy(&id_data->ids);
    bitmap_free(id_data->id_bitmap);
}

void *
en_acl_id_init(struct engine_node *node OVS_UNUSED,
               struct engine_arg *arg OVS_UNUSED)
{
    struct acl_id_data *id_data = acl_id_data_alloc();
    return id_data;
}

static void
add_acl_id(struct hmap *id_map, int64_t id, enum id_state state,
           const struct uuid *acl_uuid)
{
    struct acl_id *acl_id = xzalloc(sizeof *acl_id);
    acl_id->id = id;
    acl_id->state = state;
    acl_id->nb_acl_uuid = *acl_uuid;
    hmap_insert(id_map, &acl_id->node, uuid_hash(acl_uuid));
}

void
en_acl_id_run(struct engine_node *node, void *data)
{
    const struct engine_context *eng_ctx = engine_get_context();
    if (!eng_ctx->ovnnb_idl_txn || !eng_ctx->ovnsb_idl_txn) {
        return;
    }

    const struct nbrec_acl_table *nb_acl_table =
        EN_OVSDB_GET(engine_get_input("NB_acl", node));
    const struct sbrec_acl_id_table *sb_acl_id_table =
        EN_OVSDB_GET(engine_get_input("SB_acl_id", node));
    struct uuidset visited = UUIDSET_INITIALIZER(&visited);
    struct acl_id_data *id_data = data;

    acl_id_data_destroy(id_data);
    acl_id_data_init(id_data);

    const struct nbrec_acl *nb_acl;
    const struct sbrec_acl_id *sb_id;
    SBREC_ACL_ID_TABLE_FOR_EACH (sb_id, sb_acl_id_table) {
        nb_acl = nbrec_acl_table_get_for_uuid(nb_acl_table, &sb_id->nb_acl);
        if (nb_acl && !strcmp(nb_acl->action, "allow-established")) {
            bitmap_set1(id_data->id_bitmap, sb_id->id);
            uuidset_insert(&visited, &sb_id->nb_acl);
            add_acl_id(&id_data->ids, sb_id->id, ID_SYNCED, &sb_id->nb_acl);
        } else {
            /* NB ACL is deleted or has changed action type. This
             * ID is no longer active.
             */
            add_acl_id(&id_data->ids, sb_id->id, ID_INACTIVE, &sb_id->nb_acl);
        }
    }

    size_t scan_start = 1;
    size_t scan_end = MAX_ACL_ID;
    NBREC_ACL_TABLE_FOR_EACH (nb_acl, nb_acl_table) {
        if (uuidset_find_and_delete(&visited, &nb_acl->header_.uuid)) {
            continue;
        }
        if (strcmp(nb_acl->action, "allow-established")) {
            continue;
        }
        int64_t new_id = bitmap_scan(id_data->id_bitmap, 0,
                                     scan_start, scan_end + 1);
        if (new_id == scan_end + 1) {
            static struct vlog_rate_limit rl = VLOG_RATE_LIMIT_INIT(1, 1);
            VLOG_WARN_RL(&rl, "Exhausted all ACL IDs");
            break;
        }
        add_acl_id(&id_data->ids, new_id, ID_NEW, &nb_acl->header_.uuid);
        bitmap_set1(id_data->id_bitmap, new_id);
        scan_start = new_id + 1;
    }

    engine_set_node_state(node, EN_UPDATED);
    uuidset_destroy(&visited);
}

void
en_acl_id_cleanup(void *data)
{
    acl_id_data_destroy(data);
}

static const struct sbrec_acl_id *
acl_id_lookup_by_id(struct ovsdb_idl_index *sbrec_acl_id_by_id,
                    int64_t id)
{
    struct sbrec_acl_id *target = sbrec_acl_id_index_init_row(
        sbrec_acl_id_by_id);
    sbrec_acl_id_index_set_id(target, id);

    struct sbrec_acl_id *retval = sbrec_acl_id_index_find(
        sbrec_acl_id_by_id, target);

    sbrec_acl_id_index_destroy_row(target);

    return retval;
}

void sync_acl_ids(const struct acl_id_data *id_data,
                  struct ovsdb_idl_txn *ovnsb_txn,
                  struct ovsdb_idl_index *sbrec_acl_id_by_id)
{
    struct acl_id *acl_id;
    const struct sbrec_acl_id *sb_id;
    HMAP_FOR_EACH (acl_id, node, &id_data->ids) {
        switch (acl_id->state) {
        case ID_NEW:
            sb_id = sbrec_acl_id_insert(ovnsb_txn);
            sbrec_acl_id_set_id(sb_id, acl_id->id);
            sbrec_acl_id_set_nb_acl(sb_id, acl_id->nb_acl_uuid);
            break;
        case ID_INACTIVE:
            sb_id = acl_id_lookup_by_id(sbrec_acl_id_by_id, acl_id->id);
            if (sb_id) {
                sbrec_acl_id_delete(sb_id);
            }
            break;
        case ID_SYNCED:
            break;
        }
    }
}
