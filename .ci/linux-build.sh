#!/bin/bash

set -o errexit
set -x

COMMON_CFLAGS=""
OVN_CFLAGS=""
EXTRA_OPTS="--enable-Werror"

function configure_ovs()
{
    pushd ovs
    ./boot.sh && ./configure CFLAGS="${COMMON_CFLAGS}" $* || \
    { cat config.log; exit 1; }
    make -j4 || { cat config.log; exit 1; }
    popd
}

function configure_ovn()
{
    configure_ovs $*
    ./boot.sh && ./configure CFLAGS="${COMMON_CFLAGS} ${OVN_CFLAGS}" $* || \
    { cat config.log; exit 1; }
}

save_OPTS="${OPTS} $*"
OPTS="${EXTRA_OPTS} ${save_OPTS}"

# If AddressSanitizer and UndefinedBehaviorSanitizer are requested, enable them,
# but only for OVN, not for OVS.  However, disable some optimizations for
# OVS, to make sanitizer reports user friendly.
if [ "$SANITIZERS" ]; then
    # Use the default options configured in tests/atlocal.in, in UBSAN_OPTIONS.
    COMMON_CFLAGS="${COMMON_CFLAGS} -O1 -fno-omit-frame-pointer -fno-common -g"
    OVN_CFLAGS="${OVN_CFLAGS} -fsanitize=address,undefined"
fi

if [ "$CC" = "clang" ]; then
    COMMON_CFLAGS="${COMMON_CFLAGS} -Wno-error=unused-command-line-argument"
elif [ "$M32" ]; then
    # Not using sparse for 32bit builds on 64bit machine.
    # Adding m32 flag directly to CC to avoid any possible issues with API/ABI
    # difference on 'configure' and 'make' stages.
    export CC="$CC -m32"
else
    OPTS="$OPTS --enable-sparse"
fi

if [ "$TESTSUITE" ]; then
    if [ "$TESTSUITE" = "system-test" ]; then
        configure_ovn $OPTS
        make -j4 || { cat config.log; exit 1; }
        if ! sudo make -j4 check-kernel RECHECK=yes; then
            # system-kmod-testsuite.log is necessary for debugging.
            cat tests/system-kmod-testsuite.log
            exit 1
        fi
    else
        # 'distcheck' will reconfigure with required options.
        # Now we only need to prepare the Makefile without sparse-wrapped CC.
        configure_ovn

        export DISTCHECK_CONFIGURE_FLAGS="$OPTS"
        if ! make distcheck CFLAGS="${COMMON_CFLAGS} ${OVN_CFLAGS}" -j4 \
            TESTSUITEFLAGS="-j4" RECHECK=yes
        then
            # testsuite.log is necessary for debugging.
            cat */_build/sub/tests/testsuite.log
            exit 1
        fi
    fi
elif [ "$DEB_PACKAGE" ]; then
    configure_ovn
    make debian

    # There is a pending SRU to the Ubuntu Open vSwitch package that allows
    # building OVN 22.03.1 and onwards.  Let's use the Debian package until
    # it arrives.
    deb_ovs_pool=http://ftp.debian.org/debian/pool/main/o/openvswitch
    wget -O /tmp/openvswitch-source_2.17.2-3_all.deb \
        $deb_ovs_pool/openvswitch-source_2.17.2-3_all.deb
    sudo dpkg -i /tmp/openvswitch-source_2.17.2-3_all.deb

    mk-build-deps --install --root-cmd sudo --remove debian/control
    dpkg-checkbuilddeps
    make debian-deb
    packages=$(ls $(pwd)/../*.deb)
    deps=""
    for pkg in $packages; do
        _ifs=$IFS
        IFS=","
        for dep in $(dpkg-deb -f $pkg Depends); do
            dep_name=$(echo "$dep"|awk '{print$1}')
            # Don't install internal package inter-dependencies from apt
            echo $dep_name | grep -q ovn && continue
            deps+=" $dep_name"
        done
        IFS=$_ifs
    done
    # install package dependencies from apt
    echo $deps | xargs sudo apt -y install
    # install the locally built openvswitch packages
    sudo dpkg -i $packages
else
    configure_ovn $OPTS
    make -j4 || { cat config.log; exit 1; }
fi


exit 0
