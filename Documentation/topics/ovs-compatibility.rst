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

=================================
Compatibility between OVN and OVS
=================================

This document lists versions of OVN and details their compatibility against
versions of OVS. This document will be updated as new versions of OVS and OVN
are released.

For a more detailed look into OVN and OVS compatibility, see
:doc:`internals/contributing/ovs-ovn-compatiblity`.

The following is a template for how a version might be documented:

::

    OVN version X.Y is 100% feature compatible with OVS versions A.B.C and higher.

    - OVN feature "foo" is unavailable in OVS versions A.(B-2).0 until version
      A.B.C.

    OVN version X.Y has been explicitly tested against

    - OVS version A.(B-3).0 - A.(B-3).4
    - OVS version A.(B-2).0 - A.(B-2).2
    - OVS version A.(B-1).0 - A.(B-1).2
    - OVS version A.B.0 - A.B.C
    - OVS master A.B.90

    OVN version X.Y has not been tested against OVS versions prior to A.(B-3).0,
    but it is expected to be able to run as far back as OVS version A.(B-6).0.

    OVN version X.Y is not runtime compatible with versions prior to A.(B-6).0.

OVN master (2.12.90)
--------------------

OVN master is 100% feature compatible with OVS versions 2.12.0 and higher.

OVN master (version 2.12.90) has been explicitly tested against

- OVS 2.12.0
- OVS master (2.12.90)

OVN master is not compatible with versions of OVS prior to 2.12.0.
