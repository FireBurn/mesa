#!/bin/bash

# Note that this script is not actually "building" rust, but build- is the
# convention for the shared helpers for putting stuff in our containers.

set -ex

section_start rust "Building Rust toolchain"

# Pick a specific snapshot from rustup so the compiler doesn't drift on us.
RUST_VERSION=1.81.0-2024-09-05

# For rust in Mesa, we use rustup to install.  This lets us pick an arbitrary
# version of the compiler, rather than whatever the container's Debian comes
# with.
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
    --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- \
      --default-toolchain $RUST_VERSION \
      --profile minimal \
      -y

# Make rustup tools available in the PATH environment variable
# shellcheck disable=SC1091
. "$HOME/.cargo/env"

rustup component add clippy rustfmt

# Set up a config script for cross compiling -- cargo needs your system cc for
# linking in cross builds, but doesn't know what you want to use for system cc.
cat > "$HOME/.cargo/config" <<EOF
[target.armv7-unknown-linux-gnueabihf]
linker = "arm-linux-gnueabihf-gcc"

[target.aarch64-unknown-linux-gnu]
linker = "aarch64-linux-gnu-gcc"
EOF

section_end rust
