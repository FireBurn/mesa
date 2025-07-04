#!/usr/bin/env bash
# shellcheck disable=SC1091

# When changing this file, you need to bump the following
# .gitlab-ci/image-tags.yml tags:
# FEDORA_X86_64_BUILD_TAG

set -e

. .gitlab-ci/setup-test-env.sh

set -o xtrace


EPHEMERAL=(
        autoconf
        automake
        bzip2
        cmake
        git
        libtool
        "pkgconfig(epoxy)"
        "pkgconfig(gbm)"
        "pkgconfig(openssl)"
        python3-pip
        unzip
        xz
)

DEPS=(
    bison
    ccache
    clang-devel
    flex
    flatbuffers-compiler
    gcc
    gcc-c++
    gettext
    glslang
    kernel-headers
    llvm-devel
    ninja-build
    "pkgconfig(LLVMSPIRVLib)"
    "pkgconfig(SPIRV-Tools)"
    "pkgconfig(dri2proto)"
    "pkgconfig(expat)"
    "pkgconfig(flatbuffers)"
    "pkgconfig(glproto)"
    "pkgconfig(libclc)"
    "pkgconfig(libelf)"
    "pkgconfig(libglvnd)"
    "pkgconfig(libva)"
    "pkgconfig(pciaccess)"
    "pkgconfig(vdpau)"
    "pkgconfig(vulkan)"
    "pkgconfig(x11)"
    "pkgconfig(x11-xcb)"
    "pkgconfig(xcb)"
    "pkgconfig(xcb-dri2)"
    "pkgconfig(xcb-dri3)"
    "pkgconfig(xcb-glx)"
    "pkgconfig(xcb-present)"
    "pkgconfig(xcb-randr)"
    "pkgconfig(xcb-sync)"
    "pkgconfig(xcb-xfixes)"
    "pkgconfig(xdamage)"
    "pkgconfig(xext)"
    "pkgconfig(xfixes)"
    "pkgconfig(xrandr)"
    "pkgconfig(xshmfence)"
    "pkgconfig(xtensor)"
    "pkgconfig(xxf86vm)"
    "pkgconfig(zlib)"
    procps-ng
    python-unversioned-command
    python3-devel
    python3-mako
    python3-packaging
    python3-ply
    python3-pycparser
    python3-yaml
    rust-packaging
    spirv-tools-devel
    spirv-llvm-translator-devel
    vulkan-headers
    which
)

dnf install -y --setopt=install_weak_deps=False "${DEPS[@]}" "${EPHEMERAL[@]}"


. .gitlab-ci/container/container_pre_build.sh


# dependencies where we want a specific version
export              XORG_RELEASES=https://xorg.freedesktop.org/releases/individual

export         XORGMACROS_VERSION=util-macros-1.19.0

curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
    -O $XORG_RELEASES/util/$XORGMACROS_VERSION.tar.bz2
tar -xvf $XORGMACROS_VERSION.tar.bz2 && rm $XORGMACROS_VERSION.tar.bz2
cd $XORGMACROS_VERSION; ./configure; make install; cd ..
rm -rf $XORGMACROS_VERSION

. .gitlab-ci/container/install-meson.sh

. .gitlab-ci/container/build-bindgen.sh

. .gitlab-ci/container/build-mold.sh

. .gitlab-ci/container/build-libdrm.sh

. .gitlab-ci/container/build-wayland.sh


############### Uninstall the build software

dnf remove -y "${EPHEMERAL[@]}"

. .gitlab-ci/container/container_post_build.sh
