..
      Licensed under the Apache License, Version 2.0 (the "License"); you may
      not use this file except in compliance with the License. You may obtain
      a copy of the License at

          http://www.apache.org/licenses/LICENSE-2.0

      Unless required by applicable law or agreed to in writing, software
      distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
      WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
      License for the specific language governing permissions and limitations
      under the License.

      Convention for heading levels in OVN documentation:

      =======  Heading 0 (reserved for the title in a document)
      -------  Heading 1
      ~~~~~~~  Heading 2
      +++++++  Heading 3
      '''''''  Heading 4

      Avoid deeper levels because they do not render well.

=======================================
Dynamic Routing Configuration and Usage
=======================================

Introduction
------------

This guide covers some common examples of how to configure OVN's IP
route exchange feature with an external routing daemon such as FRR
(Free Range Routing).  For the underlying architecture and data flow
details, see :doc:`/topics/dynamic-routing/architecture`.

Prerequisites
-------------

Before configuring dynamic routing, ensure the following are in place:

- A running OVN deployment.

- FRR (or another routing daemon) installed on each chassis that will
  participate in dynamic routing.

IP Route Exchange
-----------------

IP route exchange allows OVN to advertise logical network prefixes to
external routing peers and learn external routes back into the OVN
logical topology.

Enabling Dynamic Routing on a Logical Router
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Enable dynamic routing by setting the ``dynamic-routing`` option on
the logical router::

    $ ovn-nbctl set Logical_Router lr1 options:dynamic-routing=true

This alone does not advertise any routes.  You must also configure
which route types to redistribute (see below).

Configuring Route Redistribution
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``dynamic-routing-redistribute`` option controls which routes OVN
advertises to the external fabric.  It accepts a comma-separated list
of the following values:

``connected``
    Subnet prefixes from networks attached to the router's ports
    (e.g., ``10.0.1.0/24`` for a port with address ``10.0.1.1/24``).

``connected-as-host``
    Individual host routes (``/32`` for IPv4, ``/128`` for IPv6) for
    each actively used IP on the connected networks.  This includes
    logical switch port addresses, router port addresses, and NAT
    entries of routers connected directly or via a shared logical
    switch.  Replaces the subnet prefix routes that ``connected``
    would advertise with per-IP host routes.  Useful for steering
    traffic to the chassis that hosts a specific workload.

``static``
    All ``Logical_Router_Static_Route`` entries on the router.

``nat``
    The external IP of each NAT rule on this router and its
    neighboring routers (routers connected directly or via a shared
    logical switch).

``lb``
    The VIP address of each load balancer on this router and its
    neighboring routers.

``hub-spoke``
    Routes learned through OVN Interconnection (OVN-IC) from other
    routers, enabling hub-and-spoke propagation.

Set redistribution on the router::

    $ ovn-nbctl set Logical_Router lr1 \
        options:dynamic-routing-redistribute=connected,static,nat,lb

Per-port overrides are also supported.  Setting
``dynamic-routing-redistribute`` on a ``Logical_Router_Port``
overrides the router-level setting for routes associated with that
port::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:dynamic-routing-redistribute=connected,nat

If no per-port value is set, the router-level setting applies.

VRF Configuration
~~~~~~~~~~~~~~~~~

Each dynamic-routing-enabled logical router is associated with a
Linux VRF on each chassis.  ``ovn-controller`` and the routing daemon
exchange routes through this VRF's routing table.

**VRF Table ID.**
The VRF table ID is determined by:

1. The ``dynamic-routing-vrf-id`` option, if set to a valid integer
   (1--4294967295; IDs 253--255 are reserved by the Linux kernel
   and should be avoided)::

       $ ovn-nbctl set Logical_Router lr1 \
           options:dynamic-routing-vrf-id=100

2. Otherwise, the datapath tunnel key of the logical router is used.

**VRF Lifecycle.**
By default, ``ovn-controller`` expects the VRF associated with a
logical router port to already exist on the chassis, managed by
external tooling.  To have ``ovn-controller`` create and delete the
VRF automatically, set ``dynamic-routing-maintain-vrf`` on the
logical router port that is bound to the chassis::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:dynamic-routing-maintain-vrf=true

When set, ``ovn-controller`` creates the VRF when the port is bound
and deletes it when the port is unbound or dynamic routing is
disabled.

**VRF Name.**
The VRF interface name defaults to ``ovnvrf`` followed by the table
ID (e.g., ``ovnvrf100``).  Override it with
``dynamic-routing-vrf-name``::

    $ ovn-nbctl set Logical_Router lr1 \
        options:dynamic-routing-vrf-name=myvrf

The name must be a valid Linux network interface name (at most 15
characters).

Route Nexthop Configuration
~~~~~~~~~~~~~~~~~~~~~~~~~~~

By default, advertised routes are installed into the VRF as
**blackhole** routes.  This attracts traffic into OVN for processing
without requiring a specific nexthop address.

To install routes with a gateway nexthop instead (useful when the
routing daemon requires a resolvable nexthop), set::

    $ ovn-nbctl set Logical_Router lr1 \
        options:dynamic-routing-v4-prefix-nexthop=192.168.1.1

    $ ovn-nbctl set Logical_Router lr1 \
        options:dynamic-routing-v6-prefix-nexthop=fd00::1

The ``dynamic-routing-v4-prefix-nexthop`` option accepts either an
IPv4 or IPv6 address; an IPv6 nexthop for IPv4 prefixes enables
RFC 5549-style routing where IPv4 traffic is forwarded over an
IPv6-only peering link.  The ``dynamic-routing-v6-prefix-nexthop``
option accepts only IPv6 addresses.

Route Advertisement Locality
~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When an ``Advertised_Route`` record has a ``tracked_port`` set,
``ovn-controller`` adjusts the route metric based on whether the
tracked port is locally bound.  Routes for locally bound ports
receive a higher priority (lower metric), causing the routing daemon
to prefer the chassis hosting the workload.

The ``dynamic-routing-redistribute-local-only`` option further
restricts advertisement: when set to ``true``, ``ovn-controller``
only installs routes on the chassis where the ``tracked_port`` is
locally bound.  Other chassis do not advertise the route at all::

    $ ovn-nbctl set Logical_Router lr1 \
        options:dynamic-routing-redistribute-local-only=true

This is particularly useful with ``connected-as-host`` to ensure
host routes are only announced from the chassis that owns the
workload.

This option can also be set per port on the
``Logical_Router_Port``::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:dynamic-routing-redistribute-local-only=true

Route Learning
~~~~~~~~~~~~~~

``ovn-controller`` monitors VRF routing tables for routes installed
by the external routing daemon.  Routes with a protocol value above
``RTPROT_STATIC`` (i.e., routes from dynamic routing protocols like
BGP) are learned and recorded as ``Learned_Route`` entries in the
Southbound database.  Routes installed by ``ovn-controller`` itself
(protocol ``RTPROT_OVN``) are excluded from learning.
``ovn-northd`` then generates logical flows so that the logical
router forwards traffic for these learned prefixes.

Learned routes receive lower priority than static routes, ensuring
explicitly configured routes always take precedence.

To disable route learning on a router::

    $ ovn-nbctl set Logical_Router lr1 \
        options:dynamic-routing-no-learning=true

To disable learning on a specific port (overrides the router-level
setting)::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:dynamic-routing-no-learning=true

Routing Protocol Redirect
~~~~~~~~~~~~~~~~~~~~~~~~~

**Note:** Routing protocol redirect is entirely optional.  OVN's
dynamic routing works with any control plane configuration.  The
routing daemon can establish its BGP or BFD sessions completely
outside of OVN --- for example, on a separate physical interface, a
loopback, or any other interface that is not managed by OVN.  Use
this feature only when you want the routing daemon to peer using the
logical router port's IP addresses through an OVN-managed logical
switch port.

OVN can redirect routing protocol control plane traffic (BGP, BFD)
from a logical router port to a logical switch port where an external
routing daemon is listening.  This allows FRR to peer as if it were
using the router port's IP addresses.

Configure on the logical router port::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:routing-protocols=BGP,BFD \
        options:routing-protocol-redirect=lsp-bgp

Where ``lsp-bgp`` is a logical switch port on the peer logical switch
that the routing daemon is bound to.  Supported protocols:

- ``BGP`` --- forwards TCP port 179
- ``BFD`` --- forwards UDP port 3784

For BGP unnumbered mode (advertising IPv4 routes over IPv6 with
automatic peer discovery), enable periodic IPv6 Router Advertisements
on the logical router port::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        ipv6_ra_configs:send_periodic=true \
        ipv6_ra_configs:address_mode=slaac \
        ipv6_ra_configs:max_interval=10 \
        ipv6_ra_configs:min_interval=5

The ``max_interval`` and ``min_interval`` values (in seconds)
influence how quickly the initial BGP session is established.

Port-to-Interface Mapping
~~~~~~~~~~~~~~~~~~~~~~~~~

When a chassis has multiple links toward the fabric, each running BGP
independently, ``dynamic-routing-port-name`` on a logical router port
restricts route learning to a specific Linux interface::

    $ ovn-nbctl set Logical_Router_Port lrp-fabric1 \
        options:dynamic-routing-port-name=lsp-fabric1

``ovn-controller`` resolves the port name to a Linux interface.  If
the referenced port is bound locally, the interface name is discovered
automatically.  Otherwise, configure an explicit mapping on the
``Open_vSwitch`` table::

    $ ovs-vsctl set Open_vSwitch . \
        external_ids:dynamic-routing-port-mapping=\
    lsp-fabric1=eth1,lsp-fabric2=eth2

Usage Examples
--------------

The following examples demonstrate complete configurations for common
deployment scenarios.

Example: Gateway Router with BGP
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A centralized gateway router pinned to a specific chassis, peering
with an external BGP speaker to advertise connected and static
routes.

::

    External BGP Peer (AS 65001)
              |
              | eBGP peering
              |
    +---------+---------+
    | Gateway Router    |  (lr-gw, pinned to chassis-1)
    | 10.0.0.1/24       |  dynamic-routing = true
    +---------+---------+
              |
    +---------+---------+
    | Logical Switch    |  (ls-internal)
    | 10.0.0.0/24       |
    +---------+---------+
              |
        VM1  VM2  VM3

**Create the logical topology.**
::

    $ ovn-nbctl lr-add lr-gw
    $ ovn-nbctl ls-add ls-internal
    $ ovn-nbctl ls-add ls-fabric

    # Router port toward the internal network.
    $ ovn-nbctl lrp-add lr-gw lrp-internal \
        00:00:00:00:00:01 10.0.0.1/24
    $ ovn-nbctl lsp-add-router-port ls-internal \
        lsp-internal-to-gw lrp-internal

    # Router port toward the fabric.
    $ ovn-nbctl lrp-add lr-gw lrp-fabric \
        00:00:00:00:00:02 192.168.1.1/24
    $ ovn-nbctl lsp-add-router-port ls-fabric \
        lsp-fabric-to-gw lrp-fabric

    # Localnet port for physical connectivity.
    $ ovn-nbctl lsp-add-localnet-port ls-fabric \
        lsp-fabric-ln physnet-fabric

**Pin the router to a chassis and enable dynamic routing.**
::

    $ ovn-nbctl set Logical_Router lr-gw \
        options:chassis=chassis-1 \
        options:dynamic-routing=true \
        options:dynamic-routing-redistribute=connected,static \
        options:dynamic-routing-vrf-id=100

**Configure VRF management and routing protocol redirect.**
::

    # Have ovn-controller manage the VRF lifecycle.
    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:dynamic-routing-maintain-vrf=true

    # Add a logical switch port for the BGP daemon.
    $ ovn-nbctl lsp-add ls-fabric lsp-bgp
    $ ovn-nbctl lsp-set-addresses lsp-bgp unknown

    # Redirect BGP traffic to the BGP daemon port.
    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        options:routing-protocols=BGP,BFD \
        options:routing-protocol-redirect=lsp-bgp

    # Enable periodic RAs for BGP unnumbered.
    $ ovn-nbctl set Logical_Router_Port lrp-fabric \
        ipv6_ra_configs:send_periodic=true \
        ipv6_ra_configs:address_mode=slaac \
        ipv6_ra_configs:max_interval=10 \
        ipv6_ra_configs:min_interval=5

**Bind the BGP interface on the chassis.**
::

    # Create an OVS internal port bound to the BGP LSP.
    $ ovs-vsctl add-port br-int ext0-bgp -- \
        set Interface ext0-bgp type=internal \
        external-ids:iface-id=lsp-bgp

    # Place the interface into the VRF.
    $ ip link set dev ext0-bgp master ovnvrf100
    $ ip link set dev ext0-bgp up

**Configure FRR on the chassis.**
::

    configure terminal

    vrf ovnvrf100
    exit-vrf

    router bgp 65000 vrf ovnvrf100
      bgp router-id 192.168.1.1
      neighbor ext0-bgp interface remote-as external
      address-family ipv4 unicast
        redistribute kernel
      exit-address-family
      address-family ipv6 unicast
        redistribute kernel
        neighbor ext0-bgp activate
      exit-address-family

Example: Distributed Router with Gateway Ports
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

A distributed logical router with a distributed gateway port,
advertising connected, NAT, and load balancer routes.  The
``local-only`` option ensures host routes are announced only from the
chassis hosting the workload.

::

                    External Network
                          |
    +---------------------+--------------------+
    |          Distributed LR                  |
    |          (lr-dist)                       |
    |          dynamic-routing = true          |
    |          NAT: 172.16.0.10 -> 10.0.1.10   |
    |          LB VIP: 172.16.0.100            |
    +------+----------+----------+-------------+
           |          |          |
    +------+----+ +---+------+ +-+---------+
    | LS-Fabric | | LS-A     | | LS-B      |
    |           | | 10.0.1/24| | 10.0.2/24 |
    +-----------+ +----+-----+ +-----+-----+
                       |             |
                     VM-A1         VM-B1
                  (chassis-1)   (chassis-2)

**Create the distributed router with a gateway port.**
::

    $ ovn-nbctl lr-add lr-dist
    $ ovn-nbctl ls-add ls-a
    $ ovn-nbctl ls-add ls-b
    $ ovn-nbctl ls-add ls-fabric

    # Internal ports.
    $ ovn-nbctl lrp-add lr-dist lrp-a \
        00:00:00:00:01:01 10.0.1.1/24
    $ ovn-nbctl lsp-add-router-port ls-a lsp-a-to-lr lrp-a

    $ ovn-nbctl lrp-add lr-dist lrp-b \
        00:00:00:00:01:02 10.0.2.1/24
    $ ovn-nbctl lsp-add-router-port ls-b lsp-b-to-lr lrp-b

    # Distributed gateway port.
    $ ovn-nbctl lrp-add lr-dist lrp-gw \
        00:00:00:00:02:01 172.16.0.1/24
    $ ovn-nbctl lsp-add-router-port ls-fabric \
        lsp-fabric-to-lr lrp-gw

    # Configure HA chassis group for the gateway port.
    $ ovn-nbctl ha-chassis-group-add ha-gw
    $ ovn-nbctl ha-chassis-group-add-chassis ha-gw \
        chassis-1 10
    $ ovn-nbctl ha-chassis-group-add-chassis ha-gw \
        chassis-2 5
    $ GRP=$(ovn-nbctl --bare --columns=_uuid \
        find HA_Chassis_Group name=ha-gw)
    $ ovn-nbctl set Logical_Router_Port lrp-gw \
        ha_chassis_group=$GRP

**Add NAT and load balancer.**
::

    $ ovn-nbctl lr-nat-add lr-dist dnat_and_snat \
        172.16.0.10 10.0.1.10
    $ ovn-nbctl lb-add lb-web 172.16.0.100:80 \
        10.0.1.10:8080,10.0.2.10:8080
    $ ovn-nbctl lr-lb-add lr-dist lb-web

**Enable dynamic routing with NAT and LB redistribution.**
::

    $ ovn-nbctl set Logical_Router lr-dist \
        options:dynamic-routing=true \
        options:dynamic-routing-redistribute=connected,nat,lb \
        options:dynamic-routing-vrf-id=200

    $ ovn-nbctl set Logical_Router_Port lrp-gw \
        options:dynamic-routing-maintain-vrf=true \
        options:dynamic-routing-redistribute-local-only=true

With ``local-only`` enabled, NAT and LB host routes are only
advertised from the chassis where the associated workload is bound,
ensuring optimal traffic forwarding.

**Set up routing protocol redirect and FRR** (same pattern as the
gateway router example above).

Verification and Troubleshooting
--------------------------------

Checking OVN State
~~~~~~~~~~~~~~~~~~

Verify that ``ovn-northd`` has populated the ``Advertised_Route``
table::

    $ ovn-sbctl list Advertised_Route

Check for learned routes from external peers::

    $ ovn-sbctl list Learned_Route

Checking the VRF and Kernel Routes
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Verify the VRF exists and is up::

    $ ip link show type vrf

List routes in the VRF table.  Routes marked ``proto ovn`` were
installed by ``ovn-controller``; routes with other protocol values
(e.g., ``proto bgp``) were learned from the routing daemon::

    $ ip route show table 100

Checking FRR State
~~~~~~~~~~~~~~~~~~

Verify BGP peering is established::

    $ vtysh -c "show bgp summary"

Check received and advertised routes::

    $ vtysh -c "show bgp ipv4 unicast"
    $ vtysh -c "show bgp ipv6 unicast"

Best Practices
--------------

**Use explicit VRF IDs.**
Set ``dynamic-routing-vrf-id`` rather than relying on the datapath
tunnel key.  Explicit IDs make the VRF configuration predictable and
easier to reference in FRR configurations and monitoring.

**Use local-only for host routes.**
When using ``connected-as-host`` redistribution, combine it with
``dynamic-routing-redistribute-local-only=true`` to ensure host
routes are only announced from the chassis that owns the workload.
This provides optimal traffic forwarding and avoids unnecessary
traffic tromboning.

**Let ovn-controller manage VRFs when possible.**
Set ``dynamic-routing-maintain-vrf=true`` on the logical router port
to let ``ovn-controller`` handle VRF creation and deletion.  This
simplifies chassis provisioning and ensures VRF lifecycle matches
port binding.

**Configure BFD for fast failure detection.**
Include ``BFD`` in the ``routing-protocols`` option and enable BFD
in FRR for sub-second failure detection.  This significantly improves
convergence time compared to relying on BGP hold timers alone.

**Use per-port redistribution for multi-homed setups.**
When a chassis has multiple fabric links, use per-port
``dynamic-routing-redistribute`` and ``dynamic-routing-port-name`` to
control exactly which routes are advertised and learned on each link.

See Also
--------

- :doc:`/topics/dynamic-routing/architecture` --- Architecture and
  internal design of the dynamic routing feature.

- ``ovn-nb``\(5) --- Full reference for all ``Logical_Router``
  and ``Logical_Router_Port`` dynamic routing options.

- ``ovn-sb``\(5) --- Documentation of the ``Advertised_Route``
  and ``Learned_Route`` tables.

- ``ovn-controller``\(8) --- Controller-side configuration options
  including ``dynamic-routing-port-mapping``.
