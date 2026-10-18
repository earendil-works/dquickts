#!/bin/sh
# Regenerate the bundled bindings of rquickjs-sys (sys/src/bindings) from the
# quickjs.h of this tree. Needs cargo-zigbuild (zig provides the C headers of
# every target).
set -e

# The bindings are written before the engine is compiled: a target whose C
# build fails still gets them.
build_target() {
    echo "Generating for $1"
    rustup target add "$1"
    if ! RQUICKJS_SYS_NO_WASI_SDK=1 cargo zigbuild --manifest-path sys/Cargo.toml \
        --features=bindgen,update-bindings --target "$1"; then
        echo "warning: the engine does not build for $1"
    fi
}

copy_bindings() {
    echo "Copying bindings from $1 to $2"
    cp "sys/src/bindings/$1.rs" "sys/src/bindings/$2.rs"
}

# Generate bindings for unique size_t types and value representations only
build_target x86_64-unknown-linux-gnu    # c_ulong representative
build_target aarch64-apple-darwin        # __darwin_size_t representative
build_target x86_64-pc-windows-gnu       # c_ulonglong representative
build_target i686-unknown-linux-gnu      # c_uint, NaN boxing
build_target wasm32-wasip1
build_target armv7-unknown-linux-gnueabihf
build_target riscv64gc-unknown-linux-gnu

# Copy bindings for targets with the same types
copy_bindings aarch64-apple-darwin x86_64-apple-darwin
copy_bindings x86_64-unknown-linux-gnu aarch64-unknown-linux-gnu
copy_bindings x86_64-unknown-linux-gnu aarch64-unknown-linux-musl
copy_bindings x86_64-unknown-linux-gnu loongarch64-unknown-linux-gnu
copy_bindings x86_64-unknown-linux-gnu loongarch64-unknown-linux-musl
copy_bindings riscv64gc-unknown-linux-gnu riscv64a23-unknown-linux-gnu
copy_bindings wasm32-wasip1 wasm32-wasip2
copy_bindings x86_64-unknown-linux-gnu x86_64-unknown-linux-musl
copy_bindings armv7-unknown-linux-gnueabihf armv7-unknown-linux-gnueabi
copy_bindings x86_64-pc-windows-gnu x86_64-pc-windows-msvc
copy_bindings x86_64-pc-windows-gnu aarch64-pc-windows-msvc
