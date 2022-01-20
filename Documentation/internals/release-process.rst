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

===================
OVN Release Process
===================

This document describes the process ordinarily used for OVN development and
release.  Exceptions are sometimes necessary, so all of the statements here
should be taken as subject to change through rough consensus of OVN
contributors, obtained through public discussion on, e.g., ovs-dev or the
#openvswitch IRC channel.

Release Strategy
----------------

OVN feature development takes place on the "master" branch. Ordinarily, new
features are rebased against master and applied directly.  For features that
take significant development, sometimes it is more appropriate to merge a
separate branch into master; please discuss this on ovs-dev in advance.

The process of making a release has the following stages.  See `Release
Scheduling`_ for the timing of each stage:

1. "Soft freeze" of the master branch.

   During the freeze, we ask committers to refrain from applying patches that
   add new features unless those patches were already being publicly discussed
   and reviewed before the freeze began.  Bug fixes are welcome at any time.
   Please propose and discuss exceptions on ovs-dev.
 
2. Fork a release branch from master, named for the expected release number,
   e.g. "branch-2019.10" for the branch that will yield OVN 2019.10.x.

   Release branches are intended for testing and stabilization.  At this stage
   and in later stages, they should receive only bug fixes, not new features.
   Bug fixes applied to release branches should be backports of corresponding
   bug fixes to the master branch, except for bugs present only on release
   branches (which are rare in practice).

   At this stage, sometimes there can be exceptions to the rule that a release
   branch receives only bug fixes.  Like bug fixes, new features on release
   branches should be backports of the corresponding commits on the master
   branch.  Features to be added to release branches should be limited in scope
   and risk and discussed on ovs-dev before creating the branch.

3. When committers come to rough consensus that the release is ready, they
   release the .0 release on its branch, e.g. 2019.10.0 for branch-2019.10.  To
   make the actual release, a committer pushes a signed tag named, e.g.
   v2019.10.0, to the OVN repository, makes a release tarball available on
   openvswitch.org, and posts a release announcement to ovs-announce.

4. As bug fixes accumulate, or after important bugs or vulnerabilities are
   fixed, committers may make additional releases from a branch: 2019.10.1,
   2019.10.2, and so on.  The process is the same for these additional release
   as for a .0 release.

Long-term Support Releases
--------------------------

The OVN project will periodically designate a release as "long-term support" or
LTS for short. An LTS release has the distinction of being maintained for
longer than a standard release.

LTS releases will receive bug fixes until the point that another LTS is
released. At that point, the old LTS will receive an additional year of
critical and security fixes. Critical fixes are those that are required to
ensure basic operation (e.g. memory leak fixes, crash fixes). Security fixes
are those that address concerns about exploitable flaws in OVN and that have a
corresponding CVE report.

LTS releases are scheduled to be released once every two years. This means
that any given LTS will receive bug fix support for two years, followed by
one year of critical bug fixes and security fixes.


Release Numbering
-----------------

The version number on master should normally end in .90.  This indicates that
the OVN version is "almost" the next version to branch.

Forking master into branch-x.y requires two commits to master.  The first is
titled "Prepare for x.y.0" and increments the version number to x.y.  This is
the initial commit on branch-x.y.  The second is titled "Prepare for post-x.y.0
(x.y.90)" and increments the version number to x.y.90.

The version number on a release branch is x.y.z, where x is the current year, y
is the month of the release, and z is initially 0. Making a release requires two
commits.  The first is titled *Set release dates for x.y.z.* and updates NEWS
and debian/changelog to specify the release date of the new release.  This
commit is the one made into a tarball and tagged. The second is titled *Prepare
for x.y.(z+1).* and increments the version number and adds a blank item to NEWS
with an unspecified date.

Release Scheduling
------------------

OVN makes releases at the following three-month cadence.  All dates are
approximate:

+---------------+---------------------+--------------------------------------+
| Time (months) | Example Dates       | Stage                                |
+---------------+---------------------+--------------------------------------+
| T             | Dec 1, Mar 1, ...   | Begin x.y release cycle              |
+---------------+---------------------+--------------------------------------+
| T + 2         | Feb 1, May 1, ...   | "Soft freeze" master for x.y release |
+---------------+---------------------+--------------------------------------+
| T + 2.5       | Feb 15, May 15, ... | Fork branch-x.y from master          |
+---------------+---------------------+--------------------------------------+
| T + 3         | Mar 1, Jun 1, ...   | Release version x.y.0                |
+---------------+---------------------+--------------------------------------+

Release Calendar
----------------

The 2021 timetable is shown below. Note that these dates are not set in stone.
If extenuating circumstances arise, a release may be delayed from its target
date.

+---------+-------------+-----------------+---------+
| Release | Soft Freeze | Branch Creation | Release |
+---------+-------------+-----------------+---------+
| 21.03.0 | Feb 5       | Feb 19          | Mar 5   |
+---------+-------------+-----------------+---------+
| 21.06.0 | May 7       | May 21          | Jun 4   |
+---------+-------------+-----------------+---------+
| 21.09.0 | Aug 6       | Aug 20          | Sep 3   |
+---------+-------------+-----------------+---------+
| 21.12.0 | Nov 5       | Nov 19          | Dec 3   |
+---------+-------------+-----------------+---------+

Below is the 2022 timetable

+---------+-------------+-----------------+---------+
| Release | Soft Freeze | Branch Creation | Release |
+---------+-------------+-----------------+---------+
| 22.03.0 | Feb 4       | Feb 18          | Mar 4   |
+---------+-------------+-----------------+---------+
| 22.06.0 | May 6       | May 20          | Jun 3   |
+---------+-------------+-----------------+---------+
| 22.09.0 | Aug 5       | Aug 19          | Sep 2   |
+---------+-------------+-----------------+---------+
| 22.12.0 | Nov 4       | Nov 18          | Dec 2   |
+---------+-------------+-----------------+---------+

Contact
-------

Use dev@openvswitch.org to discuss the OVN development and release process.
