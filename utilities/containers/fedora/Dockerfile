FROM quay.io/fedora/fedora:37

# Update distro, install packages and clean any possible leftovers
RUN dnf -y update \
    && \
    dnf -y install \
        autoconf \
        automake \
        checkpolicy \
        clang \
        curl \
        dhcp-server \
        ethtool \
        gcc \
        gcc-c++ \
        git \
        glibc-langpack-en \
        groff \
        iproute \
        iproute-tc \
        iputils \
        kernel-devel \
        libcap-ng-devel \
        libtool \
        net-tools \
        nmap-ncat \
        openssl \
        openssl-devel \
        procps-ng \
        python3-devel \
        python3-pip \
        rpmdevtools \
        rsync \
        selinux-policy-devel \
        tcpdump \
        unbound \
        unbound-devel \
        wget \
        which \
    && \
    dnf clean all

# Compile sparse from source
WORKDIR /workspace/sparse

RUN git clone git://git.kernel.org/pub/scm/devel/sparse/sparse.git /workspace/sparse \
    && \
    make -j4 PREFIX=/usr HAVE_LLVM= HAVE_SQLITE= install

WORKDIR /workspace

# Update and install pip dependencies
RUN python3 -m pip install --upgrade pip \
    && \
    python3 -m pip install wheel \
    && \
    python3 -m pip install flake8 'hacking>=3.0' sphinx setuptools pyelftools pyOpenSSL

CMD ["/usr/sbin/init"]
