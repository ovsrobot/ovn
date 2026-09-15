=======
ovn-sim
=======

Synopsis
========

``ovn-sim`` [*option*]... [*script*]...

Description
===========

``ovn-sim`` is a wrapper around ``ovs-sim`` that adds commands for
simulating OVN.

``ovs-sim`` provides a convenient environment for running one or more Open
vSwitch instances and related software in a sandboxed simulation environment.

To use ``ovn-sim``, first build Open vSwitch, then invoke it directly from the
build directory, e.g.::

    git clone https://github.com/openvswitch/ovs.git
    cd ovs
    ./boot.sh && ./configure && make
    cd ..
    git clone https://github.com/ovn-org/ovn.git
    cd ovn
    ./boot.sh && ./configure --with-ovs-source=${PWD}/../ovs
    make
    utilities/ovn-sim

On startup, ``ovn-sim`` checks that both Open vSwitch and OVN have been
built.  It then performs the normal ``ovs-sim`` initialization: it removes
any existing ``sandbox`` directory in the current directory, creates a new
one, installs the built Open vSwitch man pages there, and starts a simulated
Open vSwitch instance named ``main``.  The simulation does not require
superuser privileges and should not normally be run with them.

See ``ovs-sim``\(1) for more information about the sandbox and the Open
vSwitch commands available within it.  For example, run ``man ovs-sim`` from
an interactive ``ovn-sim`` shell.

Command-line Options
====================

*script*
    Sources *script* into the simulator's Bash shell after initializing the
    sandbox.  Multiple scripts run in command-line order, and state changes
    made by one script are visible to subsequent scripts.  If a script fails,
    ``ovn-sim`` exits immediately with the same status.

``-i`` or ``--interactive``
    Starts an interactive Bash shell after running any scripts.  An
    interactive shell is also started when no scripts are specified.  Without
    this option, ``ovn-sim`` exits after the specified scripts finish.

``-h`` or ``--help``
    Prints a brief usage message and exits.

Commands
========

Scripts and interactive sessions can use all commands documented by
``ovs-sim``\(1), including ``sim_add``, ``as``, ``net_add``, and
``net_attach``.  They can also use the following OVN-specific commands.  The
commands are exported Bash functions, so they are available in scripts.

OVN Commands
------------

These commands interact with OVN, the Open Virtual Network.

``ovn_start`` [*options*]
    Creates and initializes the central OVN databases (both
    ``ovn-sb``\(5) and ``ovn-nb``\(5)), starts their ``ovsdb-server``
    instances, and starts ``ovn-northd``.  It also installs the built OVN man
    pages and configures ``ovn-nbctl`` and ``ovn-sbctl`` in the simulation to
    use these databases by default.  ``ovn_start`` may be run only once in a
    simulation.

    The following options are available:

       ``--nbdb-model`` *model*
           Uses *model* for the northbound database.  *model* may be
           ``standalone`` (the default), ``backup``, or ``clustered``.
           A standalone model starts one server, a backup model starts an
           active server and a backup server, and a clustered model starts
           the number selected by ``--nbdb-servers``.

       ``--nbdb-servers`` *n*
           Selects the clustered model and starts *n* northbound database
           servers.  *n* must be from 1 through 99.  The default for the
           clustered model is 3.

       ``--sbdb-model`` *model*
           Uses *model* for the southbound database.  *model* may be
           ``standalone`` (the default), ``backup``, or ``clustered``.
           A standalone model starts one server, a backup model starts an
           active server and a backup server, and a clustered model starts
           the number selected by ``--sbdb-servers``.

       ``--sbdb-servers`` *n*
           Selects the clustered model and starts *n* southbound database
           servers.  *n* must be from 1 through 99.  The default for the
           clustered model is 3.

       ``-h`` or ``--help``
           Prints usage information for ``ovn_start``.

``ovn_attach`` *network* *bridge* *ip* [*masklen*]
    Attaches *bridge* in the default sandbox to interconnection network
    *network*, as with ``net_attach`` *network* *bridge*.  It configures the
    simulated IPv4 address *ip* on *bridge*, with prefix length *masklen*,
    which defaults to 24.  IPv6 addresses are not supported.

    The command configures the sandbox to use the southbound database,
    configures Geneve encapsulation with *ip* as the encapsulation address,
    creates ``br-int``, and starts ``ovn-controller``.  Run ``ovn_start`` and
    ``net_add`` first, then use ``sim_add`` and ``ovs-vsctl`` to create the
    sandbox and *bridge*.  The default sandbox must not be ``main``.

    ``ovn_attach --help`` prints usage information for ``ovn_attach``.

``ovn_as`` *sandbox* [*command* [*arg*]...]
    Selects *sandbox* for both OVN and Open vSwitch commands.  Without a
    *command*, it changes the default sandbox for subsequent commands.  With
    a *command*, it runs that command in the selected sandbox, as with
    ``as`` *sandbox* *command* *arg*..., and leaves the default Open vSwitch
    target unchanged.  The OVN directory selection remains in effect.  This
    is useful for commands such as ``ovn-appctl`` that use OVN runtime
    directories.

Examples
========

The following example creates two simulated hypervisors, starts an
``ovn-controller`` on each one with ``ovn_attach``, and adds one logical port
per hypervisor::

    ovn_start
    ovn-nbctl ls-add lsw0
    net_add n1
    for i in 0 1; do
        sim_add hv$i
        ovn_as hv$i
        ovs-vsctl add-br br-phys
        ovn_attach n1 br-phys 192.168.0.$((i + 1))
        ovs-vsctl add-port br-int vif$i -- \
            set Interface vif$i external-ids:iface-id=lp$i
        ovn-nbctl lsp-add lsw0 lp$i
        ovn-nbctl lsp-set-addresses lp$i f0:00:00:00:00:0$i
    done

The following primitive scale test creates a clustered southbound database
and 200 hypervisors.  Adjust the scale by changing ``n`` in the first line::

    n=200; export n
    ovn_start --sbdb-model=clustered
    net_add n1
    ovn-nbctl ls-add br0
    for i in $(seq "$n"); do
        (sim_add hv$i
        ovn_as hv$i
        ovs-vsctl add-br br-phys
        y=$((i / 256))
        x=$((i % 256))
        ovn_attach n1 br-phys 192.168.$y.$x
        ovs-vsctl add-port br-int vif$i -- \
            set Interface vif$i external-ids:iface-id=lp$i) &
        case $i in
            *50|*00) echo $i; wait ;;
        esac
    done
    wait
    for i in $(seq "$n"); do
        yy=$(printf %02x $((i / 256)))
        xx=$(printf %02x $((i % 256)))
        ovn-nbctl lsp-add br0 lp$i
        ovn-nbctl lsp-set-addresses lp$i f0:00:00:00:$yy:$xx
    done

When the scale test has finished initializing, the following command shows
logical ports that are not yet up::

    watch 'for i in $(seq "$n"); do \
    if test "$(ovn-nbctl lsp-get-up lp$i)" != up; then echo $i; fi; done'
