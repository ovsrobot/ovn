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

#ifndef OVN_FDB_H
#define OVN_FDB_H 1

#include "ovsdb-idl.h"
#include "ovn-sb-idl.h"

void
delete_fdb_entries(struct ovsdb_idl_index *sbrec_fdb_by_dp_and_port,
                   uint32_t dp_key, uint32_t port_key);
const struct sbrec_fdb *
fdb_lookup(struct ovsdb_idl_index *sbrec_fdb_by_dp_key_mac,
           uint32_t dp_key, const char *mac);

#endif
