#!/usr/bin/env bash
set -euo pipefail

addon=${1:?Pass honk or oxidns}
recipe_dir=$(cd "$(dirname "$0")" && pwd)
output=${RUNNER_TEMP:?Run on a GitHub-hosted Ubuntu runner}/ace6-android
mkdir -p "$output"
work=$(mktemp -d "$RUNNER_TEMP/ace6-android.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
source_dir=$work/source
package=$work/package
mkdir "$package"

export CARGO_BUILD_JOBS=2
export CMAKE_BUILD_PARALLEL_LEVEL=2
export MAKEFLAGS=-j2

fetch_checked() {
    local url=$1 file=$2 checksum=$3 checker=$4
    curl --fail --location --retry 3 --silent --show-error "$url" -o "$file"
    printf '%s  %s\n' "$checksum" "$file" | "$checker" --check
}

check_arm64() {
    local binary=$1
    local headers
    headers=$(llvm-readelf-18 --file-header --program-headers "$binary")
    grep -Eq 'Machine:[[:space:]]+AArch64' <<< "$headers"
    printf '%s\n' "$headers" > "$binary.elf.txt"
}

build_oxidns() {
    local rust=1.95.0
    local ndk=r28c
    local ndk_revision=28.2.13676358
    local target=aarch64-linux-android
    local api=28
    local ndk_archive=$work/android-ndk-$ndk-linux.zip
    local toolchain

    fetch_checked "https://dl.google.com/android/repository/android-ndk-$ndk-linux.zip" "$ndk_archive" \
        a7b54a5de87fecd125a17d54f73c446199e72a64 sha1sum
    unzip -q "$ndk_archive" -d "$work"
    grep -Fx "Pkg.Revision = $ndk_revision" "$work/android-ndk-$ndk/source.properties"
    toolchain=$work/android-ndk-$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
    export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$toolchain/$target$api-clang
    export CC_aarch64_linux_android=$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER
    export AR_aarch64_linux_android=$toolchain/llvm-ar
    export CARGO_PROFILE_RELEASE_LTO=thin
    export CARGO_PROFILE_RELEASE_CODEGEN_UNITS=8

    rustup toolchain install "$rust" --profile minimal --target "$target"
    cargo "+$rust" build --locked --release --target "$target" --no-default-features --features standard
    install -m 755 "target/$target/release/oxidns" "$package/oxidns"
    check_arm64 "$package/oxidns"
    grep -Fq '/system/bin/linker64' "$package/oxidns.elf.txt"
    {
        printf 'target=%s\nandroid_api=%s\nndk_revision=%s\nfeatures=standard\n' "$target" "$api" "$ndk_revision"
        rustc "+$rust" --version
    } >> "$package/BUILD-INFO"
}

build_honk() {
    local rust=1.99.1
    local bpf_rust=nightly-2026-10-20
    local zig_version=0.16.0
    local bpf_linker_version=0.12.0
    local zig_archive=zig-x86_64-linux-$zig_version.tar.xz
    local bpf_archive=bpf-linker-x86_64-unknown-linux-musl.tar.zst
    local zig_root=$work/${zig_archive%.tar.xz}
    local target=aarch64-unknown-linux-musl
    local bpf_object=target/bpfel-unknown-none/release/honk-ebpf
    local bpf_linker

    fetch_checked "https://ziglang.org/download/$zig_version/$zig_archive" "$work/$zig_archive" \
        70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00 sha256sum
    tar -xf "$work/$zig_archive" -C "$work"
    fetch_checked "https://github.com/aya-rs/bpf-linker/releases/download/v$bpf_linker_version/$bpf_archive" \
        "$work/$bpf_archive" 6789717c8cb6a2a5d920f3b6bfac2bd7efc00f6228ecd149108a99699a75c679 sha256sum
    mkdir "$work/bpf-linker" "$work/bin"
    tar -xf "$work/$bpf_archive" -C "$work/bpf-linker"
    bpf_linker=$(find "$work/bpf-linker" -type f -name bpf-linker -print -quit)
    test -x "$bpf_linker"
    ln -s "$bpf_linker" "$work/bin/bpf-linker"
    export PATH="$zig_root:$work/bin:$PATH"
    export ZIG_LIB=$zig_root/lib
    export LIBCLANG_PATH=/usr/lib/llvm-18/lib

    rustup toolchain install "$rust" --profile minimal --target "$target"
    rustup toolchain install "$bpf_rust" --profile minimal --component rust-src,llvm-tools
    (
        cd src/crates/honk-ebpf
        cargo "+$bpf_rust" build --locked --release --target bpfel-unknown-none -Z build-std=core
        printf '%s' "$bpf_rust" > "$bpf_object.toolchain"
        llvm-readelf-18 --file-header --sections --wide "$bpf_object" > "$package/honk-ebpf.elf.txt"
        grep -Eq 'Machine:[[:space:]]+(Linux BPF|EM_BPF|BPF)' "$package/honk-ebpf.elf.txt"
        grep -Eq '[[:space:]]\.BTF[[:space:]]' "$package/honk-ebpf.elf.txt"
        install -m 644 "$bpf_object" "$package/honk-ebpf.o"
    )

    # Honk's pinned fetcher verifies both Doona editions and their notices.
    HONK_DOONA_DIR=$(bash src/ci/fetch-doona.sh "$work/doona")
    export HONK_DOONA_DIR
    test -s "$HONK_DOONA_DIR/index.html"
    test -s "$HONK_DOONA_DIR-lite/index.html"
    bash build.sh --locked
    bash build-boxctl.sh --locked
    install -m 755 "src/target/$target/release/honk-core" "$package/honk-core"
    install -m 755 "boxproxy/boxctl/target/$target/release/boxctl" "$package/boxctl"
    check_arm64 "$package/honk-core"
    check_arm64 "$package/boxctl"
    {
        printf 'target=%s\nbpf_rust=%s\nzig=%s\n' "$target" "$bpf_rust" "$zig_version"
        bpf-linker --version
        rustc "+$rust" --version
    } >> "$package/BUILD-INFO"
}

case "$addon" in
    honk)
        repo=https://github.com/Wcoom/honk-android.git
        revision=a515fe1ffaa68a6d75220a578b557d8598a63ccd
        ;;
    oxidns)
        repo=https://github.com/svenshi/oxidns.git
        revision=acc6155beec9c99bb77e3c792a681a84dcb7cb12
        ;;
    *)
        printf 'Unknown Android addon: %s\n' "$addon" >&2
        exit 2
        ;;
esac

sudo apt-get update -qq
sudo apt-get install -y --no-install-recommends build-essential cmake libclang-18-dev llvm-18 pkg-config unzip zstd
patch=$recipe_dir/$addon.patch
git init --quiet "$source_dir"
cd "$source_dir"
git fetch --quiet --depth=1 "$repo" "$revision"
git checkout --quiet --detach FETCH_HEAD
test "$(git rev-parse HEAD)" = "$revision"
git apply --check --index "$patch"
git apply --index "$patch"
patch_digest=$(sha256sum "$patch")
printf 'source=%s\nbase_commit=%s\npatched_tree=%s\npatch_sha256=%s\n' \
    "$repo" "$revision" "$(git write-tree)" "${patch_digest%% *}" > "$package/BUILD-INFO"
cp LICENSE "$package/"

case "$addon" in
    honk) build_honk ;;
    oxidns) build_oxidns ;;
esac

(
    cd "$package"
    sha256sum -- * > SHA256SUMS
    sha256sum --check SHA256SUMS
    tar -czf "$output/$addon.tar.gz" ./*
)
