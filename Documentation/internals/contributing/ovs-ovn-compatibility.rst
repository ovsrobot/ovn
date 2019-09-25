..
      Copyright (c) 2019 Red Hat, Inc.

      Licensed under the Apache License, Version 2.0 (the "License"); you may
      not use this file except in compliance with the License. You may obtain
      a copy of the License at

          http://www.apache.org/licenses/LICENSE-2.0

      Unless required by applicable law or agreed to in writing, software
      distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
      WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
      License for the specific language governing permissions and limitations
      under the License.

      Convention for heading levels in Open vSwitch documentation:

      =======  Heading 0 (reserved for the title in a document)
      -------  Heading 1
      ~~~~~~~  Heading 2
      +++++++  Heading 3
      '''''''  Heading 4

      Avoid deeper levels because they do not render well.

===============================
Keeping OVN Compatible with OVS
===============================

This document discusses the general policy of compatibility between OVS and OVN.
If you are looking for a listing of version compatiblity, please see
:doc:`topics/ovs-compatibility`

OVN has split from OVS. Prior to this split, there were few issues with
compatibility. All code changes happened at the same time and in the same repo.
New releases contained the latest OVS and OVN changes. The most commonon time
that  compatibility typically came into question was during an upgrade; there
might be a brief period where mismatched versions of OVS and OVN are running.
That situation was transient.

With OVN and OVS being developed and versioned separately, previous assumptions
regarding compatibility are no longer valid. It is valid to permanently run a
version of OVN that is different from the concurrent version of OVS.

OVS and OVN versions
--------------------

Prior to the split, the release schedule for OVN was bound to the release
schedule for OVS. OVS releases a new version approximately every 6 months. OVN
has not yet determined a release schedule, but it is entirely possible that it
will be different from OVS. Eventually, this will lead to a situation where it
is very important that we publish which versions of OVN are compatible with
which versions of OVS. When incompatibilities are discovered, it is important to
ensure that these are clearly stated.

The split of OVS and OVN happened in the run-up to the release of OVS 2.12. As a
result, all versions of OVN *must* be compiled against OVS version 2.12 or
later. Before going further into compatibility, let's explore the ways that OVN
and OVS can become incompatible.

Compile-time Incompatibility
----------------------------

The first way that the projects can become incompatible is if the C code for OVN
no longer can compile.

The most likely case for this would be that an OVN change requires a parallel
change to OVS. Those keeping up to date with OVN but not OVS will find that OVN
will no longer compile since it refers to a nonexistent function or out of date
function in OVS.

Most OVN users will consume OVN via package from their distribution of choice.
OVN consumes libopenvswitch statically, so even if the version of OVS installed
on a user's machine is incompatible at compile time, it will not matter.

OVN developers are the only ones that would be inconvenienced by a compile-time
incompatibility. OVN developers will be expected to regularly update the version
of OVS they are using. If an OVN developer notices that OVN is not compiling,
then they should update their OVS code to the latest and try again.

Developers who are making changes to both OVS and OVN at the same time *must*
contribute the OVS change first and ensure it is merged upstream before
submitting the OVN change. This way, OVN should never be in a state where it
will not compile.

The other aspect to consider is compiling an older release of OVN against a
newer release of OVS. In practice, this is not typically something that someone
would want to do. However, if it is desired for some reason, then the ABI and
API guarantees for OVS's libraries should allow for OVN to compile properly. An
exception to this may be if OVS changes major versions. When OVS changes major
versions, they reserve the right to make breaking changes to the API and ABI.

Runtime Incompatibility
-----------------------

The next way that the projects may become incompatible is at runtime. The most
common way this would happen is if new OpenFlow capabilities are added to OVS as
part of an OVN change. In this case, if someone updates OVN but does not also
update OVS, then OVN will not be able to install the OpenFlow rules it wishes
to.

Unlike with compile-time incompatibilities, we can't wallpaper over the fact
that the OVS installation is not up to date. The best we can do is make it very
clear at runtime that a certain feature is not present, and if the feature is
desired, OVS must be upgraded.

The following is the process that OVN developers should use when making a
runtime compatibility change to OVS and OVN.

1. Submit the change to OVS first. See the change through until it is merged.
2. Make the necessary changes to OVN. 

  a. At startup, probe OVS for the existence of the OpenFlow addition. If it
     is not present, then output an informational message to the logs that 
     explains which OVN feature(s) cannot be used.
  b. If a user attempts to explicitly configure the feature that is not usable
     due to the incompatibility, then output a warning message to the logs. If
     the lack of the feature can be detected from a utility (such as ovn-nbctl
     or ovn-sbctl), then the utility should return an error and print to the
     console why the feature cannot be used.
  c. Ensure that the code that installs the OpenFlow will only do so if the new
     feature is present.

Compatibility Statement
-----------------------

Given the above, the OVN team will try its hardest to maintain any released
version of OVN with any released version of OVS after version 2.12. Versions of
OVS prior to 2.12 are not guaranteed to run properly since OVN does not have
appropriate OpenFlow feature probes in place.

It may seem prudent to only guarantee compatibility with certain releases of
OVS (e.g. the current and previous versions of OVS). However, dropping
compatibility would involve actively removing code that ensures runtime safety.
It seems unwise to do so.

This, however, is a "best effort" policy. The OVN project reserves the right to
withdraw compatibility support with a previous OVS version, for reasons such as:

- Security risks.
- Earthshatteringly large changes in OVS (e.g. no longer using OpenFlow or the
  OVSDB). This likely would coincide with a change in the OVS major version
  number.
- Difficulty in safely maintaining compatibility across versions.

In the event that compatibility for a certain version or versions of OVS is
dropped, the OVN project will clearly document it.

At some point, the compatibility matrix between OVS and OVN will likely become
too complex to explicitly test every version of OVN against every version of
OVS. Therefore, we will maintain a list of "tested" and "untested" compabitility
options for OVS and OVN. "untested" combinations *should* work, but since they
have not had explicit testing done, they cannot be guaranteed.
