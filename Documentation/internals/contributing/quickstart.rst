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

==============================================
Simple steps if you want to start from scratch
==============================================

This is a quick reference document on some steps a contributor could make
to ramp up more quickly with knowledge and unstanding of OVN

Understand the Architecture
---------------------------

Take a look at the overall architecture first.

High level Overview
~~~~~~~~~~~~~~~~~~~

This video is from 2016 but content is still relevant and it gives a quick
overview of the various components.

`OVN introduction <https://www.youtube.com/watch?v=q3cJ6ezPnCU>`_.


In depth architecture
~~~~~~~~~~~~~~~~~~~~~

You may want to dive deeper in the architecture after.

Look at
`OVN architecture <http://www.openvswitch.org/support/dist-docs/ovn-architecture.7.pdf>`_.

or from ovn repo root:

::

  nroff -man ovn-architecture.7 | less

Ovsdb
~~~~~

The role of ovsdb is central for any activity on ovn. Understand at least the data model,
the schemas, and the monitoring.

`OVSDB documentation <http://docs.openvswitch.org/en/latest/ref/ovsdb.7/>`_.


Set a goal for yourself
-----------------------

Before proceeding further it may be important to set an achievable goal
to give a direction to your study.
Join the weekly meeting, present yourself and ask for things to do


Study the code
--------------

Knowing the overall architecture it's not difficult to map the components
to the source in the tree, but there may be some abbreviations not immediately familiar.

Glossary
~~~~~~~~

:sbrec/nbrec:
  record on southbound/northbound database

:ic:
  interconnection controller

:idl:
   Interface Definition Language. Structures related to the idl help
   maintain in-memory replicas of OVSDB instances.

:hmap/smap:
   Hash map implementations that ... ???

Data Structures
~~~~~~~~~~~~~~~

Keep a tab opened to data structures implementation, and keep in mind that many may
still be imported from ovs repo.

An example of flow with reference to the code
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

How a change in the nb database from the cloud plugin becomes a set of
rules in the ovs switch ?

1. The northd receives notification of a record from northbound database

  1.1 northd/ovs_northd.c:??? handles the notification.

2. ???
3. ???



Hands-on
--------

Quick Start
~~~~~~~~~~~

::

  git clone  https://github.com/openvswitch/ovs
  pushd ovs
  ./boot.sh ; ./configure ; make
  popd

  git clone https://github.com/ovn-org/ovn
  ./boot.sh ; ./configure --with-ovs-source=../ovs ; make
  popd


Playgrounds
~~~~~~~~~~~

To do some experiments, there are a few method at disposal:

A simple non isolated OVS bridge controlled by OVN can be easily created with

::

  pushd ovn
  make sandbox


to create a larger playground wtih multiple bridges in multiple nodes, 
look at `OVN fake multinode <https://github.com/ovn-org/ovn-fake-multinode>`_. repo

Tests
------

The test suite is run with the help of autotest.

Types of tests
~~~~~~~~~~~~~~

:component:
  WIP

:end to end:
  WIP

Create a test
~~~~~~~~~~~~~

1. ???
2. Ensure description is present in at_help_all var in tests/testsuite
3. ???

Branching Model
---------------

WIP

Your first submission
---------------------

Before submitting your patch, review  :doc:`internals/contributin/submitting-patches`

:: 
  
  git checkout -b my-first-patch


Modify the code. then.

::

  git commit -a --signoff
  git format-patch master --prefix "[PATCH ovn - my first patch]"

edit the .patch file with tags.

::

  git send-email --smtp-server=your.smtp.server *.patch


