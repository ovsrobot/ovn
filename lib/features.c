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

#include "ovn/features.h"

static enum ovs_feature_support ovs_features;

enum ovs_feature_support
ovs_feature_support_get(void)
{
    return ovs_features;
}

/* Returns 'true' if the OVS feature set has been updated since the last
 * call.
 */
bool
ovs_feature_support_update(enum ovs_feature_support features)
{
    if (features != ovs_features) {
        ovs_features = features;
        return true;
    }
    return false;
}
