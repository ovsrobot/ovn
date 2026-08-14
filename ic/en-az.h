#ifndef EN_IC_AZ_H
#define EN_IC_AZ_H 1

#include "lib/inc-proc-eng.h"

struct icsbrec_availability_zone;

/* Data maintained by the "az" engine node: the availability zone this ovn-ic
 * instance runs for.  This replaces the former az_run() helper and its file
 * scope 'az_name' static variable. */
struct ed_type_az {
    /* The availability zone record for this instance, or NULL if it is not
     * known/registered yet.  Owned by the IDL. */
    const struct icsbrec_availability_zone *runned_az;
    /* The name this instance registered as, persisted across iterations so a
     * later rename can update the existing IC-SB record. */
    char *az_name;
};

enum engine_node_state en_az_run(struct engine_node *node, void *data);
void *en_az_init(struct engine_node *node, struct engine_arg *arg);
void en_az_cleanup(void *data);

#endif /* EN_IC_AZ_H */
