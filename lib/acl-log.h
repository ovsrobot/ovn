/*
 * Copyright (c) 2017 Nicira, Inc.
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

#ifndef ACL_LOG_H
#define ACL_LOG_H 1

#include <stdint.h>
#include "compiler.h"
#include "openvswitch/types.h"

struct ofpbuf;
struct flow;

struct log_pin_header {
    uint8_t direction_verdict;  /* 4 bits for LOG_DIRECTION_* and
                                 * 4 bits for LOG_VERDICT_*. */
    uint8_t severity;           /* One of LOG_SEVERITY*. */
    /* Followed by an optional string containing the rule's name. */
};

enum log_direction {
    LOG_DIRECTION_NONE,
    LOG_DIRECTION_IN,
    LOG_DIRECTION_OUT,
    LOG_DIRECTION_MAX,
};

enum log_verdict {
    LOG_VERDICT_ALLOW,
    LOG_VERDICT_DROP,
    LOG_VERDICT_REJECT,
    LOG_VERDICT_MAX,
    LOG_VERDICT_UNKNOWN = UINT8_MAX
};

/* For backwards compatibility, use the least significant 4 bits for
 * verdict values and the most significant 4 bits for direction values.
 *
 * This is backwards compatible; old encodings will be decoded as:
 * - direction: NONE
 * - verdict:   VERDICT
 */
#define LOG_VERDICT_BITS   4
#define LOG_DIRECTION_BITS 4
#define LOG_VERDICT_MASK   ((1 << LOG_VERDICT_BITS) - 1)
#define LOG_DIRECTION_MASK (0xFF ^ LOG_VERDICT_MASK)

BUILD_ASSERT_DECL(LOG_VERDICT_MAX <= (1 << LOG_VERDICT_BITS));
BUILD_ASSERT_DECL(LOG_DIRECTION_MAX <= (1 << LOG_DIRECTION_BITS));

const char *log_verdict_to_string(uint8_t verdict);
const char *log_direction_to_string(uint8_t direction);


/* Severity levels.  Based on RFC5424 levels. */
#define LOG_SEVERITY_ALERT    1
#define LOG_SEVERITY_WARNING  4
#define LOG_SEVERITY_NOTICE   5
#define LOG_SEVERITY_INFO     6
#define LOG_SEVERITY_DEBUG    7

#define LOG_DIRECTION_VERDICT(DIR, VERDICT) \
    ((DIR) << LOG_VERDICT_BITS | (VERDICT))

#define LOG_DIRECTION(DIR_VERDICT) \
    (((DIR_VERDICT) & LOG_DIRECTION_MASK) >> LOG_VERDICT_BITS)

#define LOG_VERDICT(DIR_VERDICT) \
    ((DIR_VERDICT) & LOG_VERDICT_MASK)

const char *log_severity_to_string(uint8_t severity);
uint8_t log_severity_from_string(const char *name);

void handle_acl_log(const struct flow *headers, struct ofpbuf *userdata);

#endif /* lib/acl-log.h */
