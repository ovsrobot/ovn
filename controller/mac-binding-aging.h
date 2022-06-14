
/* Copyright (c) 2022, Red Hat, Inc.
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

#ifndef OVN_MAC_BINDING_AGING_H
#define OVN_MAC_BINDING_AGING_H 1

#include "ovn-sb-idl.h"

void mac_binding_aging_init(void);
void mac_binding_aging_destroy(void);
void mac_binding_aging_run(struct ovsdb_idl_txn *ovnsb_idl_txn,
                           const char *br_int_name,
                           const struct sbrec_chassis *chassis,
                           const struct sbrec_mac_binding_table
                           *mac_binding_table,
                           struct ovsdb_idl_index *mb_by_chassis_index,
                           unsigned long long threshold);

#endif /* controller/mac-binding-aging.h */
