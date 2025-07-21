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

================
Test Development
================

This document provides information relevant to writing tests for OVN. The
documentation for executing tests exists in :doc:`/topics/testing`.

OVN uses `Autotest <https://www.gnu.org/software/autoconf/manual/autoconf-2.68/html_node/Using-Autotest.html#Using-Autotest>`_
for generating its tests. The top-level of the test code is located in the
file ``tests/testsuite.at``. This file is expanded into a shell script that
runs all of the OVN tests. Please refer to the Autotest documentation linked
above for more information regarding Autotest, as the rest of this document
assumes a general understanding of it.

OVN Test Suite Overview
-----------------------

All test code is located in the ``tests/`` directory at the root of the OVN
repository. Generally, files suffixed with ``-macros.at`` contain macros and
shell functions to aid in writing tests. Files suffixed with just ``.at``
contain the code for running actual tests.

By convention macros are denoted with all uppercase letters, while functions
use lowercase letters.

The most used and important macros/functions are documented here. All of
these macros/functions are implicitly available from all test files when
running the testsuite via ``make check``.

Macros and Functions
--------------------

Verification
~~~~~~~~~~~~

check COMMAND...
++++++++++++++++

Function to run COMMAND and check that it succeeds without any output. This is
the primary function for actually running a test, as this function wraps
``AT_CHECK``.

OVN_CHECK_PACKETS([PCAP], [EXPECTED])
+++++++++++++++++++++++++++++++++++++

Macro to compare packets read from PCAP, in pcap format, to those read from
EXPECTED, which is a text file containing packets as hex strings, one per line.
If PCAP contains fewer packets than EXPECTED, it waits up to 10 seconds for
more packets to appear.

OVN_CHECK_PACKETS_CONTAIN([PCAP], [EXPECTED])
+++++++++++++++++++++++++++++++++++++++++++++

Macro to check packets read from PCAP contain data from EXPECTED.

check_uuid COMMAND
++++++++++++++++++

Function to run COMMAND and check that it does not print anything else than
uuid as output. It also fails if the output is empty.

Daemon/Sandbox Management
~~~~~~~~~~~~~~~~~~~~~~~~~

ovn_start [--backup-northd=none|paused] [AZ]
++++++++++++++++++++++++++++++++++++++++++++

Creates and initializes ovn-sb and ovn-nb databases and starts their
ovsdb-server instance, sets appropriate environment variables so that ovn-sbctl
and ovn-nbctl use them by default, and starts ovn-northd running against them.

Normally this starts only an active northd and no backup northd. The following
options are accepted to adjust that:

``--backup-northd``         Start a backup northd.
``--backup-northd=paused``  Start the backup northd in the paused state.
``--use-tcp-to-sb``         Use tcp to connect to sb.

ovn_attach NETWORK BRIDGE IP [MASKLEN] [ENCAP]
++++++++++++++++++++++++++++++++++++++++++++++

First, this function attaches BRIDGE to interconnection network NETWORK, just
like ``net_attach NETWORK BRIDGE``. Second, it configures (simulated) IP
address IP (with network mask length MASKLEN, which defaults to 24) on BRIDGE.
Finally, it configures the Open vSwitch database to work with OVN and starts
ovn-controller.

sim_add SANDBOX
+++++++++++++++

Function to start a new simulated Open vSwitch instance named SANDBOX. Files
related to the instance, such as logs, databases, sockets, and pidfiles, are
created in a subdirectory of the main test directory also named
SANDBOX. Afterward, the ``as`` command (see below) can be used to run Open
vSwitch utilities in the context of the new sandbox.

The new sandbox starts out without any bridges. Use ovs-vsctl in the context of
the new sandbox to create a bridge, e.g.::

    sim_add hv0           # Create sandbox hv0.
    as hv0                # Set hv0 as default sandbox.
    ovs-vsctl add-br br0  # Add bridge br0 inside hv0.

or::

     sim_add hv0
     as hv0 ovs-vsctl add-br br0

as [OVS_DIR] COMMAND
++++++++++++++++++++

``as $1`` sets the ``OVS_*DIR`` environment variables to point to $ovs_base/$1.

``as $1 COMMAND...`` sets those variables in a subshell and invokes COMMAND
there.

net_add NETWORK
+++++++++++++++

Function to create a new interconnection network named NETWORK.

wait_for_ports_up [PORT...]
+++++++++++++++++++++++++++

With arguments, this function waits for specified Logical_Switch_Ports to come
up. Without arguments, waits for all "plain" and router Logical_Switch_Ports to
come up.

DUMP_FLOWS(sbox, output_file)
+++++++++++++++++++++++++++++

Dump openflows to ``output_file`` for ``sbox``.

PARSE_LISTENING_PORT LOGFILE VARIABLE()
+++++++++++++++++++++++++++++++++++++++

Macro that parses the TCP or SSL/TLS port on which a server is listening from
LOGFILE, given that the server was told to listen on a kernel-chosen port, and
assigns the port number to shell VARIABLE. You should specify the listening
remote as ptcp:0:127.0.0.1 or pssl:0:127.0.0.1, or the equivalent with [::1]
instead of 127.0.0.1. Here's an example of how to use this with ovsdb-server::

    ovsdb-server --log-file --remote=ptcp:0:127.0.0.1 ...
    PARSE_LISTENING_PORT([ovsdb-server.log], [TCP_PORT])

Now $TCP_PORT holds the listening port.

OVN_POPULATE_ARP()
++++++++++++++++++

Macro to pre-populate the ARP tables of all of the OVN instances that have been
started with ```ovn_attach()``. That means that packets sent from one
hypervisor to another never get dropped or delayed by ARP resolution, which
makes testing easier.

OVS_VSWITCHD_START([vsctl-args],[vsctl-output],[=override],[vswitchd-aux-args])
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Macro to creates a database and start ovsdb-server, start ovs-vswitchd
connected to that database, call ovs-vsctl to create a bridge named br0 with
predictable settings, passing 'vsctl-args' as additional commands to ovs-vsctl.
If 'vsctl-args' causes ovs-vsctl to provide output (e.g. because it includes
"create" commands) then 'vsctl-output' specifies the expected output after
filtering through uuidfilt.

If a test needs to use "system" devices (as dummies), then specify
``=override`` (literally) as the third argument. Otherwise, system devices
won't work at all (which makes sense because tests should not access a system's
real Ethernet devices).

OVS_VSWITCHD_STOP([ALLOWLIST])
++++++++++++++++++++++++++++++

Macro to gracefully stops ovs-vswitchd and ovsdb-server, checking their log
files for messages with severity WARN or higher and signaling an error if any
is present. The optional ALLOWLIST may contain shell-quoted "sed" commands to
delete any warnings that are actually expected, e.g.::

    OVS_VSWITCHD_STOP(["/expected error/d"])

OVS_APP_EXIT_AND_WAIT(DAEMON)
+++++++++++++++++++++++++++++

Ask the daemon named DAEMON to exit, via ``ovs-appctl``, and then wait for it
to exit.

Macro to dump openflows to ``output_file`` for ``sbox``.

OVN_CLEANUP(sim [, sim ...])
++++++++++++++++++++++++++++

Macro to gracefully terminate all OVN daemons, including those in specified
sandbox instances.

OVN_CLEANUP_SBOX(sbox)
++++++++++++++++++++++

Macro to gracefully terminate OVN daemons in the specified sandbox instance.
The sandbox name ``vtep`` is treated as a special case, and is assumed to have
ovn-controller-vtep and ovs-vtep daemons running instead of ovn-controller.

OVN_CLEANUP_CONTROLLER(sbox)
++++++++++++++++++++++++++++

Macro to gracefully terminate ovn-controller in the specified sandbox
instance. The sandbox name ``vtep`` is treated as a special case, and is
assumed to have ovn-controller-vtep and ovs-vtep daemons running instead of
ovn-controller.

OVN_CLEANUP_IC([az ...])
++++++++++++++++++++++++

Macro to gracefully terminate all interconnection DBs and daemons in the
specified AZs, if any.

Test Management
~~~~~~~~~~~~~~~

OVN_FOR_EACH_NORTHD(TEST)
+++++++++++++++++++++++++

Macro to wrap an arbitrary TEST. Defines versions of the TEST with all
combinations of northd, parallelization enabled and conditional monitoring
on/off. Normally the first statement in TEST is a call to ``AT_SETUP``.

OVN_FOR_EACH_NORTHD_NO_HV(TEST)
+++++++++++++++++++++++++++++++

Macro to wrap an arbitrary TEST. Defines versions of the TEST with all
combinations of northd and parallelization enabled. To be used when the
ovn-controller configuration is not relevant. Normally the first statement in
TEST is a call to ``AT_SETUP``.

on_exit COMMAND
+++++++++++++++

Function to add the shell COMMAND to a collection that is executed when the
current test completes, as a cleanup action. The most common use is to kill a
daemon started by the test. This is important to prevent tests that start
daemons from hanging at exit.

Cleanup commands are executed in the reverse order of calls to this function.

OVN_NBCTL(NBCTL_COMMAND)
++++++++++++++++++++++++

Macro to add NBCTL_COMMAND to list of commands to be run by the
``RUN_OVN_NBCTL`` macro.

RUN_OVN_NBCTL()
+++++++++++++++

Macro to execute a list of commands built by the ``OVN_NBCTL`` macro.

OVS_VSCTL(VSCTL_COMMAND)
++++++++++++++++++++++++

Macro to add VSCTL_COMMAND to list of commands to be run by ``RUN_OVS_VSCTL``.

RUN_OVS_VSCTL()
+++++++++++++++

Macro to execute the list of commands built by the ``OVS_VSCTL`` macro.

ovs_setenv
++++++++++

With no parameter or an empty parameter, this function sets the OVS_*DIR
environment variables to point to $ovs_base, the base directory in which the
test is running.

With a parameter, sets them to $ovs_base/$1.

OVS_WAIT_FOR_OUTPUT(COMMAND, EXIT-STATUS, STDOUT, STDERR)
+++++++++++++++++++++++++++++++++++++++++++++++++++++++++

Executes shell COMMAND in a loop until it exits with status EXIT-STATUS, prints
STDOUT on stdout, and prints STDERR on stderr.  If this doesn't happen within a
reasonable time limit, then the test fails.

There is a ``OVS_WAIT_FOR_OUTPUT_UNQUOTED`` version of this macro that expands
shell ``$variables``, ``$(command)``, and so on.  The plain version does not

OVS_WAIT_UNTIL(COMMAND[, IF-FAILED])
++++++++++++++++++++++++++++++++++++

Macro that executes shell COMMAND in a loop until it returns zero return code.
If COMMAND does not return zero code within reasonable time limit, then the
test fails. In that case, runs IF-FAILED before exiting.

OVS_WAIT_WHILE(COMMAND[, IF-FAILED])
++++++++++++++++++++++++++++++++++++

Macro that executes shell COMMAND in a loop until it returns non-zero return
code. If COMMAND does not return non-zero code within reasonable time limit,
then the test fails. In that case, runs IF-FAILED before exiting.

OVS_WAIT_UNTIL_EQUAL(COMMAND, OUTPUT)
+++++++++++++++++++++++++++++++++++++

Macro that executes shell COMMAND in a loop until it returns zero and the
output equals OUTPUT. If COMMAND does not return zero or a desired output
within a reasonable time limit, fails the test.
