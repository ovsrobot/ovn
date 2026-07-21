#ifndef EN_IC_DP_ENUM_H
#define EN_IC_DP_ENUM_H 1

#include <config.h>

#include "lib/inc-proc-eng.h"
#include "openvswitch/hmap.h"
#include "openvswitch/shash.h"

/* Data maintained by the "dp_enum" engine node: the result of enumerating the
 * IC-SB datapath bindings.  It is the explicit, engine-visible form of the
 * state that used to be built on the stack by enumerate_datapaths() inside
 * ovn_db_run().  The transit-switch (en_ts) and transit-router (en_tr) nodes
 * consume this data instead of rebuilding it themselves, which also keeps the
 * shared 'dp_tnlids' tunnel-key allocator consistent between them. */
struct ed_type_dp_enum {
    /* Set of tunnel keys already in use by IC-SB datapath bindings.  Holds
     * 'struct tnlid_node *' entries owned by this node. */
    struct hmap dp_tnlids;
    /* Transit-switch datapath bindings, keyed by transit switch name.  Values
     * are 'const struct icsbrec_datapath_binding *' owned by the IDL. */
    struct shash isb_ts_dps;
    /* Transit-router datapath bindings, keyed by NB IC UUID string.  Values
     * are 'const struct icsbrec_datapath_binding *' owned by the IDL. */
    struct shash isb_tr_dps;
};

enum engine_node_state en_dp_enum_run(struct engine_node *node, void *data);
void *en_dp_enum_init(struct engine_node *node, struct engine_arg *arg);
void en_dp_enum_cleanup(void *data);

enum engine_input_handler_result
en_dp_enum_icsb_datapath_binding_handler(struct engine_node *node, void *data);

/* Shared handler for the IC-NB Global input: ignores nb_ic_cfg/sb_ic_cfg
 * sequence-number-only changes, recomputes on options (vxlan_mode) changes. */
enum engine_input_handler_result
en_ic_nb_global_handler(struct engine_node *node, void *data);

/* Shallow-copies the datapath shash 'src' into 'dst' (initialized here).  The
 * values (IDL row pointers) are shared, not duplicated.  en_ts and en_tr
 * destructively consume their datapath shash (removing matched entries and
 * deleting the leftovers' datapaths), so they must operate on a copy to keep
 * en_dp_enum's authoritative maps intact across iterations. */
void dp_enum_shash_clone(struct shash *dst, const struct shash *src);

#endif /* EN_IC_DP_ENUM_H */
