#!/bin/bash

set -ev

if [ "$DEB_PACKAGE" ]; then
    # We're not using sparse for debian packages, tests are skipped and
    # all extra dependencies tracked by mk-build-deps.
    exit 0
fi

# Build and install sparse.
#
# Explicitly disable sparse support for llvm because some travis
# environments claim to have LLVM (llvm-config exists and works) but
# linking against it fails.
# Disabling sqlite support because sindex build fails and we don't
# really need this utility being installed.
git clone git://git.kernel.org/pub/scm/devel/sparse/sparse.git
cd sparse && make -j4 HAVE_LLVM= HAVE_SQLITE= install && cd ..

# Installing wheel separately because it may be needed to build some
# of the packages during dependency backtracking and pip >= 22.0 will
# abort backtracking on build failures:
#     https://github.com/pypa/pip/issues/10655
pip3 install --disable-pip-version-check --user wheel
pip3 install --disable-pip-version-check --user \
    flake8 'hacking>=3.0' sphinx setuptools pyelftools pyOpenSSL
