#!/bin/bash

nrtr=$1
nlb=$2
nbackends=$3
use_template=$4

echo "ROUTERS        : $nrtr"
echo "LBS            : $nlb"
echo "BACKENDS PER LB: $nbackends"
echo "USE TEMPLATE   : ${use_template}"

export OVN_NB_DAEMON=$(ovn-nbctl --detach)
export OVN_SB_DAEMON=$(ovn-sbctl --detach)
trap "killall -9 ovn-nbctl; killall -9 ovn-sbctl" EXIT

lbg=$(ovn-nbctl create load_balancer_group name=lbg)

ovn-nbctl ls-add join
ovn-nbctl lr-add cluster
ovn-nbctl lrp-add cluster rcj 00:00:00:00:00:01 10.0.0.1/8
ovn-nbctl lsp-add join sjc \
    -- lsp-set-type sjc router \
    -- lsp-set-addresses sjc router \
    --  lsp-set-options sjc router-port=rcj

for i in $(seq $nrtr); do
    ch="chassis-$i"
    gwr=lr-$i
    echo "ROUTER $gwr"
    gwr2join=lr2j-$i
    join2gwr=j2lr-$i

    ovn-nbctl lr-add $gwr \
        -- set logical_router $gwr load_balancer_group=$lbg \
        -- set logical_router $gwr options:chassis=$ch
    ovn-nbctl lrp-add $gwr $gwr2join 00:00:00:00:00:01 10.0.0.1/8
    ovn-nbctl lsp-add join $join2gwr \
        -- lsp-set-type $join2gwr router \
        -- lsp-set-addresses $join2gwr router \
        -- lsp-set-options $join2gwr router-port=$gwr2join

    s=ls-$i
    echo "SWITCH $s"
    s2cluster=s2c-$s
    cluster2s=c2s-$s
    ovn-nbctl ls-add $s \
        -- set logical_switch $s load_balancer_group=$lbg
    ovn-nbctl lsp-add $s $s2cluster \
        -- lsp-set-type $s2cluster router \
        -- lsp-set-addresses $s2cluster router \
        -- lsp-set-options $s2cluster router-port=$cluster2s
    ovn-nbctl lrp-add cluster $cluster2s 00:00:00:00:00:01 10.0.0.1/8
    ovn-nbctl lrp-set-gateway-chassis $cluster2s $ch 1

    lsp=lsp-$i
    echo "LSP $lsp"
    ovn-nbctl lsp-add $s $lsp
done

# Bind a port from the first LS locally.
ovs-vsctl add-port br-int lsp-1 \
    -- set interface lsp-1 external_ids:iface-id=lsp-1

# Add NodePort-like LBs using templates.
function add_template_lbs() {
    for l in $(seq $nlb); do
        lb=lb-$l
        echo "LB $lb"
        ovn-nbctl --template lb-add $lb "^vip:$l" "^backends$l" tcp
        lb_uuid=$(ovn-nbctl --columns _uuid --bare find load_balancer name=$lb)
        ovn-nbctl add load_balancer_group $lbg load_balancer $lb_uuid
    done

    # Generate LBs chassis_template_vars.
    python ovn-gen-lb-template-vars.py -n $nrtr -v $nlb -b $nbackends \
        -r unix:$PWD/sandbox/nb1.ovsdb
}

# Add NodePort-like LBs without using templates.
function add_non_template_lbs() {
    for i in $(seq $nrtr); do
        echo ITERATION $i
        gwr=lr-$i
        s=ls-$i
        for l in $(seq $nlb); do
            lb=lb-$l-$i
            echo LB $lb
            backends=""
            for k in $(seq $nbackends); do
                l1=$(expr $l / 250)
                l2=$(expr $l % 250)
                backends="42.$k.$l1.$l2:$l,$backends"
            done
            lb_uuid=$(ovn-nbctl create load_balancer name=$lb \
                      protocol=tcp vips:"\"42.42.42.$i:$l\""="\"$backends\"")
            ovn-nbctl add logical_switch $s load_balancer ${lb_uuid}
            ovn-nbctl add logical_router $gwr load_balancer ${lb_uuid}
        done
    done
}

if [ "${use_template}" = "yes" ]; then
    add_template_lbs
else
    add_non_template_lbs
fi

ovs-appctl -t $PWD/sandbox/nb1 ovsdb-server/compact
ovs-appctl -t $PWD/sandbox/sb1 ovsdb-server/compact
