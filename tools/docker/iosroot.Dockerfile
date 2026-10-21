# iosroot-tools: build/edit the iOS HFS+ root image (tools/mkiosroot.sh) in a KVM guest
# with a stock Ubuntu kernel (the WSL2 kernel has no hfsplus).
#   docker build -t iosroot-tools -f tools/docker/iosroot.Dockerfile tools/docker
FROM ubuntu:24.04
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
      hfsprogs rsync sudo fuse3 libfuse3-dev cmake g++ git zlib1g-dev libbz2-dev libattr1-dev \
      kmod python3 file >/dev/null && rm -rf /var/lib/apt/lists/*
RUN git clone -q --recursive https://github.com/sgan81/apfs-fuse /tmp/apfs-fuse && \
    cmake -S /tmp/apfs-fuse -B /tmp/apfs-fuse/build -DCMAKE_BUILD_TYPE=Release >/dev/null && \
    make -C /tmp/apfs-fuse/build -j"$(nproc)" >/dev/null && \
    install /tmp/apfs-fuse/build/apfs-fuse /tmp/apfs-fuse/build/apfsutil /usr/local/bin/ && \
    rm -rf /tmp/apfs-fuse
# A stock Ubuntu kernel (hfsplus, fuse, 9p as modules) to run the image build in a KVM guest.
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
      linux-image-generic qemu-system-x86 busybox-static zstd cpio >/dev/null && rm -rf /var/lib/apt/lists/*
