# Linux build/test environment for the host-side tools (shim cross-compile,
# boot images, QEMU). The XNU kernel itself still builds on GitHub's macOS runners.
#   docker build -t ios-sweet-dev tools/docker
#   docker run --rm -it -v "$PWD:/src" -w /src ios-sweet-dev
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu gcc libc6-dev make \
        qemu-system-arm python3 cpio gzip gawk android-sdk-libsparse-utils \
        device-tree-compiler llvm clang git patch file xxd ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*
# Ubuntu's mkbootimg package lacks its gki module: use AOSP's
RUN mkdir -p /opt/mkbootimg \
    && curl -fsSL https://android.googlesource.com/platform/system/tools/mkbootimg/+archive/refs/heads/main.tar.gz \
       | tar xz -C /opt/mkbootimg \
    && ln -sf /opt/mkbootimg/mkbootimg.py /usr/local/bin/mkbootimg \
    && ln -sf /opt/mkbootimg/unpack_bootimg.py /usr/local/bin/unpack_bootimg
