/* Copyright (c) 2024 Red Hat, INc.
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

#ifndef OVN_ACL_IDS_H
#define OVN_ACL_IDS_H

#include <config.h>
#include "lib/inc-proc-eng.h"

void *en_acl_id_init(struct engine_node *, struct engine_arg *);
void en_acl_id_run(struct engine_node *, void *);
void en_acl_id_cleanup(void *);

struct tracked_acl_ids;
void acl_ids_update_swconn(const char *target, int probe_interval);
void acl_ids_run(struct tracked_acl_ids *);

#endif /* OVN_ACL_IDS_H */
