/* Copyright (c) 2024, Nutanix, Inc.
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
#include "lib/fdb.h"

void
delete_fdb_entries(struct ovsdb_idl_index *sbrec_fdb_by_dp_and_port,
                   uint32_t dp_key, uint32_t port_key)
{
    struct sbrec_fdb *target =
        sbrec_fdb_index_init_row(sbrec_fdb_by_dp_and_port);
    sbrec_fdb_index_set_dp_key(target, dp_key);
    sbrec_fdb_index_set_port_key(target, port_key);

    struct sbrec_fdb *fdb_e;
    SBREC_FDB_FOR_EACH_EQUAL (fdb_e, target, sbrec_fdb_by_dp_and_port) {
        sbrec_fdb_delete(fdb_e);
    }
    sbrec_fdb_index_destroy_row(target);
}

const struct sbrec_fdb *
fdb_lookup(struct ovsdb_idl_index *sbrec_fdb_by_dp_key_mac, uint32_t dp_key,
           const char *mac)
{
    struct sbrec_fdb *fdb = sbrec_fdb_index_init_row(sbrec_fdb_by_dp_key_mac);
    sbrec_fdb_index_set_dp_key(fdb, dp_key);
    sbrec_fdb_index_set_mac(fdb, mac);

    const struct sbrec_fdb *retval
        = sbrec_fdb_index_find(sbrec_fdb_by_dp_key_mac, fdb);

    sbrec_fdb_index_destroy_row(fdb);

    return retval;
}
